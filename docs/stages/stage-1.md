# Stage 1: LD_PRELOAD interceptor (`libpdp.so`)

Status: built and tested (`make && make test` passes). Commit `891e236` (WIP). Source: `src/libpdp.c` (~320 lines), `src/pdp.h`.

## What was built

`libpdp.so` is a shared library that is loaded before the real C library when a program is started with
`LD_PRELOAD=./build/libpdp.so`. It replaces six functions with its own versions ("hooks"):

| Hook | What it logs |
|---|---|
| `pthread_mutex_lock` | `REQ` before blocking, then `ACQ` (or `FAIL` with an error code) |
| `pthread_mutex_unlock` | `REL` |
| `pthread_mutex_trylock` | `TRY_OK` or `TRY_FAIL` (never blocks, so no `REQ`) |
| `pthread_mutex_destroy` | `DESTROY` (the address is retired from every thread's held list) |
| `sem_wait` | `REQ` / `ACQ` / `FAIL`, semaphore modelled as a binary lock (kind `S`) |
| `sem_post` | `POST` (removes the semaphore from the poster's held list) |

Each hook does four things: find the calling thread's slot in the thread table, record what the thread
holds, call the real function, and write one log line. The real function is found once at start-up with
`dlsym(RTLD_NEXT, "pthread_mutex_lock")` and so on, so the program still gets real locking behaviour.

**Per-thread table.** A global array `pdp_threads[MAX_THREADS]` (64 slots). Each slot is a `struct tcb`
with `tid` (kernel thread id, 0 = free), `held[MAX_HELD]` (up to 16 lock addresses), `nheld`,
`held_lost` (1 = the list overflowed) and `waiting_on` (the lock the thread is blocked on, or 0). A
thread finds its slot through a `__thread` pointer `me`. It is global, not thread-local, so a later
watchdog (Stage 4) can read every thread's state.

**Spin lock.** `tbl_acquire()` loops on `__sync_lock_test_and_set(&tbl_lock, 1)` (an atomic exchange).
`tbl_release()` calls `__sync_lock_release`. All table reads and writes happen inside this lock. The
critical sections are a few instructions long, so spinning is short.

**Log format** (defined in `src/pdp.h`), one event per line:

```
# pdp v1 pid=4321 exe=/abs/path/tests/bin/basic
0 4321 REQ M 0x55d0c2a0 ./tests/bin/basic+0x12c8 held=-
1 4321 ACQ M 0x55d0c2a0 ./tests/bin/basic+0x12c8 held=-
2 4321 REQ M 0x55d0c2b0 ./tests/bin/basic+0x12e0 held=0x55d0c2a0
```

Fields: `seq` (global order from an atomic counter), `tid` (gettid), `OP`, `kind` (M mutex / S semaphore),
lock address, call site, and `held=` (the locks held before this event, comma-separated). `held=-` means
"holds nothing", `held=?` means "unknown" (the table was full, or the thread held more than 16 locks). The
analyzer must not make edges from a `?` line. Warnings are written as comment lines:
`# pdp WARN overflow tid=<tid> what=threads|held`.

**Call site.** `site_str()` takes the return address, subtracts 1, calls `dladdr()` to find which loaded
module contains it, and prints `module+0xoffset` (offset from the module's load base). Stage 2 can then run
`addr2line -e module 0xoffset` and get the source line, even though ASLR moves the program on every run.

**Thread exit.** `pthread_key_create(&tkey, slot_free)` registers a destructor. When a thread exits, the
destructor clears its slot and sets `tid = 0`. So the 64-slot limit counts threads alive at the same time,
not threads ever created. A thread pool that creates many short-lived threads does not run out of slots.

**Errno.** Each hook saves `errno` on entry and restores it before returning. Our logging (`dladdr`,
`write`) could otherwise change the `errno` the application checks after a failed call (for example
`EINTR` after `sem_wait`).

**Failures.** `end_wait()` logs `ACQ` only if the real call succeeded (or returned `EOWNERDEAD`, a robust
mutex whose lock is owned). Otherwise it logs `FAIL ... err=<code>`. So every `REQ` ends in `ACQ` or
`FAIL`. A `REQ` with neither means the thread was really blocked, which is what a deadlock looks like.

**Re-entry guard.** A thread-local flag `in_hook` is set while a hook is working. A hook called while the
flag is set (from inside our own code, or from a signal handler) skips logging and calls the real function
directly.

**Start-up.** A constructor (`__attribute__((constructor))`) runs `pdp_init()` before `main`. It resolves
the six real functions, creates the key, reads the main program's absolute path from `/proc/self/exe`,
finds its load base with `dl_iterate_phdr`, and opens the log (`$PDP_LOG`, default `pdp.log`, with
`O_APPEND`).

## Why

The analyzer (Stage 2) needs the order in which each thread takes locks. Without changing the program's
source code, the only place to see every lock call is at the library boundary, so we interpose on it.
`LD_PRELOAD` gives this without recompiling. The log is plain text so that every stage can be checked by eye
and by `grep`.

## Design decisions

- **Spin lock, not a pthread mutex, for the table.** A pthread mutex call goes through our own hook
  (`pthread_mutex_lock`), which would take the table lock again and recurse forever. A spin lock is a plain
  atomic instruction and never calls our hooks. The cost is busy waiting, which is acceptable because the
  lock is held for a few instructions. The known weakness is a holder that is preempted while it holds the
  lock; the fix (`pause` or `sched_yield` in the loop) is listed as an open item below.
- **Single `write()` per line.** The line is built on the stack with `snprintf` and sent with one `write()`
  to an `O_APPEND` file. `O_APPEND` makes the offset update and the write atomic, so lines from different
  threads do not interleave. We do not use `stdio` (`printf`, `fprintf`) because it has its own internal
  lock and buffers, which would add more locks to the program we are measuring.
- **`ret - 1`.** The return address is the instruction after the `call`. That instruction can belong to the
  next source line (in the test, `pthread_mutex_lock(&B)` on line 15 was reported as line 16). One byte back
  is still inside the `call`, so `addr2line` names the right line.
- **The lock itself is left out of its own `held` list.** `ACQ` lists only the locks held before this one,
  and `REL` lists what remains after the release. So a recursive mutex, locked twice by the same thread,
  never produces an edge from itself to itself.
- **Unknown is not the same as empty.** `held=?` marks lines where tracking failed. Writing `held=-` there
  would silently remove a lock-order edge. The analyzer must treat `?` as missing data.
- **PIE only.** `dladdr` reports the load base. A PIE program is loaded at a random base and its offsets are
  stable. A non-PIE program (`ET_EXEC`) is loaded at a fixed base (`0x400000`), and `addr2line` expects
  absolute addresses. We chose to require `-fPIE -pie` for test programs (`TESTCF` in the Makefile) rather
  than add a second case. This is written in `pdp.h`.
- **`dlsym(RTLD_NEXT)`.** `RTLD_NEXT` asks the loader for the next definition of the symbol after ours in the
  search order, which is the real glibc function.
- **`/proc/self/exe` for the program path.** glibc reports the main program by its `argv[0]` spelling, which
  can be relative. The analyzer might run from a different directory, so we log the absolute path.

## Review fixes applied

Reviewer report: `docs/reviews/stage-1.md` (3 MUST-FIX, 10 SHOULD-FIX, 9 NIT). Status as of the current code:

| Item | Status |
|---|---|
| M-1 call site points to the next line | Fixed: `ret - 1` in `site_str`, test checks the line |
| M-2 lost edges shown as `held=-` | Fixed: `held=?` marker, WARN lines, slots freed at thread exit |
| M-3 `pdp_init` not guarded against re-entry | Fixed: `if (inited \|\| in_hook) return;`, NULL-pointer checks in hooks |
| S-1 `waiting_on` written without the lock | Fixed: set and cleared inside `tbl_acquire`/`tbl_release` |
| S-2 a failed `REQ` left no trace | Fixed: `FAIL ... err=<code>` event; test asserts `REQ == ACQ + FAIL` |
| S-3 errno not preserved | Fixed: saved and restored in every hook |
| S-5 site offset wrong for non-PIE | Fixed by requirement: PIE only, documented in `pdp.h` and Makefile |
| S-6 relative program path | Fixed: absolute path from `/proc/self/exe` |
| S-10 trylock edge semantics | Partly: `TRY_OK`/`TRY_FAIL` are logged, but `pdp.h` does not yet say that they create no edge |
| S-4 `fork()` not handled | **Open**: no `pthread_atfork`; a child logs under the parent's tid |
| S-7 condvars and timed locks not hooked | Listed as known limits in `pdp.h`; not hooked |
| S-8 semaphore model gives false edges | Partly: `LIMITATION` comment in code; `pdp.h` note for the analyzer missing |
| S-9 test too weak | Partly: the script checks `addr2line` output, `REQ == ACQ + FAIL`, and overflow WARN counts |
| NIT-2, NIT-3 guard on `off` and `len < 0` | Applied |
| NIT-4 spin loop with `pause` / `sched_yield` | **Open** |
| NIT-6, NIT-7 `-fvisibility=hidden`, `initial-exec` TLS | **Open** |
| NIT-8 log name per pid | **Open**: default `pdp.log` with `O_TRUNC` |

## Known limits (from `src/pdp.h`)

- `fork()`: the child shares the parent's tid slot and log file.
- `pthread_cond_wait` / `pthread_cond_timedwait`: glibc drops and re-takes the mutex without calling our
  hooks, so the table still shows the mutex as held while the thread waits.
- `pthread_mutex_timedlock`, `sem_timedwait`, `sem_trywait`: not hooked.
- Semaphores are binary locks. A semaphore used to signal (one thread waits, another posts) leaves the
  waiter's held list wrong.
- `sem_destroy` is not hooked, so semaphore addresses are never retired.
- A freed mutex that was never destroyed (static initializer on a heap object) can have its address reused.
- Programs that are statically linked, setuid, or that clear `LD_PRELOAD` are not seen.

## Explain it in the viva

Our library sits between the program and the C library. When the program calls `pthread_mutex_lock`, the
dynamic linker gives it our version, because we were loaded first (that is what `LD_PRELOAD` does). We write
down that the thread is about to wait for the lock, call the real function to wait, and then write down that
it got the lock. We also keep a small table, one row per thread, with the locks each thread currently
holds. When a thread takes a second lock, we write down the first lock as "held", and that pair is an edge in
the lock-order graph.

The table is protected by a spin lock, not a normal mutex, because a normal mutex would call our own hook and
loop forever. A spin lock is one atomic instruction that does not call anything of ours.

To find where in the source the call happened, we take the return address (where the code continues after the
call), step back one byte so we are inside the call, and ask `dladdr` which file that address belongs to. We
print the file and the offset from where the file was loaded. Because the file is loaded at a random address
each run (ASLR), the offset is the stable part, and `addr2line` turns it into a line number later.

Each log line is built in memory and sent with one `write` to a file opened in append mode, so two threads
cannot tear each other's lines. We save and restore `errno` so the program does not see our side effects. If
the table is full we write `held=?` and not `held=-`, because "unknown" must never look like "holds nothing";
a missing edge would hide a deadlock.
