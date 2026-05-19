/* src/compat.h
 * PostgreSQL version compatibility shims for pg_rolequota (PG 14–18+)
 * 2-space indentation per AGENTS.md
 * All #if PG_VERSION_NUM branches must compile cleanly on the CI matrix.
 */

#ifndef PG_ROLEQUOTA_COMPAT_H
#define PG_ROLEQUOTA_COMPAT_H

#include "postgres.h"
#include "fmgr.h"

/* PG_MODULE_MAGIC handling */
#define PG_ROLEQUOTA_MODULE_MAGIC() PG_MODULE_MAGIC
/* (PG 18+ still provides the classic PG_MODULE_MAGIC; the _EXT form with
 * designated initializers (.name = ..., .version = ...) is only needed when
 * supplying optional ABI fields.  The previous positional attempt was the
 * source of the "integer to pointer" error on real PG 18 headers.) */

/* Background worker flags compatibility */
#if PG_VERSION_NUM < 140000
#error "pg_rolequota requires PostgreSQL 14 or later"
#endif

/* Shmem request API changed in PG 15+ (RequestNamedLWLockTranche etc already
 * stable) */
/* Add more shims here as implementation progresses (e.g. for WaitLatch, latch
 * API) */

/* Safe way to register a background worker that works across versions */
#define PG_ROLEQUOTA_REGISTER_WORKER(worker) RegisterBackgroundWorker(worker)

/* WaitLatch compatibility for background workers.
 *
 * The PG_WAIT_* constants (e.g. PG_WAIT_EXTENSION) were moved into
 * utils/wait_classes.h in PostgreSQL 18. On PG 14–17 they are not
 * available as a separate header, so we define our own value (0 is safe
 * and disables wait-event classification, which is acceptable).
 *
 * This allows the extension to compile against postgresql-dev packages
 * on Alpine for PG 14–17.
 */
#if PG_VERSION_NUM >= 180000
#include "utils/wait_classes.h"
#define PG_ROLEQUOTA_WAIT_EVENT_INFO PG_WAIT_EXTENSION
#else
#define PG_ROLEQUOTA_WAIT_EVENT_INFO 0
#endif

#define PG_ROLEQUOTA_WAIT_LATCH(latch, wakeEvents, timeout_ms)                 \
  WaitLatch((latch), (wakeEvents), (timeout_ms), PG_ROLEQUOTA_WAIT_EVENT_INFO)

/* Hard caps on the in-shmem hashes. Both bound peak resource usage (DoS
 * protection) and feed the request-size calculation in shmem.c's request
 * hook. Defined here so scanner_common.c can reference them in user-visible
 * cap-hit warnings without leaking the literal everywhere. */
#define PG_ROLEQUOTA_MAX_ROLE_ENTRIES 16384
#define PG_ROLEQUOTA_MAX_BLOCKED_ENTRIES 4096

/* Maximum number of per-database scanner worker BGWs the launcher will
 * spawn concurrently. This bounds (a) the worker_slots[] table in shmem,
 * (b) the no_ext_dbs[] negative cache, and (c) is also the upper bound for
 * the pg_rolequota.max_workers GUC. 256 covers very large clusters; each
 * slot costs 8 bytes of shmem. */
#define PG_ROLEQUOTA_MAX_WORKERS 256

/* Composite hash key (database OID, role OID). Used by RoleSizeHash,
 * BlockedRolesHash, and the wake-up ring. 8 bytes, naturally aligned, no
 * padding — safe for HASH_BLOBS keying. The struct MUST appear as the
 * first member of any entry type the dynahash code keys on. */
typedef struct RoleQuotaKey {
  Oid dbid;
  Oid roleid;
} RoleQuotaKey;

/* Low-latency wake-up queue (lock-free SPSC ring of roleids the BGW must
 * refresh on its next wake). Power of 2 so we can mask the cursor with
 * `& (SIZE-1)` instead of doing a modulo. 256 slots × 4 bytes = ~1 KiB.
 * Overflow is non-fatal — see shmem.c's enqueue path: drops bump a counter
 * which the BGW reconciles by running a single full scan. */
#define PG_ROLEQUOTA_WAKEUP_QUEUE_SIZE 256
#define PG_ROLEQUOTA_WAKEUP_QUEUE_MASK (PG_ROLEQUOTA_WAKEUP_QUEUE_SIZE - 1)
#define PG_ROLEQUOTA_INVALID_PGPROCNO ((uint32)(-1))

/* Stable accessor for "this PGPROC's proc number" / "PGPROC pointer from a
 * proc number". In PG 18 these are the canonical macros GetNumberFromPGProc
 * and GetPGProcByNumber. In earlier versions PGPROC has a plain int field
 * called `pgprocno`. We wrap both behind a single name. */
#if PG_VERSION_NUM >= 170000
#define PG_ROLEQUOTA_PGPROC_NUMBER(proc) ((uint32)GetNumberFromPGProc(proc))
#define PG_ROLEQUOTA_PGPROC_BY_NUMBER(n) (GetPGProcByNumber(n))
#else
#define PG_ROLEQUOTA_PGPROC_NUMBER(proc) ((uint32)(proc)->pgprocno)
#define PG_ROLEQUOTA_PGPROC_BY_NUMBER(n) (&ProcGlobal->allProcs[(n)])
#endif

#endif /* PG_ROLEQUOTA_COMPAT_H */
