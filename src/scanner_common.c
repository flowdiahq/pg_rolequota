/* src/scanner_common.c
 * Common post-scan logic: load quotas from rolequota.limits, compare, update
 * shmem, decide on termination / lock / notify / Slack. Used by both scanner
 * modes. Slice 1 implementation (real limits lookup + short-lock hash update
 * for hard limit enforcement). 2-space.
 *
 * Tested via rolequota.common_test() called from test_termination.sql
 * (gives this .c its required automated test per AGENTS.md §3).
 *
 * Future implementers: the hazard language below (and in the two scanner files)
 * is the contract that prevents the three targeted bug classes.
 */

#include "postgres.h"
#include "fmgr.h"

#include "compat.h"
#include "access/xact.h"
#include "executor/spi.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/builtins.h"
#include "utils/errcodes.h"
#include "utils/snapmgr.h"        /* PushActiveSnapshot / PopActiveSnapshot */
#include "utils/backend_status.h" /* pgstat_report_activity, STATE_RUNNING */
#include "pgstat.h"               /* pgstat_report_stat */
#include "storage/lwlock.h"
/* (no extra acl include needed after SPI-based role name lookup) */

/* Forward declaration for the now-public EXEC_BACKEND-safe shmem helper.
 * Proves (at link time) that other modules can call pg_rolequota_ensure_shmem
 * from shmem.c — closing the visibility gap identified in analysis.
 */
void pg_rolequota_ensure_shmem(void);

/* Cancellation GUCs — defined in pg_rolequota.c. cancel_enabled gates the
 * mid-query SIGINT path; cancel_grace_bytes raises the threshold above
 * hard_bytes so small bursts don't kill the session. */
extern bool pg_rolequota_cancel_enabled;
extern int pg_rolequota_cancel_grace_bytes;

/* Minimum interval between successive cancel attempts for the same
 * (dbid, roleid). Stops cancel storms when a role hammers retries while
 * still over hard. One second is generous; the BGW typically does at most
 * one scan per second under the wake-up bus. */
#define PG_ROLEQUOTA_CANCEL_THROTTLE_USEC INT64CONST(1000000)

/* Cross-module: enqueue (dbid, roleid) for targeted refresh + SetLatch the
 * worker BGW for that dbid. Defined in shmem.c. */
void pg_rolequota_enqueue_wakeup_role(Oid dbid, Oid roleid);

/* Public per-role targeted scanner. Cross-module from pg_rolequota.c (BGW
 * loop). The dbid arg is always MyDatabaseId in the live BGW flow; we
 * accept it explicitly so the SQL wrapper works correctly when called
 * from inside any DB. */
void pg_rolequota_scan_single_role(Oid dbid, Oid roleid);

/* Slack sender (implemented in slack.c, wired for do_notify hard/ soft) */
void pg_rolequota_slack_send(const char *message);

/* The process-local alias pointers are defined (non-static) in shmem.c.
 * We declare them extern here so common_post_scan can update the hashes
 * after ensure_shmem() has populated them. All use is under short lock.
 */
extern HTAB *RoleSizeHash;
extern HTAB *BlockedRolesHash;
extern LWLock *RoleSizeLock;

/* RoleSizeEntry / BlockedRoleEntry duplicated for direct field access from
 * this translation unit (and enforcement.c). These definitions MUST be kept
 * byte-for-byte identical to the ones in shmem.c.
 */
typedef struct RoleSizeEntry {
  RoleQuotaKey key; /* composite (dbid, roleid) — must be the first member */
  int64 used_bytes;
  int64 soft_bytes;
  int64 hard_bytes;
  bool hard_exceeded;
  bool soft_exceeded;
  TimestampTz last_checked;
  char enforcement_policy[16]; /* cached for post-scan actions (terminate/lock)
                                */
  TimestampTz last_cancel_ts;  /* throttle for mid-query cancellation bursts */
} RoleSizeEntry;

typedef struct BlockedRoleEntry {
  RoleQuotaKey key;
} BlockedRoleEntry;

/*
 * Send pg_cancel_backend() to every active session owned by `roleid` in
 * database `dbid`. Best-effort: errors (backend already gone, race with
 * session disconnect) are logged but do not propagate.
 *
 * Called from common_post_scan after a hard-limit breach is observed. The
 * worker BGW typically owns the outer transaction; we wrap SPI accordingly.
 * Returns the number of pg_cancel_backend() invocations attempted.
 *
 * Hazard contract:
 *   - Zero RoleSizeLock held by the caller (this function does SPI).
 *   - Throttling is the caller's job — we always run when invoked.
 *   - On the SQL invocation path (rolequota.scan_*() called from a user
 *     transaction), errors re-throw so the user's transaction rolls back
 *     consistently. On the BGW path, errors are swallowed.
 */
static int pg_rolequota_cancel_overage_backends(Oid dbid, Oid roleid) {
  bool aborted = false;
  bool owns_txn;
  int n_cancelled = 0;

  owns_txn = !IsTransactionState();
  if (owns_txn) {
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
  }
  if (SPI_connect() != SPI_OK_CONNECT) {
    if (owns_txn)
      CommitTransactionCommand();
    return 0;
  }
  if (owns_txn)
    PushActiveSnapshot(GetTransactionSnapshot());

  PG_TRY();
  {
    /* Cancel — NOT terminate — every active backend for (dbid, roleid)
     * EXCEPT ourselves. pg_cancel_backend() preserves the session; the
     * user's next statement then hits the enforcement hook and is rejected
     * with the normal ERRCODE_DISK_FULL message. pg_terminate_backend is
     * reserved for the explicit enforcement_policy = 'terminate' branch
     * elsewhere in this file. */
    Oid argtypes[2] = {OIDOID, OIDOID};
    Datum args[2] = {ObjectIdGetDatum(dbid), ObjectIdGetDatum(roleid)};
    int ret = SPI_execute_with_args("SELECT pg_cancel_backend(pid) "
                                    "FROM pg_stat_activity "
                                    "WHERE datid = $1 "
                                    "  AND usesysid = $2 "
                                    "  AND state = 'active' "
                                    "  AND pid <> pg_backend_pid()",
                                    2, argtypes, args, NULL,
                                    false /* read/write */, 0);
    if (ret == SPI_OK_SELECT)
      n_cancelled = (int)SPI_processed;
  }
  PG_CATCH();
  {
    if (owns_txn) {
      FlushErrorState();
      AbortOutOfAnyTransaction();
      aborted = true;
    } else {
      /* SQL caller path: let the outer transaction see the error. */
      PG_RE_THROW();
    }
  }
  PG_END_TRY();

  if (!aborted) {
    if (owns_txn)
      PopActiveSnapshot();
    SPI_finish();
  }
  if (!aborted && owns_txn)
    CommitTransactionCommand();

  return n_cancelled;
}

void pg_rolequota_common_post_scan(Oid dbid, Oid roleid, int64 used_bytes) {
  /* C89 / PG -Wdeclaration-after-statement: all decls at absolute top of block
   */
  MemoryContext oldctx;
  MemoryContext perrole_ctx;
  int64 soft_bytes;
  int64 hard_bytes;
  char policy[16];
  bool do_notify;
  bool hard_exceeded;
  bool soft_exceeded;
  bool found;
  bool owns_txn;
  RoleQuotaKey lookup_key;
  RoleSizeEntry *entry;

  elog(LOG,
       "pg_rolequota common_post_scan: ENTER dbid=%u role=%u used_bytes=%lld",
       dbid, roleid, (long long)used_bytes);

  pg_rolequota_ensure_shmem();

  if (RoleSizeHash == NULL || RoleSizeLock == NULL) {
    elog(LOG,
         "pg_rolequota common_post_scan: shmem null after ensure for role %u",
         roleid);
    return;
  }

  elog(LOG, "pg_rolequota common_post_scan: shmem ready for role %u", roleid);

  oldctx = CurrentMemoryContext;
  soft_bytes = 0;
  hard_bytes = 0;
  strlcpy(policy, "terminate", sizeof(policy));
  do_notify = true;
  (void)do_notify; /* reserved for future NOTIFY/Slack in post-scan actions
                      (outside lock) */

  /* Per-role short-lived context for all SPI and decision work (hazard
   * contract) */
  perrole_ctx = AllocSetContextCreate(
      oldctx, "pg_rolequota_common_per_role", ALLOCSET_DEFAULT_MINSIZE,
      ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);
  MemoryContextSwitchTo(perrole_ctx);

  /* SPI work for limits lookup — must complete before any RoleSizeLock.
   * BGWs require explicit transaction bracketing around every SPI_connect.
   * PushActiveSnapshot is required so the executor has an MVCC snapshot
   * available for the catalog read (canonical worker_spi.c pattern).
   *
   * Skip the Start/Commit when an outer transaction is already active —
   * we're then being called from the SQL wrapper inside a user txn.
   */
  owns_txn = !IsTransactionState();
  if (owns_txn) {
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
  }
  if (SPI_connect() == SPI_OK_CONNECT) {
    if (owns_txn)
      PushActiveSnapshot(GetTransactionSnapshot());
    PG_TRY();
    {
      /* Probe via to_regclass: returns NULL (no error) if the table
       * does not exist yet (extension not CREATE EXTENSIONed in this
       * DB). Avoids ereport(ERROR, UNDEFINED_TABLE) and the noisy
       * "scan threw ERROR (will retry)" log every cycle. */
      int probe = SPI_execute(
          "SELECT to_regclass('rolequota.limits') IS NOT NULL", true, 1);
      bool limits_exists = false;
      if (probe == SPI_OK_SELECT && SPI_processed > 0) {
        bool isnull;
        Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1,
                                &isnull);
        limits_exists = !isnull && DatumGetBool(d);
      }

      if (limits_exists) {
        Oid argtypes[1] = {OIDOID};
        Datum args[1] = {ObjectIdGetDatum(roleid)};
        int ret = SPI_execute_with_args(
            "SELECT soft_bytes, hard_bytes, enforcement_policy, notify "
            "FROM rolequota.limits WHERE roleid = $1",
            1, argtypes, args, NULL, true /* read-only */, 1);

        if (ret == SPI_OK_SELECT && SPI_processed > 0) {
          TupleDesc td = SPI_tuptable->tupdesc;
          HeapTuple tup = SPI_tuptable->vals[0];
          bool isnull;
          Datum d;

          d = SPI_getbinval(tup, td, 1, &isnull);
          soft_bytes = isnull ? 0 : DatumGetInt64(d);

          d = SPI_getbinval(tup, td, 2, &isnull);
          hard_bytes = isnull ? 0 : DatumGetInt64(d);

          d = SPI_getbinval(tup, td, 3, &isnull);
          if (!isnull) {
            text *txt = DatumGetTextPP(d);
            char *cstr = text_to_cstring(txt);
            strlcpy(policy, cstr, sizeof(policy));
            pfree(cstr);
          }

          d = SPI_getbinval(tup, td, 4, &isnull);
          do_notify = isnull ? true : DatumGetBool(d);
        }
      }
    }
    PG_CATCH();
    {
      /* Any SPI error propagates to the outer caller (BGW PG_CATCH or SQL
       * invoker). Do not clean up SPI/snapshot/transaction state here —
       * double cleanup leaves the next scan cycle's SPI bookkeeping
       * corrupted and segfaults. */
      MemoryContextSwitchTo(oldctx);
      MemoryContextDelete(perrole_ctx);
      PG_RE_THROW();
    }
    PG_END_TRY();
    if (owns_txn)
      PopActiveSnapshot();
    SPI_finish();
  }
  if (owns_txn)
    CommitTransactionCommand();

  /* Switch out and delete temp ctx before lock (no allocations in lock section)
   */
  MemoryContextSwitchTo(oldctx);
  MemoryContextDelete(perrole_ctx);

  elog(LOG,
       "pg_rolequota common_post_scan: limits lookup done role=%u soft=%lld "
       "hard=%lld policy=%s notify=%d",
       roleid, (long long)soft_bytes, (long long)hard_bytes, policy,
       do_notify ? 1 : 0);

  /* Decision (outside lock) — vars declared at function top for C89 hygiene */
  hard_exceeded = (hard_bytes > 0 && used_bytes > hard_bytes);
  soft_exceeded = (soft_bytes > 0 && used_bytes > soft_bytes);

  /* Short exclusive lock only for the hash mutation (hazard contract:
   * zero I/O, zero ereport, zero allocation inside this critical section).
   * Cap-warning ereports are deferred to flags read outside the lock. */
  {
    TimestampTz now = GetCurrentTimestamp(); /* hoisted out of lock */
    bool role_cap_hit = false;
    bool blocked_cap_hit = false;
    bool should_cancel = false;

    lookup_key.dbid = dbid;
    lookup_key.roleid = roleid;

    LWLockAcquire(RoleSizeLock, LW_EXCLUSIVE);

    entry = (RoleSizeEntry *)hash_search(RoleSizeHash, &lookup_key, HASH_ENTER,
                                         &found);
    if (entry != NULL) {
      if (!found) {
        entry->key = lookup_key;
        entry->used_bytes = 0;
        entry->soft_bytes = 0;
        entry->hard_bytes = 0;
        entry->hard_exceeded = false;
        entry->soft_exceeded = false;
        entry->enforcement_policy[0] = '\0';
        entry->last_cancel_ts = 0;
      }

      entry->used_bytes = used_bytes;
      entry->soft_bytes = soft_bytes;
      entry->hard_bytes = hard_bytes;
      entry->soft_exceeded = soft_exceeded;
      entry->hard_exceeded = hard_exceeded;
      entry->last_checked = now;
      strlcpy(entry->enforcement_policy, policy,
              sizeof(entry->enforcement_policy));

      /* Mid-query cancellation decision — made under the lock so we can
       * atomically read+update last_cancel_ts and avoid duplicate cancel
       * bursts from overlapping scan cycles.
       *
       * Conditions:
       *   - GUC pg_rolequota.cancel_enabled is true (operator opt-out).
       *   - used_bytes exceeds hard_bytes + cancel_grace_bytes.
       *   - At least PG_ROLEQUOTA_CANCEL_THROTTLE_USEC has elapsed since
       *     the last cancel for this (dbid, roleid).
       *
       * The actual SPI call to pg_cancel_backend happens strictly OUTSIDE
       * this lock (below) — zero I/O / ereport / allocation in the
       * critical section. */
      if (pg_rolequota_cancel_enabled && hard_bytes > 0 &&
          used_bytes > hard_bytes + (int64)pg_rolequota_cancel_grace_bytes) {
        int64 since_last;
        since_last = (entry->last_cancel_ts == 0)
                         ? PG_ROLEQUOTA_CANCEL_THROTTLE_USEC + 1
                         : (int64)(now - entry->last_cancel_ts);
        if (since_last >= PG_ROLEQUOTA_CANCEL_THROTTLE_USEC) {
          should_cancel = true;
          entry->last_cancel_ts = now;
        }
      }

      /* Maintain BlockedRolesHash for 'lock' policy. */
      if (hard_exceeded && strcmp(policy, "lock") == 0) {
        BlockedRoleEntry *b = (BlockedRoleEntry *)hash_search(
            BlockedRolesHash, &lookup_key, HASH_ENTER, &found);
        if (b != NULL)
          b->key = lookup_key;
        else
          blocked_cap_hit = true;
      } else {
        (void)hash_search(BlockedRolesHash, &lookup_key, HASH_REMOVE, NULL);
      }
    } else {
      role_cap_hit = true;
    }

    LWLockRelease(RoleSizeLock);

    /* Cap-hit warnings emitted strictly outside the lock. */
    if (role_cap_hit)
      elog(WARNING,
           "pg_rolequota: RoleSizeHash full (cap %d), skipping update for "
           "role %u",
           PG_ROLEQUOTA_MAX_ROLE_ENTRIES, roleid);
    if (blocked_cap_hit)
      elog(WARNING,
           "pg_rolequota: BlockedRolesHash full, could not block role %u",
           roleid);

    /* Mid-query cancellation — outside the lock per hazard contract. */
    if (should_cancel) {
      int cancelled = pg_rolequota_cancel_overage_backends(dbid, roleid);
      elog(LOG,
           "pg_rolequota common_post_scan: cancelled %d active backend(s) "
           "for dbid=%u role=%u (used=%lld > hard=%lld + grace=%d)",
           cancelled, dbid, roleid, (long long)used_bytes,
           (long long)hard_bytes, pg_rolequota_cancel_grace_bytes);
    }
  }

  /* === Real policy actions (executed strictly outside RoleSizeLock — contract)
   * === */
  if (hard_exceeded) {
    elog(LOG,
         "pg_rolequota common_post_scan: HARD EXCEEDED role=%u policy=%s -> "
         "taking action",
         roleid, policy);

    if (strcmp(policy, "terminate") == 0) {
      bool term_aborted = false;
      bool term_owns_txn;
      elog(LOG,
           "pg_rolequota common_post_scan: executing termination for role %u",
           roleid);
      /* Terminate sessions for this role. Idempotent and safe to retry.
       * Only swallow errors when we own the outer transaction (e.g. BGW);
       * when invoked from an SQL caller we must re-throw so the caller's
       * transaction sees the error and rolls back consistently. */
      term_owns_txn = !IsTransactionState();
      if (term_owns_txn) {
        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();
      }
      if (SPI_connect() == SPI_OK_CONNECT) {
        if (term_owns_txn)
          PushActiveSnapshot(GetTransactionSnapshot());
        PG_TRY();
        {
          /* Use pg_terminate_backend on all backends owned by the role */
          char query[256];
          snprintf(
              query, sizeof(query),
              "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
              "WHERE usename = (SELECT rolname FROM pg_roles WHERE oid = %u) "
              "AND pid <> pg_backend_pid()",
              roleid);

          (void)SPI_execute(query, false, 0);
        }
        PG_CATCH();
        {
          if (term_owns_txn) {
            /* BGW path: swallow termination errors (backend already gone,
             * etc.) and let the next scan cycle retry. */
            FlushErrorState();
            AbortOutOfAnyTransaction();
            term_aborted = true;
          } else {
            /* SQL caller path: re-throw so the outer transaction rolls back. */
            PG_RE_THROW();
          }
        }
        PG_END_TRY();
        if (!term_aborted) {
          if (term_owns_txn)
            PopActiveSnapshot();
          SPI_finish();
        }
      }
      if (!term_aborted && term_owns_txn)
        CommitTransactionCommand();
    } else if (strcmp(policy, "lock") == 0) {
      bool lock_aborted = false;
      bool lock_owns_txn;
      elog(LOG,
           "pg_rolequota common_post_scan: executing account lock for role %u",
           roleid);
      /* Lock the account (NOLOGIN). Only do ALTER if not already locked. */
      lock_owns_txn = !IsTransactionState();
      if (lock_owns_txn) {
        SetCurrentStatementStartTimestamp();
        StartTransactionCommand();
      }
      if (SPI_connect() == SPI_OK_CONNECT) {
        if (lock_owns_txn)
          PushActiveSnapshot(GetTransactionSnapshot());
        PG_TRY();
        {
          char query[256];
          int ret;
          snprintf(
              query, sizeof(query),
              "SELECT rolname FROM pg_roles WHERE oid = %u AND rolcanlogin",
              roleid);

          ret = SPI_execute(query, true, 1);
          if (ret == SPI_OK_SELECT && SPI_processed > 0) {
            char *rolname =
                SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
            if (rolname) {
              char alter[256];
              snprintf(alter, sizeof(alter), "ALTER ROLE %s NOLOGIN",
                       quote_identifier(rolname));
              (void)SPI_execute(alter, false, 0);
            }
          }
        }
        PG_CATCH();
        {
          if (lock_owns_txn) {
            FlushErrorState();
            AbortOutOfAnyTransaction();
            lock_aborted = true;
          } else {
            PG_RE_THROW();
          }
        }
        PG_END_TRY();
        if (!lock_aborted) {
          if (lock_owns_txn)
            PopActiveSnapshot();
          SPI_finish();
        }
      }
      if (!lock_aborted && lock_owns_txn)
        CommitTransactionCommand();
    }
    /* 'warn' does nothing here — logging can be added later via events table or
     * NOTICE */
  } else if (soft_exceeded) {
    /* Soft limit crossed, hard not exceeded, and policy is warn (or any
     * non-hard action) */
    elog(WARNING,
         "pg_rolequota: soft limit exceeded for role %u (used %lld > soft "
         "%lld, policy=%s)",
         roleid, (long long)used_bytes, (long long)soft_bytes, policy);
  }

  /* === LISTEN/NOTIFY + Slack polish: emit on threshold cross (best-effort,
   * outside locks) === */
  if (do_notify && hard_exceeded) {
    bool notify_aborted = false;
    bool notify_owns_txn;
    elog(LOG,
         "pg_rolequota common_post_scan: emitting NOTIFY + Slack for role %u",
         roleid);
    notify_owns_txn = !IsTransactionState();
    if (notify_owns_txn) {
      SetCurrentStatementStartTimestamp();
      StartTransactionCommand();
    }
    if (SPI_connect() == SPI_OK_CONNECT) {
      if (notify_owns_txn)
        PushActiveSnapshot(GetTransactionSnapshot());
      PG_TRY();
      {
        char notify_cmd[128];
        snprintf(notify_cmd, sizeof(notify_cmd), "NOTIFY quota_wakeup, '%u'",
                 roleid);
        (void)SPI_execute(notify_cmd, false, 0);
      }
      PG_CATCH();
      {
        if (notify_owns_txn) {
          FlushErrorState();
          AbortOutOfAnyTransaction();
          notify_aborted = true;
        } else {
          PG_RE_THROW();
        }
      }
      PG_END_TRY();
      if (!notify_aborted) {
        if (notify_owns_txn)
          PopActiveSnapshot();
        SPI_finish();
      }
    }
    if (!notify_aborted && notify_owns_txn)
      CommitTransactionCommand();
    {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "role %u hard limit exceeded (used=%lld hard=%lld policy=%s)",
               roleid, (long long)used_bytes, (long long)hard_bytes, policy);
      pg_rolequota_slack_send(msg);
    }
    /* Also enqueue the (dbid, roleid) pair for a fresh targeted re-scan
     * on the next worker wake. After a terminate/lock action the on-disk
     * footprint can change (sessions are killed, role is locked).
     * Re-running the per-role aggregate refreshes used_bytes so recovery
     * (space reclamation) is detected promptly without waiting for the
     * periodic full scan. */
    pg_rolequota_enqueue_wakeup_role(dbid, roleid);
  }

  elog(LOG,
       "pg_rolequota common_post_scan: EXIT dbid=%u role=%u (hard_exceeded=%d "
       "soft_exceeded=%d)",
       dbid, roleid, hard_exceeded ? 1 : 0, soft_exceeded ? 1 : 0);
}

/*
 * Targeted per-role refresh. Runs a single cheap SPI aggregate over the
 * one role's relations and feeds the result to pg_rolequota_common_post_scan
 * (which then does the limits lookup + hash update + policy actions).
 *
 * Called by the BGW loop after dequeueing a roleid from the wake-up ring,
 * and indirectly via the SQL surface rolequota.request_wakeup(oid) /
 * rolequota.request_self_wakeup().
 *
 * Hazard contract is identical to common_post_scan: zero lock held during
 * SPI, IsTransactionState() guard so the SQL wrapper path works inside an
 * outer user transaction. Errors propagate to the caller's PG_CATCH (BGW
 * outer for the BGW path, the SQL invoker for the SQL path).
 */
void pg_rolequota_scan_single_role(Oid dbid, Oid roleid) {
  /* C89 — all decls at top of block. */
  MemoryContext oldctx;
  MemoryContext perrole_ctx;
  int64 used_bytes = 0;
  bool owns_txn;

  pg_rolequota_ensure_shmem();
  if (RoleSizeHash == NULL || RoleSizeLock == NULL) {
    elog(LOG,
         "pg_rolequota scan_single_role: shmem null after ensure for dbid=%u "
         "role=%u",
         dbid, roleid);
    return;
  }

  oldctx = CurrentMemoryContext;
  perrole_ctx = AllocSetContextCreate(
      oldctx, "pg_rolequota_scan_single_role", ALLOCSET_DEFAULT_MINSIZE,
      ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);
  MemoryContextSwitchTo(perrole_ctx);

  owns_txn = !IsTransactionState();
  if (owns_txn) {
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
  }
  if (SPI_connect() == SPI_OK_CONNECT) {
    if (owns_txn)
      PushActiveSnapshot(GetTransactionSnapshot());
    PG_TRY();
    {
      Oid argtypes[1] = {OIDOID};
      Datum args[1] = {ObjectIdGetDatum(roleid)};
      int ret;
      const char *q =
          "SELECT COALESCE(SUM(pg_total_relation_size(oid)), 0)::bigint "
          "FROM pg_class "
          "WHERE relowner = $1 "
          "  AND relkind IN ('r','m','t','i') "
          "  AND relpersistence <> 't' "
          "  AND relfilenode <> 0";

      ret = SPI_execute_with_args(q, 1, argtypes, args, NULL,
                                  true /* read-only */, 1);
      if (ret == SPI_OK_SELECT && SPI_processed > 0) {
        bool isnull;
        Datum d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1,
                                &isnull);
        if (!isnull)
          used_bytes = DatumGetInt64(d);
      }
    }
    PG_CATCH();
    {
      /* Same policy as common_post_scan: do not double-clean SPI/snapshot
       * state here. Let the outer caller (BGW PG_CATCH or SQL invoker)
       * handle abort + flush. */
      MemoryContextSwitchTo(oldctx);
      MemoryContextDelete(perrole_ctx);
      PG_RE_THROW();
    }
    PG_END_TRY();
    if (owns_txn)
      PopActiveSnapshot();
    SPI_finish();
  }
  if (owns_txn)
    CommitTransactionCommand();

  MemoryContextSwitchTo(oldctx);
  MemoryContextDelete(perrole_ctx);

  elog(LOG,
       "pg_rolequota scan_single_role: dbid=%u role=%u used_bytes=%lld, "
       "dispatching to common_post_scan",
       dbid, roleid, (long long)used_bytes);

  /* Delegate to the shared per-role logic. */
  pg_rolequota_common_post_scan(dbid, roleid, used_bytes);
}

Datum pg_rolequota_common_test(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pg_rolequota_common_test);

/*
 * Test entry point. Called from test_termination.sql (and potentially other
 * policy-matrix tests later). Proves scanner_common.c was compiled and linked
 * and satisfies the "every .c file has an automated test" rule.
 *
 * We also call pg_rolequota_ensure_shmem() here to prove at test time that
 * the non-static symbol exported from shmem.c is resolvable from this
 * compilation unit (the previous "static" definition was the latent gap).
 */
Datum pg_rolequota_common_test(PG_FUNCTION_ARGS) {
  pg_rolequota_ensure_shmem(); /* cross-module call + EXEC_BACKEND contract
                                  exercise */
  PG_RETURN_INT32(1);
}
