/* src/pg_rolequota.c
 * Main entry point, GUCs, launcher registration, and hook installation for
 * pg_rolequota.
 *
 * Multi-database architecture (production fix):
 *   - The statically-registered BGW is the LAUNCHER. It connects to
 *     `template1` (always present, never dropped) and periodically
 *     enumerates `pg_database` to find databases with the extension
 *     installed.
 *   - For each such database, the launcher spawns a per-database WORKER
 *     via RegisterDynamicBackgroundWorker. The worker connects by OID,
 *     probes pg_extension, and runs the scan loop scoped to its database.
 *   - The composite (dbid, roleid) hash key + per-DB worker latch table
 *     (defined in shmem.c) keep different databases' state isolated.
 *
 * 2-space indentation per AGENTS.md.
 */

#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "postmaster/bgworker.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "utils/errcodes.h"
#include "access/xact.h" /* AbortOutOfAnyTransaction */
#include "storage/ipc.h" /* before_shmem_exit for latch unpublish */
#include "executor/spi.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "compat.h"

/* Forward declarations for modules (in real tree we would have
 * src/include/pg_rolequota.h) */
void pg_rolequota_register_shmem_hooks(void);
void pg_rolequota_install_hooks(void);
void pg_rolequota_scan_shared(void); /* shared_hosting path */
void pg_rolequota_scan_enterprise(
    void); /* enterprise_db authoritative fs-walk path */

/* Wake-up counter accessor + low-latency queue helpers (from shmem.c). */
int32 pg_rolequota_get_wakeup_counter(void);
void pg_rolequota_publish_worker_latch(Oid dbid);
void pg_rolequota_unpublish_worker_latch(Oid dbid);
bool pg_rolequota_dequeue_wakeup_role(RoleQuotaKey *out_key);
void pg_rolequota_enqueue_wakeup_role(Oid dbid, Oid roleid);
uint32 pg_rolequota_consume_overflow(void);
bool pg_rolequota_db_known_no_ext(Oid dbid);
void pg_rolequota_mark_db_no_ext(Oid dbid);
void pg_rolequota_bump_no_ext_epoch(void);
bool pg_rolequota_worker_present_for(Oid dbid);
bool pg_rolequota_reserve_worker_slot(Oid dbid);
void pg_rolequota_unpublish_worker_latch(Oid dbid);
void pg_rolequota_publish_launcher_latch(void);
void pg_rolequota_unpublish_launcher_latch(void);

/* Per-role targeted scanner (from scanner_common.c). */
void pg_rolequota_scan_single_role(Oid dbid, Oid roleid);

/* Extern declarations for GUCs shared with other .c files (scanners, slack).
 * Satisfies -Wmissing-variable-declarations while keeping them visible.
 */
extern int pg_rolequota_max_roles_tracked;
extern char *pg_rolequota_slack_webhook_url;
extern bool pg_rolequota_cancel_enabled;
extern int pg_rolequota_cancel_grace_bytes;

/* GUC variables */
static int pg_rolequota_scanner_mode =
    0; /* 0=shared_hosting, 1=enterprise_db */
static int pg_rolequota_scan_interval =
    300; /* seconds, SIGHUP. Wakes are event-driven (latch + queue);
            the timer is only a periodic reconciliation tick. */
static int pg_rolequota_launcher_interval =
    60; /* seconds, SIGHUP. How often the launcher re-enumerates pg_database
            looking for new databases that have CREATE EXTENSION pg_rolequota
            run since last cycle. Workers are event-driven once spawned. */
static int pg_rolequota_max_workers =
    32; /* PGC_POSTMASTER. Bounds concurrent per-DB worker BGWs (and the
            negative-cache size). Max value is PG_ROLEQUOTA_MAX_WORKERS. */
int pg_rolequota_max_roles_tracked =
    16384; /* non-static for scanner caps (use-guc-max polish) */
char *pg_rolequota_slack_webhook_url =
    NULL; /* non-static for slack.c access; SIGHUP-reloadable */
bool pg_rolequota_cancel_enabled = true; /* SIGHUP-reloadable; controls
                                            mid-query pg_cancel_backend()
                                            after the worker detects a breach.
                                            Set to false to fall back to
                                            detect-then-block-on-next-statement
                                            semantics. */
int pg_rolequota_cancel_grace_bytes =
    0; /* SIGHUP-reloadable. Bytes of slack added to hard_bytes before
          cancellation fires — absorbs short-lived bursts. Default 0 = no
          slack. Range 0..1e9. */

static const struct config_enum_entry pg_rolequota_scanner_mode_options[] = {
    {"shared_hosting", 0, false},
    {"enterprise_db", 1, false},
    {NULL, 0, false}};

/*
 * EXEC_BACKEND-safe shmem accessor helper (Round 2 fix).
 * All code paths that touch RoleSizeHash / BlockedRolesHash / RoleSizeLock
 * must call this first. It is the canonical way to obtain a valid view of
 * the shared anchor from any backend, including exec children.
 */
void pg_rolequota_ensure_shmem(void);

/* Worker / launcher entry points (declared here so RegisterBackgroundWorker
 * + RegisterDynamicBackgroundWorker can name them by string). */
PGDLLEXPORT void pg_rolequota_launcher_main(Datum main_arg);
PGDLLEXPORT void pg_rolequota_worker_main(Datum main_arg);

/* Use the compat macro so PG18+ extended magic is used automatically */
PG_ROLEQUOTA_MODULE_MAGIC();

/*
 * _PG_init
 * Called at server start (shared_preload_libraries). Defines GUCs,
 * registers shmem hooks, installs enforcement hooks, and registers the
 * launcher BGW.
 */
void _PG_init(void) {
  if (process_shared_preload_libraries_in_progress) {
    /* C89 / PG -Wdeclaration-after-statement: decl at top of block */
    BackgroundWorker worker;

    elog(LOG, "pg_rolequota: _PG_init (shared_preload_libraries phase) - "
              "multi-database launcher active");

    /* GUCs first (PGC_POSTMASTER for scanner_mode/max/max_workers must be
     * defined before shmem registration so that the request hook can
     * observe them if needed; scan_interval + launcher_interval are
     * SIGHUP-reloadable). */
    DefineCustomEnumVariable(
        "pg_rolequota.scanner_mode",
        "Scanner implementation used by per-database worker BGWs", NULL,
        &pg_rolequota_scanner_mode, 0 /* shared_hosting */,
        pg_rolequota_scanner_mode_options, PGC_POSTMASTER, 0, NULL, NULL, NULL);

    DefineCustomIntVariable(
        "pg_rolequota.scan_interval",
        "Interval (seconds) between background quota reconciliation scans. "
        "Event-driven wake-ups (rolequota.request_wakeup) reach the worker "
        "in <10 ms regardless of this value; the timer is a safety net.",
        NULL, &pg_rolequota_scan_interval, 300, 5, 3600, PGC_SIGHUP, 0, NULL,
        NULL, NULL);

    DefineCustomIntVariable(
        "pg_rolequota.launcher_interval",
        "Interval (seconds) at which the launcher BGW re-enumerates "
        "pg_database to discover newly-installed instances of the "
        "extension. Workers are spawned dynamically per database.",
        NULL, &pg_rolequota_launcher_interval, 60, 5, 3600, PGC_SIGHUP, 0, NULL,
        NULL, NULL);

    DefineCustomIntVariable(
        "pg_rolequota.max_workers",
        "Maximum number of concurrent per-database worker BGWs. Each "
        "worker tracks one database. Hard cap is PG_ROLEQUOTA_MAX_WORKERS "
        "(256). Increase if you have many databases with the extension "
        "installed; ensure max_worker_processes accommodates this + the "
        "launcher.",
        NULL, &pg_rolequota_max_workers, 32, 1, PG_ROLEQUOTA_MAX_WORKERS,
        PGC_POSTMASTER, 0, NULL, NULL, NULL);

    DefineCustomIntVariable("pg_rolequota.max_roles_tracked",
                            "Maximum number of (database,role) pairs tracked "
                            "in shared memory",
                            NULL, &pg_rolequota_max_roles_tracked, 16384, 128,
                            16384, PGC_POSTMASTER, 0, NULL, NULL, NULL);

    DefineCustomStringVariable(
        "pg_rolequota.slack_webhook_url",
        "Webhook URL for optional Slack notifications on hard quota breaches "
        "(must start with https://hooks.slack.com/)",
        NULL, &pg_rolequota_slack_webhook_url, NULL, PGC_SIGHUP, 0, NULL, NULL,
        NULL);

    DefineCustomBoolVariable(
        "pg_rolequota.cancel_enabled",
        "If true (default), the worker BGW issues pg_cancel_backend() to "
        "every active session owned by a role that is over its hard limit "
        "after each scan. Set to false to revert to detect-then-block-on-"
        "next-statement semantics (the v0.x behaviour). Superuser-settable "
        "in-session for testing and operational overrides.",
        NULL, &pg_rolequota_cancel_enabled, true, PGC_SUSET, 0, NULL, NULL,
        NULL);

    DefineCustomIntVariable(
        "pg_rolequota.cancel_grace_bytes",
        "Bytes of slack added to hard_bytes before mid-query cancellation "
        "fires. Lets operators absorb short-lived bursts (e.g. a single "
        "large transaction that briefly overshoots) without killing the "
        "session. Default 0 means a hard wall at hard_bytes. Superuser-"
        "settable in-session.",
        NULL, &pg_rolequota_cancel_grace_bytes, 0, 0, 1000000000, PGC_SUSET, 0,
        NULL, NULL, NULL);

    /* shmem hooks: anchor + composite-key hashes + per-DB worker latch table */
    pg_rolequota_register_shmem_hooks();

    /* Phase 4: install enforcement hooks (chaining pattern) */
    pg_rolequota_install_hooks();

    /* Register the statically-defined LAUNCHER BGW. It connects to
     * template1 and spawns per-database workers via
     * RegisterDynamicBackgroundWorker. */
    memset(&worker, 0, sizeof(worker));
    worker.bgw_flags =
        BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    /* RecoveryFinished — wait until the cluster is fully open before
     * touching catalogs (pg_total_relation_size can NULL-deref on
     * relations without primed relcache state during recovery). */
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 60; /* auto-restart on crash */
    snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_rolequota");
    snprintf(worker.bgw_function_name, BGW_MAXLEN,
             "pg_rolequota_launcher_main");
    snprintf(worker.bgw_name, BGW_MAXLEN, "pg_rolequota launcher");
    snprintf(worker.bgw_type, BGW_MAXLEN, "pg_rolequota launcher");
    RegisterBackgroundWorker(&worker);

    elog(LOG, "pg_rolequota: GUCs + hooks + launcher BGW registered");
  } else {
    /* Addresses Issue 7 (inert mode): extension is useless without
     * shared_preload_libraries. Emit a loud WARNING so operators notice
     * the misconfiguration immediately. */
    elog(WARNING, "pg_rolequota: loaded via CREATE EXTENSION without "
                  "shared_preload_libraries. "
                  "Hooks and background workers are inactive. Enforcement will "
                  "do nothing. "
                  "Add 'pg_rolequota' to shared_preload_libraries and restart "
                  "the server.");
  }
}

/*
 * Simple C function exposed for SQL to call (proves MODULE_PATHNAME linkage).
 */
Datum pg_rolequota_c_linkage_test(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pg_rolequota_c_linkage_test);

Datum pg_rolequota_c_linkage_test(PG_FUNCTION_ARGS) {
  PG_RETURN_INT32(42);
}

/* ----------------------------------------------------------------
 * Launcher BGW
 * ----------------------------------------------------------------
 * Statically registered; connects to template1; loops every
 * launcher_interval seconds, enumerates pg_database, and spawns one
 * dynamic worker per candidate database that doesn't already have one.
 */
static volatile sig_atomic_t bgw_got_sigterm = false;

static void pg_rolequota_bgw_sigterm(SIGNAL_ARGS) {
  int save_errno = errno;
  bgw_got_sigterm = true;
  if (MyProc)
    SetLatch(&MyProc->procLatch);
  errno = save_errno;
}

/* before_shmem_exit callback used by workers: clear the per-DB latch slot
 * so producers don't try to SetLatch on a dead procLatch. */
static void pg_rolequota_worker_unpublish_exit_cb(int code, Datum arg) {
  Oid dbid = DatumGetObjectId(arg);
  (void)code;
  pg_rolequota_unpublish_worker_latch(dbid);
}

/* before_shmem_exit callback for the launcher BGW: clear the published
 * launcher latch so producers don't try to SetLatch on a dead procLatch
 * during the auto-restart window (`bgw_restart_time = 60`). */
static void pg_rolequota_launcher_unpublish_exit_cb(int code, Datum arg) {
  (void)code;
  (void)arg;
  pg_rolequota_unpublish_launcher_latch();
}

PGDLLEXPORT void pg_rolequota_launcher_main(Datum main_arg) {
  (void)main_arg;

  elog(LOG, "pg_rolequota launcher: starting (connects to postgres)");

  /* We connect to the standard 'postgres' maintenance database (always
   * present in a fresh cluster and rarely dropped). NOT template1 —
   * holding a connection to template1 prevents CREATE DATABASE rq_a
   * from using it as a source. The launcher's only DB-bound work is
   * running SPI against pg_database; it does not touch user data in
   * the connected DB. */
  BackgroundWorkerInitializeConnection("postgres", NULL, 0);

  pqsignal(SIGTERM, pg_rolequota_bgw_sigterm);
  BackgroundWorkerUnblockSignals();

  /* Bump the negative-cache epoch so any stale entries from a previous
   * postmaster lifetime are invalidated. Publish our latch so that any
   * backend's request_wakeup() in a DB without a worker can SetLatch us
   * to ask for an immediate enumeration cycle. */
  pg_rolequota_ensure_shmem();
  pg_rolequota_bump_no_ext_epoch();
  pg_rolequota_publish_launcher_latch();
  before_shmem_exit(pg_rolequota_launcher_unpublish_exit_cb, (Datum)0);

  elog(LOG, "pg_rolequota launcher: entering main loop (interval=%d sec)",
       pg_rolequota_launcher_interval);

  /* `first_pass` runs the enumeration immediately on startup so workers
   * for already-present extension databases come up within milliseconds
   * (predictable latency for users who do their first request_wakeup()
   * right after the cluster comes online). After the first pass we fall
   * into the regular WaitLatch loop. */
  {
    bool first_pass = true;
    int wl = 0;
    bool tick = false;

    while (!bgw_got_sigterm) {
      if (!first_pass) {
        wl = PG_ROLEQUOTA_WAIT_LATCH(
            &MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
            (long)pg_rolequota_launcher_interval * 1000L);
        ResetLatch(&MyProc->procLatch);

        if (wl & WL_POSTMASTER_DEATH)
          break;

        CHECK_FOR_INTERRUPTS();

        /* Re-enumerate on EITHER timer fire OR latch wake. The latch
         * is set by producers (request_wakeup) in databases that don't
         * yet have a worker — we need to spawn one ASAP. */
        tick = ((wl & (WL_TIMEOUT | WL_LATCH_SET)) != 0);
        if (!tick && !bgw_got_sigterm)
          continue;
        (void)tick;
      }
      first_pass = false;

      /* Enumerate pg_database. Connect SPI, take a snapshot, iterate, and
       * for each candidate database that is not already covered by a worker
       * AND not on the negative-cache, register a dynamic worker. */
      PG_TRY();
      {
        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();
        if (SPI_connect() == SPI_OK_CONNECT) {
          int ret;
          PushActiveSnapshot(GetTransactionSnapshot());
          ret = SPI_execute("SELECT oid FROM pg_database "
                            "WHERE datallowconn AND NOT datistemplate "
                            "  AND datname <> 'template0' "
                            "  AND datconnlimit <> -2 "
                            "ORDER BY oid",
                            true /* read-only */, 0);
          if (ret == SPI_OK_SELECT) {
            int spawned = 0;
            int skipped_cap = 0;
            uint64 nrows = SPI_processed;
            TupleDesc td = SPI_tuptable->tupdesc;
            for (uint64 r = 0; r < nrows && !bgw_got_sigterm; r++) {
              HeapTuple tup = SPI_tuptable->vals[r];
              bool isnull;
              Datum d = SPI_getbinval(tup, td, 1, &isnull);
              Oid dbid;
              BackgroundWorker bw;
              BackgroundWorkerHandle *handle;

              if (isnull)
                continue;
              dbid = DatumGetObjectId(d);

              if (pg_rolequota_db_known_no_ext(dbid))
                continue;

              if (spawned >= pg_rolequota_max_workers) {
                skipped_cap++;
                continue;
              }

              /* Atomically reserve a worker slot for this dbid. If a slot
               * already exists (because a previously-spawned worker is
               * either running or still in startup), reserve_worker_slot
               * returns false and we skip. Closes the race between
               * RegisterDynamicBackgroundWorker and the worker's own
               * publish_worker_latch call. */
              if (!pg_rolequota_reserve_worker_slot(dbid))
                continue;

              memset(&bw, 0, sizeof(bw));
              bw.bgw_flags =
                  BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
              bw.bgw_start_time = BgWorkerStart_RecoveryFinished;
              bw.bgw_restart_time = BGW_NEVER_RESTART;
              snprintf(bw.bgw_library_name, BGW_MAXLEN, "pg_rolequota");
              snprintf(bw.bgw_function_name, BGW_MAXLEN,
                       "pg_rolequota_worker_main");
              snprintf(bw.bgw_name, BGW_MAXLEN, "pg_rolequota worker [%u]",
                       dbid);
              snprintf(bw.bgw_type, BGW_MAXLEN, "pg_rolequota worker");
              bw.bgw_main_arg = ObjectIdGetDatum(dbid);
              /* Do NOT set bgw_notify_pid: we don't want the postmaster
               * to send us SIGUSR1 every time a worker exits, which
               * would re-trigger enumeration in a tight loop on any
               * batch failure scenario. The launcher's own timer +
               * latch-on-enqueue cover discovery. */
              bw.bgw_notify_pid = 0;

              if (RegisterDynamicBackgroundWorker(&bw, &handle)) {
                spawned++;
              } else {
                /* Free the slot we reserved — the worker isn't going to
                 * publish (it never started). */
                pg_rolequota_unpublish_worker_latch(dbid);
                elog(WARNING,
                     "pg_rolequota launcher: failed to register worker for "
                     "database %u (cluster max_worker_processes saturated?)",
                     dbid);
              }
            }
            if (spawned > 0 || skipped_cap > 0)
              elog(LOG,
                   "pg_rolequota launcher: spawned=%d skipped_cap=%d "
                   "(max_workers=%d)",
                   spawned, skipped_cap, pg_rolequota_max_workers);
          }
          PopActiveSnapshot();
          SPI_finish();
        }
        CommitTransactionCommand();
      }
      PG_CATCH();
      {
        AbortOutOfAnyTransaction();
        FlushErrorState();
        elog(LOG, "pg_rolequota launcher: enumeration cycle threw ERROR "
                  "(will retry on next tick)");
      }
      PG_END_TRY();
    } /* close: while (!bgw_got_sigterm) */
  } /* close: first_pass scope */

  elog(LOG, "pg_rolequota launcher: exiting cleanly");
}

/* ----------------------------------------------------------------
 * Worker BGW — one per database
 * ----------------------------------------------------------------
 * Spawned dynamically by the launcher. Connects to its target database
 * (passed as bgw_main_arg), probes pg_extension, and either exits cleanly
 * (extension absent → mark dbid in negative cache) or runs the scan loop.
 */
PGDLLEXPORT void pg_rolequota_worker_main(Datum main_arg) {
  Oid dbid = DatumGetObjectId(main_arg);
  int32 last_wakeup = 0;
  int wl;
  int32 cur_wake;
  bool force_scan;
  uint32 overflow;
  RoleQuotaKey key;
  int drained;
  bool extension_present = false;

  if (!OidIsValid(dbid)) {
    elog(LOG, "pg_rolequota worker: invalid dbid arg, exiting");
    return;
  }

  elog(LOG, "pg_rolequota worker: starting for database %u", dbid);

  /* Register the slot-cleanup before any code path that can ereport(FATAL),
   * so the launcher's reservation doesn't leak if we die before publishing
   * our latch. */
  before_shmem_exit(pg_rolequota_worker_unpublish_exit_cb,
                    ObjectIdGetDatum(dbid));

  BackgroundWorkerInitializeConnectionByOid(dbid, InvalidOid, 0);

  pqsignal(SIGTERM, pg_rolequota_bgw_sigterm);
  BackgroundWorkerUnblockSignals();

  /* Probe: does the extension exist in this database? */
  PG_TRY();
  {
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    if (SPI_connect() == SPI_OK_CONNECT) {
      int ret;
      PushActiveSnapshot(GetTransactionSnapshot());
      ret = SPI_execute(
          "SELECT 1 FROM pg_extension WHERE extname = 'pg_rolequota'", true, 1);
      if (ret == SPI_OK_SELECT && SPI_processed > 0)
        extension_present = true;
      PopActiveSnapshot();
      SPI_finish();
    }
    CommitTransactionCommand();
  }
  PG_CATCH();
  {
    AbortOutOfAnyTransaction();
    FlushErrorState();
    /* Slot cleanup is handled by the before_shmem_exit callback. */
    elog(LOG, "pg_rolequota worker [%u]: extension probe threw ERROR — exiting",
         dbid);
    return;
  }
  PG_END_TRY();

  if (!extension_present) {
    pg_rolequota_mark_db_no_ext(dbid);
    /* Slot cleanup is handled by the before_shmem_exit callback. */
    elog(LOG,
         "pg_rolequota worker [%u]: extension not installed — exiting cleanly",
         dbid);
    return;
  }

  /* Publish our procLatch so SQL-level request_wakeup() from this DB
   * SetLatches us directly. The launcher's reserve_worker_slot() already
   * wrote dbid into our slot; publish_worker_latch finds it (it's our
   * slot) and writes pgprocno. The before_shmem_exit cleanup was
   * registered above, BEFORE BackgroundWorkerInitializeConnectionByOid,
   * so leak-free even on FATAL during init. */
  pg_rolequota_publish_worker_latch(dbid);

  elog(LOG,
       "pg_rolequota worker [%u]: entering scan loop (mode=%d interval=%d)",
       dbid, pg_rolequota_scanner_mode, pg_rolequota_scan_interval);

  while (!bgw_got_sigterm) {
    int requeued = 0;

    wl = PG_ROLEQUOTA_WAIT_LATCH(
        &MyProc->procLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
        (long)pg_rolequota_scan_interval * 1000L);
    ResetLatch(&MyProc->procLatch);

    if (wl & WL_POSTMASTER_DEATH)
      break;

    CHECK_FOR_INTERRUPTS();

    /* Phase 1: drain the lock-free wake-up queue. Each slot carries
     * a (dbid, roleid) pair. We filter by dbid == MyDatabaseId; entries
     * for other databases are re-enqueued so the owning worker can pick
     * them up. Bounded re-enqueue count prevents ping-pong loops.
     *
     * CHECK_FOR_INTERRUPTS() per iteration so DROP DATABASE WITH FORCE
     * (which broadcasts a ProcSignalBarrier + SIGTERM) can interrupt a
     * worker mid-drain. */
    drained = 0;
    while (pg_rolequota_dequeue_wakeup_role(&key) && !bgw_got_sigterm) {
      CHECK_FOR_INTERRUPTS();
      if (key.dbid != MyDatabaseId) {
        if (requeued < PG_ROLEQUOTA_WAKEUP_QUEUE_SIZE) {
          pg_rolequota_enqueue_wakeup_role(key.dbid, key.roleid);
          requeued++;
        }
        /* If we've already re-enqueued one full ring's worth, stop;
         * the overflow path will reconcile. */
        continue;
      }
      PG_TRY();
      {
        if (OidIsValid(key.roleid)) {
          pg_rolequota_scan_single_role(key.dbid, key.roleid);
        }
      }
      PG_CATCH();
      {
        AbortOutOfAnyTransaction();
        FlushErrorState();
        elog(LOG,
             "pg_rolequota worker [%u]: targeted scan for role %u threw "
             "ERROR (will retry on next wake)",
             MyDatabaseId, key.roleid);
      }
      PG_END_TRY();
      drained++;
    }
    if (drained > 0)
      elog(LOG, "pg_rolequota worker [%u]: drained %d targeted wake-ups",
           MyDatabaseId, drained);
    if (requeued >= PG_ROLEQUOTA_WAKEUP_QUEUE_SIZE)
      elog(LOG,
           "pg_rolequota worker [%u]: hit re-enqueue ceiling (%d cross-DB "
           "entries); overflow path will reconcile",
           MyDatabaseId, requeued);

    /* Phase 2: full scan when timer fires, legacy counter changed, or
     * the queue overflowed. */
    cur_wake = pg_rolequota_get_wakeup_counter();
    force_scan = (cur_wake != last_wakeup);
    if (force_scan)
      last_wakeup = cur_wake;

    overflow = pg_rolequota_consume_overflow();
    if (overflow > 0)
      elog(LOG,
           "pg_rolequota worker [%u]: queue overflowed (%u drops) — forcing "
           "full scan",
           MyDatabaseId, overflow);

    if (((wl & WL_TIMEOUT) || force_scan || overflow > 0) && !bgw_got_sigterm) {
      elog(LOG,
           "pg_rolequota worker [%u]: starting scan (timeout=%d wakeup=%d "
           "overflow=%u mode=%d)",
           MyDatabaseId, (wl & WL_TIMEOUT) ? 1 : 0, force_scan ? 1 : 0,
           overflow, pg_rolequota_scanner_mode);

      PG_TRY();
      {
        if (pg_rolequota_scanner_mode == 0)
          pg_rolequota_scan_shared();
        else
          pg_rolequota_scan_enterprise();
      }
      PG_CATCH();
      {
        AbortOutOfAnyTransaction();
        FlushErrorState();
        elog(LOG, "pg_rolequota worker [%u]: scan threw ERROR (will retry)",
             MyDatabaseId);
      }
      PG_END_TRY();

      elog(LOG, "pg_rolequota worker [%u]: scan cycle completed", MyDatabaseId);
    }
  }

  elog(LOG, "pg_rolequota worker [%u]: exiting cleanly", MyDatabaseId);
}
