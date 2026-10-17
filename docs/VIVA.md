# VIVA prep — PDP (through Stage 3)

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
- **REQ / ACQ**: the two log events around a lock wait. `REQ` is written before the thread asks for the lock, `ACQ` after it gets it. A thread that is stuck has a `REQ` and no `ACQ`.
- **Trylock**: `pthread_mutex_trylock`, which takes the lock if it is free and returns at once otherwise. It never waits, so it cannot be part of a deadlock wait.
- **Held set**: the locks a thread holds at a given moment. The analyzer stores the held set with every edge.
- **Edge (in the lock-order graph)**: an arrow A→B that means "some thread asked for B while holding A". It stores the thread, the call site, the held set and a count.
- **Lock identity / generation counter**: a lock is identified by its address plus a generation number. The generation goes up each time the address is destroyed and reused, so a new lock at an old address is a new node.
- **Recursive mutex**: a mutex the same thread may lock again without blocking (a counter inside the lock). Locking it a second time cannot wait for anyone, so it adds no edge.
- **Elementary cycle**: a cycle in a graph that visits each node at most once (A→B→C→A, but not A→B→A→B→A). Only elementary cycles are searched, so the same loop is not counted twice.
- **Depth-first search (DFS)**: a graph search that follows one path as far as it goes, then backs up and tries the next. Recursion or an explicit stack does it.
- **Three-colour DFS**: the classic cycle check that colours nodes white (not seen), grey (on the current path) and black (finished). A cycle is found when an edge goes to a grey node. Black nodes are never visited again, which is why it can miss cycles that share nodes with one already found. Colours are a standard topic in algorithm courses, so an examiner may ask about them.
- **On-path flag**: a yes/no mark on a node that means "it is on the current path". It is set on entry and cleared on backtrack. Our search uses it in place of grey, and does not keep black.
- **Back edge**: an edge from a node to one of its ancestors on the current DFS path. A back edge is what closes a cycle.
- **Johnson's algorithm**: a standard efficient way to list all elementary cycles of a directed graph. Ours is a simpler version without its blocked-set optimisation.
- **GoodLock**: a deadlock-prediction rule from the literature (introduced by Havelund in 2000, refined by Agarwal and Stoller and in iGoodlock). A cycle of lock-order edges is a potential deadlock only if the threads are distinct and the locks each thread holds are disjoint from the others'.
- **Gate lock**: a lock that two threads of a cycle both hold while they wait. Because only one of them can hold it at a time, they cannot be stuck in the cycle together. Such a cycle is FILTERED.
- **Guard set**: the set of locks that guard an edge, i.e. the held set of that edge. The filter asks whether two guard sets share a lock.
- **False positive**: the tool reports a deadlock that could never happen. A gate lock the tool missed, or a trylock treated as a wait, would cause one.
- **False negative**: the tool misses a deadlock that could happen. Missing data (a lost event), a capped search, or a lock never taken in the log can cause one.
- **Soundness vs completeness**: a sound analysis reports only real problems (no false positives). A complete one finds every problem (no false negatives). Ours aims for completeness within its caps, and it is not sound, because it predicts from one run.
- **UNDECIDED**: our verdict for a cycle whose edge search hit its step cap. We cannot prove it safe, so it counts as a possible deadlock and exits 2.
- **Exit code**: the number a program returns to the shell. `pdp-analyze` returns 0 (nothing found), 2 (a REAL or UNDECIDED cycle), or 1 (the tool failed).
- **Topological sort / Kahn's algorithm**: an ordering of graph nodes where every arrow goes forward. Kahn's method repeatedly outputs a node with no incoming arrows left. A graph with a cycle has no such order, so the leftover nodes are reported as "still cyclic".
- **F(R)**: our fix suggestion. For each real cycle, remove the rarest edge, then give a lock order that respects what is left.
- **`nm` / symbol table**: `nm` lists the names in a binary's symbol table, with each name's address and size. We use `nm -S` to find which static variable sits at a lock's address.
- **`lk=`**: a field libpdp writes on each event: the address of the lock object as `module+0xoff`. It lets the analyzer name the lock.
- **Fixed-size table**: an array with a set maximum (`MAX_NODES`, `MAX_EDGES`). We use them so memory is bounded; overflow is counted and reported.
- **`popen`**: runs a shell command and reads its output. We use it for `addr2line` and `nm`, after checking the arguments so no shell metacharacter can get through.

- **Ground truth**: the answer we already know before running a detector. For the test programs we know by construction whether each can deadlock, so we can check the detector's answers against it.
- **Dining philosophers**: a classic deadlock example. Five philosophers sit around a table with one fork between each pair. Each needs the fork on his left and the fork on his right. If all pick up the left fork at once, each waits forever for the right one.
- **Resource ordering**: a fix for deadlock. Every lock gets a number, and every thread takes its locks in increasing order. Then no cycle of waits can form. `philo_ordered.c` does this with forks.
- **Circular wait**: the Coffman condition where threads form a loop, each waiting for a lock the next one holds. Resource ordering removes it, because a loop would need some lock to be taken in decreasing order.
- **Hold-and-wait**: the Coffman condition where a thread keeps the lock it has while it waits for another. Resource ordering does not remove it; the philosophers still hold one fork while they wait for the second.
- **Binary semaphore**: a semaphore whose counter starts at 1, so it works like a lock: one thread can hold it at a time. `sem_sq.c` uses two of them, S and Q.
- **P and V**: Dijkstra's names for the two semaphore operations. P (from Dutch "proberen", try) is `sem_wait`: it waits until the counter is above zero, then decrements it. V (from "verhogen", increase) is `sem_post`: it increments the counter and wakes a waiter.
- **Barrier (`pthread_barrier_t`)**: a meeting point. Each thread that calls `pthread_barrier_wait` stops until the given number of threads have all arrived; then all continue together. We use one at the start of race and slow modes so all threads begin at the same moment.
- **Thread stagger**: the small time gap between when one thread is created and when the next starts running. Without a barrier, the threads of a test run start one after another, so a short run can finish before the next thread exists. The experiment would then measure thread creation, not lock interleaving.
- **Timeout, exit 124**: the `timeout` command kills a program that runs longer than its limit, and it exits with code 124. Our suite and the experiment use 124 to mean "hung".
- **Flaky test**: a test that passes and fails on different runs of the same code, with no change to the code. Usually it depends on timing or load. Our race checks accept both outcomes for the real-cycle programs, so they cannot flake, and the slow checks are made certain by the barrier and the 100 ms sleep.
- **Mode (`seq`, `race N`, `slow`)**: the three ways each test program runs. `seq` runs threads one after another, `race` runs them concurrently with no delay, and `slow` runs them concurrently with a sleep between the two locks.

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

**16. Why can PDP report a deadlock that never happened in the run?**
Because it predicts from the order the locks were taken, not from what actually happened. If one thread took A then B and another took B then A, the two orders can interleave into a deadlock, even though this run was scheduled so that they never overlapped. That is a potential deadlock. The report says "potential", and Stage 4 is meant to confirm it. The tool can also be wrong the other way (a false negative), for example when the log is capped or a lock was never taken in the run.

**17. Why do trylocks add no edge?**
A trylock returns at once, so it never waits. A cycle is only dangerous if every thread in it is waiting for a lock held by the next one. A trylock cannot be such a wait, so an edge from it would be a false positive. The lock a trylock obtains is still really held, so it does appear in later held sets.

**18. Why do edges come from REQ and not from ACQ?**
In a hung run, the thread that is stuck never gets the lock, so there is no ACQ for it. The REQ is written before the wait, so it is always in the log. Using REQ means a hung run is still analysed, which is the case we care about most.

**19. Why did you drop the three-colour DFS?**
A three-colour DFS reports one cycle per back edge, and it never revisits a black (finished) node. So a second cycle that passes through an already-finished node is never found. The reviewer built a case: A and B form a cycle that is FILTERED (same thread), and a real cycle A to C to B to A is hidden behind it, so the tool printed "no deadlock" with exit 0. Searching for elementary cycles from each start node finds every such cycle, and the caps keep the cost bounded. The price is more work, and the report says when a cap is hit.

**20. What is a gate lock, and why check pairs of threads instead of one common lock?**
A gate lock is a lock that two threads both hold while they wait, so only one of them can be in the critical part at a time, and they cannot deadlock each other. The old rule asked whether one lock was held by all threads of the cycle. That misses cases where two threads share a lock and the third does not, and it wrongly calls them real. Checking every pair of threads, that their held sets are disjoint, is the standard GoodLock condition and catches both.

**21. How do you handle a lock whose memory is freed and reused?**
Address alone does not identify a lock. When a mutex is destroyed and its address is later reused by a new mutex, the analyzer would see one node where there are two locks, and could invent a cycle. A DESTROY event marks the old node dead, and the next use of that address creates a new node with the generation counter increased. The second lock is printed as `#2`.

**22. What does UNDECIDED mean, and why does it exit 2?**
It means the search for a valid combination of edges ran out of its step budget before it finished, so we cannot say the cycle is safe. We treat that as a possible deadlock, because a false alarm is cheaper than a missed deadlock for this tool. Exit 2 tells a script that something needs attention, and 0 would wrongly say "safe".

**23. How is the lock order in the fix suggestion computed?**
For each real cycle we remove one arrow: the rarest edge, because the code that produces a rarely used order is the cheapest to change. Then we sort the remaining locks with Kahn's algorithm, which repeatedly outputs a lock that nothing still has to come before. The removed arrow's target comes first. If a cycle is still left after removal, its locks are printed as "still cyclic", not given an order, because no valid order exists for them.

**24. Is the tool sound or complete?**
Neither, strictly. It is aimed at completeness within its caps: it enumerates all elementary cycles up to length 8, and reports the caps when they cut the search. It is not sound, because a REAL verdict only means a consistent combination of observed edges exists, not that the interleaving will happen. Stage 4 is where a prediction is tested.

**25. Why are the held sets stored on each edge, and not just the lock pairs?**
The filters need them. Whether two threads can be stuck together depends on what each holds, so the held set is the evidence for the gate check. Storing only A to B would lose that and would make a gate lock invisible.

**26. What does the bank demo show, and why does the sequential run not hang?**
`tests/bank.c` has two tellers who lock the two accounts in opposite orders. Run one after the other (we join between them), they can never block each other, so the program always finishes. The analyzer still sees that teller 1 went A then B and teller 2 went B then A, and reports the cycle `acct_a -> acct_b -> acct_a` with exit 2. Run with `slow`, the two tellers overlap and do hang. This is the point of the tool: predict the hang from a run that did not hang.

**27. Why does ordered philosophers never deadlock — which Coffman condition does it break?**
Every philosopher takes the lower-numbered fork first. So a thread that is waiting always waits for a higher-numbered fork than the one it holds. A loop of waits would need the numbers to go up all the way round and come back down to the start, which is impossible. That breaks **circular wait**, the fourth condition. It does not break the other three: forks are still mutually exclusive, a philosopher still holds one fork while waiting for the next (hold-and-wait), and a fork is still released only by its holder. The analyzer agrees: the graph is 0->1, 1->2, 2->3, 3->4, 0->4, with no cycle.

**28. Why did you add a barrier?**
Without it, the threads of a race run start one after another (thread stagger), so with a small N a thread can finish before the next one exists. Then `race` measures how fast `pthread_create` is. The numbers showed it: philo_naive never hung natively at N=1000 (0/20) but always hung under PDP (10/10), because the logging slowed each thread enough that they overlapped. With the barrier, all threads start contending together, so the hang rate depends on the interleaving and N, and the native and PDP runs can be compared fairly. The price is that the alignment is artificial and it raises the hang rate, which the write-up must say.

**29. Does the barrier change what the analyzer sees?**
No. PDP intercepts only the mutex and semaphore calls. `pthread_barrier_wait` is not in the hook list, so it writes no event and makes no edge. The threads hold no lock while they wait at the barrier, so no held set changes either. Checked: `pdp analyze` on `philo_ordered race 3` (with barrier) and `philo_ordered seq` (no barrier) both report the same graph, 5 locks and 5 edges, so the barrier adds nothing.

**30. Why is gate.c FILTERED and not REAL, even though it takes the locks in opposite orders?**
Both threads first take the global mutex `gate_G`. So the cycle `acct_a -> acct_b -> acct_a` is in the graph, but both edges' held sets contain `gate_G`. The GoodLock rule requires the held sets of the chosen edges to be pairwise disjoint, and they are not, so the cycle is FILTERED. The gate also means the program can never hang, which the suite checks in both `slow` and `race` mode.

**31. Why does the sequential run predict a deadlock but never hang?**
In `seq` mode the threads run one after another: each is created and joined before the next starts. So no thread ever waits for a lock another thread holds, and the program always exits 0. But every lock request is still logged, so the lock-order edges (A to B from one thread, B to A from the other) are in the log. The analyzer uses the order of requests, not the timing, so it sees the cycle. This is the basic claim of PDP.

**32. How do you know the test suite is not flaky?**
Each assertion is either certain or accepts all possible outcomes. The `slow` runs must hang (exit 124): the barrier makes all threads start together and the 100 ms sleep makes the circular wait certain. The `race` runs of the real programs accept both 0 and 124, so the scheduler cannot make them fail. The clean programs must exit 0 in every mode, which holds because their lock order (or their gate) makes a cycle impossible, not because of timing. The only timing-sensitive values are the `timeout` limits, which are long compared with the runs.

**33. What is a binary semaphore, and why can sem_sq deadlock like the mutex program?**
A binary semaphore is a counter that starts at 1, so `sem_wait` (P) takes it to 0 and blocks any other `sem_wait` until a `sem_post` (V) raises it again. Used as a lock it gives the same circular wait: P0 holds S and waits for Q, P1 holds Q and waits for S. The difference is that a semaphore has no owner, so any thread may post it. Our analyzer models it as a lock owned by the thread that waited. That is exact here, because each thread posts what it waited for. The Stage 2 limits say that a semaphore used as a signal would break this model.

**34. What does exit code 124 mean, and why does the suite use it?**
124 is what the `timeout` command returns when it kills the program because the time limit ran out. A run that returns 124 has not finished, so we count it as a hang. The suite uses `timeout 2` for the forced `slow` hangs, which must exit 124, and it accepts 124 for race runs of the real programs, but never for the clean ones.
