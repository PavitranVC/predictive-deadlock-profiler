# Stage 1 review — LD_PRELOAD interceptor

Reviewer: reviewer agent. Date: 2026-10-17. Scope: `src/pdp.h`, `src/libpdp.c`, `Makefile`,
`tests/basic.c`, `tests/check_basic.sh`.
Build/test: `make && make test` → PASS (gcc 13.3, glibc 2.39, x86-64).

Extra experiments (scratchpad, no repo changes):
- 70 threads created one after another, each locking A then B → **6 of the 70 `REQ B` lines say
  `held=-`**, so the A→B edge is lost for every thread after the 64th.
- `tests/basic.c` built with `-no-pie` → `addr2line` on the logged site gives `??:0`.
- `REQ` site for `pthread_mutex_lock(&B)` (basic.c:15) resolves to **basic.c:16** (`counter++`);
  `offset-1` resolves to basic.c:15.

Overall: a small, readable design that holds up. Things that are right and worth saying in the viva:
the TLS `in_hook` flag is set *before* any table spin-lock is taken, so a signal handler that locks
a mutex during a hook falls straight through to the real function and cannot self-deadlock on
`tbl_lock`. REL is logged before `real_unlock` and ACQ after `real_lock`, so `seq` order agrees
with happens-before for one lock. Leaving `addr` out of `held` stops recursive mutexes from making
self-edges. The `__sync_lock_test_and_set` / `__sync_lock_release` pair gives acquire and release
semantics, which is correct for a spin lock.

---

## MUST-FIX

1. **Call sites point to the wrong source line.** `src/libpdp.c:154,167,180,193,211,224`, used at `:80-82`.
   `__builtin_return_address(0)` is the instruction *after* the `call`, so addr2line can report
   the next line (confirmed: basic.c:16 instead of :15). Stage 2 reports would then blame the
   wrong line. Fix: format `ret - 1` in `site_str` (the usual trick, also used by gdb and
   backtrace tools), with a WHY comment. Strengthen the test to check the exact line (see S-9).

2. **Lost data is logged as if it were valid.** `src/libpdp.c:45-57` (table full → `t == NULL`),
   `:94` (untracked thread logs `held=-`), `:68` (held entries past MAX_HELD are dropped).
   Slots are never reused, so any program that creates more than 64 threads over its lifetime
   (thread pools, the experiment above) silently loses every lock-order edge after that. This is
   the tool's core output. The analyzer cannot tell "holds nothing" from "we didn't track". Minimum
   fix: write a distinct marker (`held=?`, or a one-time `# pdp warn table-full` /
   `held-overflow` line) and say in pdp.h how Stage 2 must treat it. Better: also free the slot when
   a thread exits (a `pthread_key_create` destructor that clears `tid` under `tbl_lock`). Either is
   a few lines and easy to explain.

3. **The recursion guard does not protect `pdp_init` itself.** `src/libpdp.c:115-117` with
   `:152-153` (and the same pattern in every hook). Each hook calls `pdp_init()` *before* it checks
   `in_hook`, and `pdp_init` checks only `inited`. So if anything inside init (`dlsym`, `getenv`,
   `open`) ever reached a hooked function, it would re-enter `pdp_init`, call `dlsym` again, and
   recurse until the stack overflows. If it did not re-enter, `if (in_hook) return real_lock(m)`
   would call a NULL pointer. Current glibc's `dlsym` only takes internal rtld locks, so this
   doesn't happen today. But the comment claims protection the code doesn't give, and "what if
   dlsym takes a lock?" is a classic viva question. Fix: `if (inited || in_hook) return;` in
   `pdp_init`, and in the hooks, if `real_x == NULL` (only possible while init is still running,
   when the process is single-threaded), return 0 / a safe value, with a comment explaining why.

## SHOULD-FIX

1. **`waiting_on` is written without `tbl_lock`.** `src/libpdp.c:139,145`. A Stage-4 watchdog that
   takes `tbl_lock` to snapshot the table can still race with these plain stores (a C11 data race),
   and can see `waiting_on` and `held[]` from different moments. Do both writes inside
   `tbl_acquire/release` (or use `__atomic_store_n`) so the table is a consistent snapshot of the
   wait-for graph. With that, the struct is enough for Stage 4: owner(lock) = scan `held[]`, and
   edge T→owner comes from `waiting_on`.

2. **A REQ that fails leaves no trace.** `src/libpdp.c:146` (`if (!ok) return;`), `:215`.
   `sem_wait` returning -1/EINTR, or `pthread_mutex_lock` returning EDEADLK/EINVAL, leaves a REQ
   with no ACQ. The analyzer can't tell that apart from "blocked forever at exit". It looks just like
   a real deadlock. Log an `ABORT` (or `REQ_FAIL`) event with the error code. Then `check_basic.sh`
   REQ==ACQ becomes REQ==ACQ+ABORT. Note that robust mutexes return `EOWNERDEAD` and the lock *is*
   acquired, so treat that as ok too, or document that it's out of scope.

3. **errno is not saved and restored.** Every hook runs `snprintf`/`dladdr`/`write` around the real
   call. A common pattern is `if (read(..)<0){ pthread_mutex_unlock(&m); perror(..); }`, and
   `unlock` calls `write()` *before* `real_unlock` (`:170-172`), so any failure in our logging
   (disk full, bad fd) changes the errno the application sees. Save `errno` on entry and restore
   it before returning. For `sem_wait`'s failure path, keep the errno that `real_sem_wait` set.

4. **fork() is not handled.** In the child, `me` still points to the parent's slot, so child
   events are logged under the *parent's* tid into the same file (the line has no pid), which
   corrupts the graph. If another thread held `tbl_lock` at fork time, the child spins forever. Add
   `pthread_atfork` handlers that reset `tbl_lock`, clear the table and `me`, and either reopen
   `pdp.<pid>.log` or add pid to the header of a new file.

5. **Site offset is wrong for non-PIE executables.** `src/libpdp.c:82`. For an `ET_EXEC` binary,
   `dli_fbase` is 0x400000 but addr2line expects the absolute address (confirmed `??:0`). Either
   subtract the base only for `ET_DYN` objects (`dl_iterate_phdr`/`dlinfo` give the load bias
   `l_addr`, which is the right value to subtract for both kinds), or make the Makefile and docs
   require `-pie` and say so in pdp.h.

6. **Site module path is relative (argv[0]).** The log shows `./tests/bin/basic+0x12c8` for the
   main program, because glibc reports the main executable by its invocation name. Run from another
   directory and Stage 2 can't open it. Write the `exe=` header path when `dli_fname` is the main
   program, or document that the analyzer must map it.

7. **condvars, timed locks and `sem_trywait`/`sem_timedwait` are not hooked.** `pthread_cond_wait`
   releases and re-takes the mutex inside glibc without going through our hooks, so during the wait
   the table says the thread still holds the mutex. A Stage-4 watchdog would see two "owners" and
   can report a false deadlock. `pthread_mutex_timedlock` and `sem_timedwait` are blocking
   acquires we never see. At least list these as known gaps in pdp.h. Ideally hook
   `pthread_cond_wait`/`timedwait` (drop `m` before the call, re-add it after) and `timedlock`.

8. **Semaphore model gives Stage 2 false edges.** `src/libpdp.c:206-207,227`. When a semaphore is
   used to signal (consumer waits, producer posts), the consumer keeps `s` in `held` forever. Every
   later lock it takes then makes a bogus `s→X` edge, which can create false cycles. The LIMITATION
   comment is good. Also add a pdp.h note on how the analyzer should treat `kind=S` edges (e.g.
   report them separately or at lower confidence). `sem_destroy` is not hooked, so semaphore
   addresses are never retired.

9. **The test is too weak to catch the bugs above.** `tests/check_basic.sh`. It checks that *some*
   REQ has a non-empty held set, not that `REQ B` has `held=<addr of A>`. It checks only the
   file name from addr2line, not the line. It doesn't test TRY_FAIL, >MAX_THREADS threads, or
   >MAX_HELD nesting. Suggested asserts: the second REQ for each tid has held equal to the first
   REQ's lockaddr; `addr2line` gives `basic.c:15`; a 70-thread case finds no unmarked `held=-`.

10. **trylock edge semantics are not documented for Stage 2.** `src/pdp.h:12`. "held as for ACQ"
    invites the analyzer to add `held→m` edges for TRY_OK, but a trylock cannot block, so it
    cannot close a deadlock cycle (lockdep skips these edges). Say explicitly: TRY_OK adds `m` to
    the held set for *later* edges but creates no edge into `m`. TRY_FAIL also covers EINVAL, not
    just EBUSY. Consider logging `r`.

## NIT

1. `src/libpdp.c:92` `held[MAX_HELD * 20 + 2]`: the 20 is unexplained. Each entry is at most
   "0x"+16 hex digits+"," = 19 bytes. Add the arithmetic as a comment, and the same for `site[256]`,
   `exe[512]`, `hdr[640]`, `PDP_LINE_MAX 768` (sum the worst case once in pdp.h).
2. `src/libpdp.c:102`: `off += snprintf(...)` would make `sizeof held - off` wrap around if output
   were ever truncated. That's safe with today's sizes, but fragile. Clamp `off`.
3. `src/libpdp.c:108-109`: a negative `snprintf` return would be passed to `write` as a huge
   `size_t`. Guard `len < 0`.
4. `src/libpdp.c:40`: plain TAS busy-spin. Test-and-test-and-set plus `__builtin_ia32_pause()` (or
   `sched_yield()` after N spins) is kinder when the holder is preempted, and is a good viva
   talking point. It's fine as is for these short critical sections.
5. Lines in the file can appear out of `seq` order (seq is fetched before `write`). Add to pdp.h:
   "analyzer must sort by seq".
6. `src/libpdp.c:22`: `pdp_threads` is exported from the .so. Fine on purpose for Stage 4, but
   build with `-fvisibility=hidden` and mark the intended exports explicitly.
7. `src/libpdp.c:25-26`: `__thread` under `-fPIC` uses the global-dynamic TLS model. This is fine
   for LD_PRELOAD (static TLS), but `__attribute__((tls_model("initial-exec")))` makes it clear no
   `__tls_get_addr`/malloc can happen inside a hook.
8. `src/libpdp.c:125`: `O_TRUNC` plus a fixed default `pdp.log` means a child process or a second
   run overwrites the first. `pdp.%d.log` by pid avoids that (see S-4).
9. Freed-but-not-destroyed mutexes (heap structs using the static initializer) can reuse addresses
   and merge two different locks in the analyzer. Document this as a limitation.

## Viva-defensibility

Every function is short and has a one-line purpose comment. `self`, `held_*`, `log_event`,
`begin_wait/end_wait` and the six hooks are all easy to explain in plain words. Weak spots a
student should prepare for: why `ret-1` (M-1), why `pdp_init` must also check `in_hook` (M-3), the
truncation line `:108`, and the buffer-size numbers (NIT-1). Once these are fixed and commented,
the whole file is defensible.
