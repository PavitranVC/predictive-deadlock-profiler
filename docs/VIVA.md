# VIVA prep — PDP (through Stage 1)

Plain-English glossary and likely examiner questions. Each entry is written so you can say it out loud.

## Glossary

- **Shared object (`.so`)**: a compiled library the dynamic linker loads into a running program at start-up. `libpdp.so` is one.
- **`LD_PRELOAD`**: an environment variable naming libraries the dynamic linker loads *before* the usual ones. If our library defines `pthread_mutex_lock`, the program calls ours instead of glibc's, without any change to its source or binary.
- **Interposition**: replacing a library function with your own version of the same name, so callers reach your code first. `LD_PRELOAD` is how we do it.
- **`dlsym`**: asks the dynamic linker for the address of a function by name at run time.
- **`RTLD_NEXT`**: a flag to `dlsym` meaning "the next definition of this name after the caller's library in the search order". Our hook uses it to find the real glibc function it replaces.
- **Constructor (`__attribute__((constructor))`)**: a function the loader runs automatically before `main`. We use it to set up the real-function pointers, the log file and the thread key.
- **Mutex**: a lock that only one thread can hold at a time. `pthread_mutex_lock` waits until it is free.
- **Semaphore**: a counter-based signal. `sem_wait` decrements it (waits if zero), `sem_post` increments it. We model it as a binary lock, which is an approximation.
- **Futex**: a Linux system call that lets a thread sleep until a memory word changes. glibc's mutexes use it so a waiting thread sleeps instead of spinning. We do not use futexes ourselves.
- **Test-and-set (TestAndSet)**: an atomic instruction that writes 1 to a memory word and returns the old value, in one indivisible step. `__sync_lock_test_and_set` is GCC's version.
- **Spin lock**: a lock that a thread gets by looping on test-and-set until it succeeds. Our table lock is one.
- **Busy waiting**: a thread using CPU while it waits for something, instead of sleeping. A spin lock does this. It is fine for very short waits.
- **Atomic write / `O_APPEND`**: opening a file with `O_APPEND` makes each `write` go to the end of the file as one step, so two threads' lines do not interleave.
- **Thread-local storage (`__thread`)**: a variable with a separate copy for each thread. We use it for `in_hook` and the `me` pointer.
- **TCB (thread control block)**: a record that holds one thread's state. Ours is `struct tcb`: thread id, held locks, what it is waiting for.
- **`gettid` vs `pthread_self`**: `gettid` returns the kernel's thread id, the number the scheduler and `top` show. `pthread_self` returns glibc's handle for the thread, a pointer-sized value in the program's memory. We log `gettid`, because the analyzer and the OS scheduler both use that number.
- **Return address**: the address in the code where a function returns to, i.e. the instruction after the `call`. We take it with `__builtin_return_address(0)`.
- **PIE / ASLR**: a Position-Independent Executable is loaded at a random address each run (ASLR, address space layout randomization). The offset from the load base is stable, which is why we log offsets.
- **`dladdr`**: given an address in code, returns which loaded file contains it and the file's base address.
- **`addr2line`**: a tool that turns an offset in a binary into a source file and line number (it needs debug info, `-g`).
- **`errno`**: a per-thread variable where C library calls store an error code. Our hooks save and restore it so logging does not change what the program sees.
- **`EINTR`**: "interrupted by a signal". `sem_wait` can return it. We log it as `FAIL ... err=4`.
- **`EOWNERDEAD`**: a robust mutex's owner died; the lock is handed over and is owned by the caller.
- **`pthread_key` destructor**: a function registered with `pthread_key_create` that runs when a thread exits. We use it to free the thread's table slot.
- **Deadlock**: a set of threads each waiting for a lock held by another thread in the set, so none can move.
- **Coffman conditions**: the four conditions that must all hold for a deadlock: mutual exclusion (a lock is held by one thread), hold-and-wait (a thread holds one lock while waiting for another), no preemption (a lock is only released by its holder), and circular wait (a cycle of threads each waiting for the next).
- **Lock-order graph**: a graph with one node per lock and an edge A→B whenever some thread takes B while holding A. A cycle in this graph (A→B and B→A) means a deadlock is possible, even if this run did not hang.
- **Setuid / static binaries ignore `LD_PRELOAD`**: for setuid programs the loader ignores `LD_PRELOAD` (security). A statically linked program has its C library inside the binary, so there is no dynamic symbol to replace.

## Likely examiner questions

**1. Why not use a pthread mutex to guard the thread table?**
Our table lock is taken inside our own `pthread_mutex_lock` hook. Using a pthread mutex there would call the hook again, which would try the same lock and recurse forever. A spin lock is a single atomic instruction and never calls our hooks.

**2. What if the program is statically linked?**
`LD_PRELOAD` only replaces functions that the dynamic linker resolves at run time. A static binary has its own copy of the lock functions, so we see nothing. We state this as a limit; the fix would be to relink with `--wrap` or to instrument at build time.

**3. How do you avoid infinite recursion?**
A thread-local flag `in_hook` is set on entry to a hook. If a hook is entered again while the flag is set (from our own code or from a signal handler), it skips logging and calls the real function. `pdp_init` checks the same flag, so setup code that calls a hook cannot start setup again.

**4. What is the overhead, and where does it come from?**
Each lock and unlock does: a table lookup, a spin lock acquire and release, a `dladdr` call (symbol lookup, not cheap), `snprintf` to build the line, and one `write` system call. The `write` is the largest cost. Logging also changes timing, so the bug may disappear under the tool (a "heisenbug"). We will measure this in the experiments.

**5. Why spin instead of sleeping on the table lock?**
The critical section is a few instructions, so the wait is very short. Sleeping needs a system call, which is slower and needs a lock of its own. The known risk is a holder that gets preempted; a `pause` instruction or `sched_yield()` in the loop would reduce it. That is still open.

**6. Why one `write()` per line, and not `printf`?**
One `write` to an `O_APPEND` file is atomic, so lines from different threads do not mix. `printf` uses a lock and a buffer inside stdio. That lock would be another lock taken inside the hook, which is exactly the problem we avoid.

**7. Why subtract 1 from the return address?**
The return address points to the instruction after the `call`. That instruction can belong to the next source line, so `addr2line` would report the wrong line. One byte back is still inside the `call`. Before the fix, our test reported line 16 for a call on line 15.

**8. Why only PIE executables?**
We log an offset from the load base, which is stable under ASLR. For a non-PIE program the base is always `0x400000` and `addr2line` wants absolute addresses. Supporting both would need a second code path, so we require `-fPIE -pie` for test programs and document it in `pdp.h`.

**9. How do you know which lock a thread holds?**
Each thread has a slot in a global table. `ACQ` adds the lock to the slot's `held` list, `REL` removes it. Every event writes the list as it was before the event. If the list overflows (more than 16 locks) or the table is full (more than 64 live threads), we write `held=?`, so the analyzer knows the data is missing.

**10. What happens when there are more than 64 threads?**
Slots are freed when a thread exits, so the limit is on threads alive at the same time, not total threads. If the table is still full, the thread gets no slot: its lock events are logged with `held=?`, and a `# pdp WARN overflow ... what=threads` line is written once for that thread.

**11. Why log `FAIL` events?**
If `pthread_mutex_lock` or `sem_wait` returns an error, the thread never got the lock. Without a `FAIL` line, its `REQ` would look like a thread blocked forever, which is what a real deadlock looks like. `FAIL ... err=<code>` explains it.

**12. Why save and restore `errno`?**
Our logging calls `write` and `dladdr`, and they can change `errno`. A program often checks `errno` after a call like `sem_wait` (for example, to retry on `EINTR`). If our code changed it, the program would behave differently only when the tool is used.

**13. How does a lock-order cycle give a deadlock prediction from one run?**
If one thread takes A then B, and another takes B then A, the graph has A→B and B→A. Those two orders could interleave into a deadlock, even if this run happened to schedule them one after the other and did not hang. That is why we predict from the graph and confirm later with the confirmer stage.

**14. Why is the `seq` number needed? Can't we use the file order?**
Two threads can call `write` in a different order from their `seq` values, because `seq` is taken before the write. The analyzer sorts by `seq`, which is the global order of events that the atomic counter gives.

**15. What does a lock that is freed but never destroyed do?**
Its address can be reused for a new lock, and the analyzer could merge two different locks into one node. `pthread_mutex_destroy` retires the address. Heap-allocated static-initialized mutexes that are freed without destroy are a documented limit.
