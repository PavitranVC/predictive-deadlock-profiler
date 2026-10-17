/* pdp.h - constants and log format shared by libpdp.so and (later) the analyzer.
 *
 * LOG FORMAT (v1), one event per line, space separated:
 *   seq tid OP kind lockaddr site held=addr1,addr2|-
 * First line is a header:  # pdp v1 pid=<pid> exe=<path>
 *
 *   seq      global sequence number (total order of events, from an atomic counter)
 *   tid      kernel thread id (gettid)
 *   OP       REQ      about to block on a lock (held = locks already held)
 *            ACQ      lock obtained (held = locks held BEFORE this one -> graph edges)
 *            REL      lock released (held = locks still held afterwards)
 *            TRY_OK / TRY_FAIL   trylock outcome (held as for ACQ / REQ)
 *            DESTROY  mutex destroyed (address is retired)
 *            POST     sem_post
 *   kind     M = pthread mutex, S = semaphore (modelled as a binary lock)
 *   lockaddr hex address of the mutex/semaphore, e.g. 0x5581a0c3
 *   site     caller as module+0xoffset (feed to: addr2line -e module 0xoffset)
 *   held     comma-separated hex addresses of locks this thread holds, or "-"
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
#define PDP_LINE_MAX  768         /* stack buffer for one log line */

#endif
