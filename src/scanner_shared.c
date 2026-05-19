/* src/scanner_shared.c
 * shared_hosting scanner: single cheap SPI aggregate over pg_class +
 * pg_total_relation_size for thousands of lightweight tenants. Slice 1
 * implementation (real SPI aggregate + common_post_scan dispatch). 2-space
 * indentation.
 *
 * Future implementers: see the explicit hazard bullets in scanner_enterprise.c.
 * The SPI path has analogous (but lighter) rules.
 */

#include "postgres.h"
#include "fmgr.h"

#include "compat.h"
#include "access/xact.h"
#include "miscadmin.h" /* MyDatabaseId */
#include "utils/builtins.h"
#include "executor/spi.h"
#include "utils/memutils.h"
#include "utils/errcodes.h"
#include "utils/snapmgr.h"        /* PushActiveSnapshot / PopActiveSnapshot */
#include "utils/backend_status.h" /* pgstat_report_activity, STATE_RUNNING */
#include "pgstat.h"               /* pgstat_report_stat */
#include "utils/timestamp.h"      /* SetCurrentStatementStartTimestamp */

/* Forward for ensure (defined in shmem.c) */
void pg_rolequota_ensure_shmem(void);

/* Alias from shmem.c (populated by ensure) */
extern HTAB *RoleSizeHash;

/* GUC for cap (polish: use-guc-max, bounded by shmem compile-time size) */
extern int pg_rolequota_max_roles_tracked;

/* Forward to common_post_scan (implemented in scanner_common.c). The dbid
 * argument is the database the worker is currently bound to (MyDatabaseId)
 * so the composite hash key correctly scopes the entry. */
void pg_rolequota_common_post_scan(Oid dbid, Oid roleid, int64 used_bytes);

/* Local struct for collecting aggregate results inside the per-scan temp
 * context */
typedef struct RoleSizeInfo {
  Oid roleid;
  int64 used_bytes;
} RoleSizeInfo;

void pg_rolequota_scan_shared(void) {
  /* C89 / PG -Wdeclaration-after-statement: all decls at absolute top of block
   */
  MemoryContext oldctx;
  MemoryContext scanctx;
  RoleSizeInfo *results;
  int nresults;
  bool owns_txn;
  const int maxcap =
      PG_ROLEQUOTA_MAX_ROLE_ENTRIES; /* match shmem hard cap (GUC
                                        pg_rolequota.max_roles_tracked
                                        drives future sizing) */

  elog(LOG, "pg_rolequota scan_shared: entering (shared_hosting mode)");

  pg_rolequota_ensure_shmem();

  if (RoleSizeHash == NULL) {
    elog(LOG, "pg_rolequota scan_shared: RoleSizeHash still NULL after ensure, "
              "bailing");
    return;
  }

  elog(LOG, "pg_rolequota scan_shared: shmem ready, running SPI aggregate");

  oldctx = CurrentMemoryContext;
  nresults = 0;
  results = NULL;

  /* Fresh temp context for the entire aggregate + collection (hazard: zero lock
   * held) */
  scanctx = AllocSetContextCreate(
      oldctx, "pg_rolequota_scan_shared", ALLOCSET_DEFAULT_MINSIZE,
      ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);
  MemoryContextSwitchTo(scanctx);

  results = palloc(sizeof(RoleSizeInfo) * maxcap);

  /* Proper BGW transaction bracketing is required before SPI in a background
   * worker (bare SPI_connect without StartTransactionCommand causes NULL
   * derefs deep in the executor / pg_total_relation_size on many PG versions).
   * Canonical worker_spi.c pattern: timestamp -> Start -> SPI_connect ->
   * PushActiveSnapshot -> pgstat_report_activity -> SPI_execute -> Pop ->
   * finish
   * -> Commit -> idle. PushActiveSnapshot is required so the executor has an
   * MVCC snapshot for catalog reads inside pg_total_relation_size.
   *
   * Skip the Start/Commit when an outer transaction is already active —
   * the SQL wrapper rolequota.scan_shared() invokes us from inside a
   * user transaction, in which case we just reuse it and let the caller
   * commit. Without this guard we hit "StartTransactionCommand: unexpected
   * state STARTED".
   */
  owns_txn = !IsTransactionState();
  if (owns_txn) {
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
  }

  /* SPI aggregate over pg_class (cheap for shared_hosting). Must finish before
   * lock.
   *
   * relkind filter ('r','m','t','i') restricts to relations with on-disk
   * storage — tables, matviews, TOAST, indexes. Excludes views, composite
   * types, sequences, foreign tables, and partitioned-table parents whose
   * relfilenode is zero. Calling pg_total_relation_size on those reaches
   * code paths in smgr that can NULL-deref on PG 18 (the original SIGSEGV
   * root cause). Defence-in-depth: also require relfilenode <> 0.
   *
   * COALESCE(...)::bigint pins the result type: SUM(bigint) returns
   * numeric in PostgreSQL, and DatumGetInt64 on a numeric Datum reads the
   * pointer value as an integer (garbage). The explicit ::bigint cast lets
   * DatumGetInt64 read the actual byte count.
   */
  if (SPI_connect() == SPI_OK_CONNECT) {
    /* Only push our own snapshot when we own the transaction. When called
     * from SQL the outer executor already has an active snapshot. */
    if (owns_txn)
      PushActiveSnapshot(GetTransactionSnapshot());
    PG_TRY();
    {
      const char *agg =
          "SELECT relowner, "
          "       COALESCE(SUM(pg_total_relation_size(oid)), 0)::bigint "
          "FROM pg_class "
          "WHERE relkind IN ('r','m','t','i') "
          "  AND relpersistence <> 't' "
          "  AND relfilenode <> 0 "
          "GROUP BY relowner";
      int ret;

      pgstat_report_activity(STATE_RUNNING, agg);
      ret = SPI_execute(agg, true /* read-only */, 0);

      if (ret == SPI_OK_SELECT) {
        uint64 nrows = SPI_processed;
        TupleDesc td = SPI_tuptable->tupdesc;

        for (uint64 r = 0; r < nrows && nresults < maxcap; r++) {
          HeapTuple tup = SPI_tuptable->vals[r];
          bool isnull1, isnull2;
          Datum d1 = SPI_getbinval(tup, td, 1, &isnull1);
          Datum d2 = SPI_getbinval(tup, td, 2, &isnull2);

          if (!isnull1 && !isnull2) {
            results[nresults].roleid = DatumGetObjectId(d1);
            results[nresults].used_bytes = DatumGetInt64(d2);
            nresults++;
          }
        }

        if (nresults == maxcap && nrows > (uint64)nresults)
          elog(WARNING, "pg_rolequota: shared scan truncated at cap (%d roles)",
               maxcap);
      }
    }
    PG_CATCH();
    {
      /* Any SPI error here propagates to the outer caller (BGW PG_CATCH or
       * SQL invoker). Do NOT clean up SPI/snapshot/transaction state here —
       * double cleanup leaves the next scan cycle's SPI bookkeeping corrupted
       * and segfaults. */
      MemoryContextSwitchTo(oldctx);
      MemoryContextDelete(scanctx);
      if (owns_txn)
        pgstat_report_activity(STATE_IDLE, NULL);
      PG_RE_THROW();
    }
    PG_END_TRY();
    if (owns_txn)
      PopActiveSnapshot();
    SPI_finish();
  }

  if (owns_txn) {
    CommitTransactionCommand();
    pgstat_report_activity(STATE_IDLE, NULL);
    pgstat_report_stat(true);
  }

  /* Leave scanctx (results live until we delete after use) */
  MemoryContextSwitchTo(oldctx);

  /* Now feed each result to common_post_scan (which does its own limits SPI +
   * short lock) */
  elog(LOG,
       "pg_rolequota scan_shared: collected %d roles, dispatching to "
       "common_post_scan",
       nresults);

  for (int i = 0; i < nresults; i++) {
    if (i < 3) /* log first few to avoid spam while debugging */
      elog(LOG,
           "pg_rolequota scan_shared: post-scan dbid=%u role=%u bytes=%lld",
           MyDatabaseId, results[i].roleid, (long long)results[i].used_bytes);
    pg_rolequota_common_post_scan(MyDatabaseId, results[i].roleid,
                                  results[i].used_bytes);
  }

  MemoryContextDelete(scanctx);
  elog(LOG, "pg_rolequota scan_shared: finished");
}

Datum pg_rolequota_scanner_shared_test(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pg_rolequota_scanner_shared_test);

Datum pg_rolequota_scanner_shared_test(PG_FUNCTION_ARGS) {
  PG_RETURN_TEXT_P(cstring_to_text("shared_stub"));
}

/* SQL-callable wrapper so tests (and admins) can trigger a scan on demand.
 * This exercises the full shared scanner + common_post_scan path and is
 * required for test coverage of the real implementation (AGENTS.md).
 * The BGW will call the internal pg_rolequota_scan_shared directly.
 */
Datum pg_rolequota_scan_shared_sql(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pg_rolequota_scan_shared_sql);

Datum pg_rolequota_scan_shared_sql(PG_FUNCTION_ARGS) {
  pg_rolequota_scan_shared();
  PG_RETURN_VOID();
}
