/* pdp.h - constants and log format shared by libpdp.so and (later) the analyzer.
 *
 * LOG FORMAT (v1), one event per line, space separated:
 *   seq tid OP kind lockaddr site held=addr1,addr2|- [err=<code>] lk=module+0xoff|?
 * First line is a header:  # pdp v1 pid=<pid> exe=<path>
 *
 *   seq      global sequence number (total order of events, from an atomic counter)
 *   tid      kernel thread id (gettid)
 *   OP       REQ      about to block on a lock (held = locks already held)
 *            ACQ      lock obtained (held = locks held BEFORE this one -> graph edges)
 *            REL      lock released (held = locks still held afterwards)
 *            FAIL     the real lock/sem_wait returned an error; line ends " err=<code>".
 *                     Pairs with the earlier REQ (so REQ == ACQ + FAIL for finished waits).
 *            TRY_OK / TRY_FAIL   trylock outcome (held as for ACQ / REQ)
 *            DESTROY  mutex destroyed (address is retired)
 *            POST     sem_post
 *   kind     M = pthread mutex, S = semaphore (modelled as a binary lock)
 *   lockaddr hex address of the mutex/semaphore, e.g. 0x5581a0c3
 *   site     caller as module+0xoffset (feed to: addr2line -e module 0xoffset)
 *   held     comma-separated hex addresses of locks this thread holds, or
 *            "-" = holds nothing (reliable), "?" = UNKNOWN (thread table full, or
 *            this thread held more than MAX_HELD locks). The analyzer must NOT
 *            create edges from a "?" line; treat it as missing data.
 *   lk       always the last field: where the lock object itself lives, as
 *            module+0xoffset (a global/static in the exe or a library) or "?" for
 *            heap/stack locks. The analyzer feeds it to nm to name the variable.
 *            Hand-written logs may omit it (treated as "?").
 *
 * Extra lines (start with '#', analyzer skips them but should report them):
 *   # pdp WARN overflow tid=<tid> what=threads|held    (once per thread and kind)
 * A thread's table slot is freed when it exits, so "threads" means more than
 * MAX_THREADS threads were alive AT THE SAME TIME.
 * Lines may appear slightly out of seq order; the analyzer must sort by seq.
 *
 * Site offsets: site = (return address - 1) - load base of the module, so
 * addr2line names the line of the call. The test executables are built PIE
 * (-fPIE -pie); a non-PIE executable would need absolute addresses (its load
 * base is 0x400000, not subtracted) and is NOT supported. The main program's
 * path is logged absolute (from /proc/self/exe).
 *
 * KNOWN LIMITS (not handled): fork() (child shares the parent's tid slot and log
 * fd), pthread_cond_wait/timedwait (mutex looks held while waiting),
 * pthread_mutex_timedlock, sem_timedwait/sem_trywait.
 *
 * Paths containing spaces are not supported in the exe=/site fields.
 */
#ifndef PDP_H
#define PDP_H

#define PDP_VERSION   1
#define MAX_THREADS   64          /* size of the global thread table */
#define MAX_HELD      16          /* max locks tracked per thread */
#define PDP_LOG_ENV   "PDP_LOG"
#define PDP_LOG_DEFAULT "pdp.log"
#define PDP_LINE_MAX  1536        /* stack buffer for one log line */

#endif
