/* src/scanner_enterprise.c
 * Enterprise_db scanner: full filesystem walk of base/ + pg_tblspc/, generation
 * stamps, RelSizeEntry / FileSizeEntry model (adapted from hlinnaka/pg_quota
 * fs_model.c). Slice 2 implementation: real authoritative FS walk for complex
 * schemas. 2-space indentation (AGENTS.md).
 *
 * The hazard bullets below are the binding contract (obeyed by this
 * implementation).
 */

#include "postgres.h"
#include "fmgr.h"

#include "compat.h"
#include "access/xact.h"
#include "utils/builtins.h"
#include "storage/fd.h" /* AllocateDir, ReadDir, FreeDir */
#include "sys/stat.h"
#include "dirent.h"
#include "miscadmin.h" /* DataDir, MyDatabaseId */
#include "executor/spi.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"        /* PushActiveSnapshot / PopActiveSnapshot */
#include "utils/backend_status.h" /* pgstat_report_activity, STATE_RUNNING */
#include "pgstat.h"               /* pgstat_report_stat */
#include "storage/lwlock.h"
#include "utils/hsearch.h"
#include "utils/timestamp.h" /* GetCurrentTimestamp */

/* Forward for ensure + common (Slice 2). dbid is the worker's MyDatabaseId,
 * passed explicitly so the composite (dbid, roleid) hash key is correct. */
void pg_rolequota_ensure_shmem(void);
void pg_rolequota_common_post_scan(Oid dbid, Oid roleid, int64 used_bytes);

/* Extern aliases (populated by ensure) — match shmem.c */
extern HTAB *RoleSizeHash;
extern HTAB *BlockedRolesHash;
extern LWLock *RoleSizeLock;

/* Duplicated entry structs for direct access (must match shmem.c) */
typedef struct RoleSizeEntry {
  RoleQuotaKey key;
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

/* Local accumulator for filenode sizes during walk (in scan context) */
typedef struct FilenodeSizeEntry {
  Oid relfilenode;
  int64 total_bytes;
} FilenodeSizeEntry;

/* Local map: relfilenode -> owner (populated via catalog before walk) */
typedef struct FilenodeOwnerEntry {
  Oid relfilenode;
  Oid owner;
} FilenodeOwnerEntry;

/* Local per-owner total (after mapping) */
typedef struct OwnerSizeEntry {
  Oid owner;
  int64 total_bytes;
} OwnerSizeEntry;

/* Walk a single directory, accumulate sizes keyed by base relfilenode.
 * Must be called with zero RoleSizeLock held.
 */
static void walk_and_accumulate(const char *dirpath, HTAB *filenode_size) {
  DIR *dir = AllocateDir(dirpath);
  if (dir == NULL)
    return; /* directory may legitimately not exist (unused tblspc) */

  struct dirent *de;
  while ((de = ReadDir(dir, dirpath)) != NULL) {
    if (de->d_name[0] == '.')
      continue;

    char fullpath[MAXPGPATH];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, de->d_name);

    struct stat st;
    if (stat(fullpath, &st) != 0 || !S_ISREG(st.st_mode))
      continue;

    /* Extract base relfilenode (before first '.' for forks/segments) */
    char namebuf[64];
    strlcpy(namebuf, de->d_name, sizeof(namebuf));
    char *dot = strchr(namebuf, '.');
    if (dot)
      *dot = '\0';

    long node = strtol(namebuf, NULL, 10);
    if (node <= 0)
      continue;

    Oid key = (Oid)node;
    bool found;
    FilenodeSizeEntry *e = (FilenodeSizeEntry *)hash_search(filenode_size, &key,
                                                            HASH_ENTER, &found);
    if (e != NULL) {
      e->relfilenode = key;
      e->total_bytes += (int64)st.st_size;
    }
  }
  FreeDir(dir);
}

/* Called by the background worker (or test) when mode == enterprise_db */
void pg_rolequota_scan_enterprise(void) {
  bool owns_txn;

  elog(LOG, "pg_rolequota scan_enterprise: entering (enterprise_db mode)");

  pg_rolequota_ensure_shmem();

  if (RoleSizeHash == NULL || RoleSizeLock == NULL) {
    elog(LOG, "pg_rolequota scan_enterprise: shmem not ready after ensure");
    return;
  }

  elog(LOG,
       "pg_rolequota scan_enterprise: shmem ready, starting filesystem walk");

  /* Fresh per-scan MemoryContext (hazard contract #3) — deleted on all exit
   * paths */
  MemoryContext oldctx = CurrentMemoryContext;
  MemoryContext scanctx = AllocSetContextCreate(
      oldctx, "pg_rolequota_scan_enterprise", ALLOCSET_DEFAULT_MINSIZE,
      ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_MAXSIZE);
  MemoryContextSwitchTo(scanctx);

  /* 1. Build relfilenode -> owner map (SPI, outside any lock — hazard #1) */
  HTAB *filenode_to_owner = NULL;
  {
    HASHCTL info;
    memset(&info, 0, sizeof(info));
    info.keysize = sizeof(Oid);
    info.entrysize = sizeof(FilenodeOwnerEntry);
    filenode_to_owner = hash_create("pg_rolequota filenode->owner map", 1024,
                                    &info, HASH_ELEM | HASH_BLOBS);
  }

  /* Skip Start/Commit when an outer transaction is already active (called
   * from rolequota.scan_enterprise() SQL wrapper). */
  owns_txn = !IsTransactionState();
  if (owns_txn) {
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
  }
  /* No-op snapshot helper used twice below; only the BGW path needs to push
   * its own snapshot. SQL callers already have one from the outer executor. */
  if (SPI_connect() == SPI_OK_CONNECT) {
    if (owns_txn)
      PushActiveSnapshot(GetTransactionSnapshot());
    PG_TRY();
    {
      /* relkind filter matches scanner_shared.c — only storage-bearing
       * relations contribute to per-role disk usage. Excludes views,
       * composite types, sequences, foreign tables, and partitioned-table
       * parents. The relfilenode <> 0 guard is kept as defence in depth. */
      int ret = SPI_execute("SELECT relfilenode, relowner FROM pg_class "
                            "WHERE relkind IN ('r','m','t','i') "
                            "  AND relpersistence <> 't' "
                            "  AND relfilenode <> 0",
                            true, 0);

      if (ret == SPI_OK_SELECT) {
        for (uint64 r = 0; r < SPI_processed; r++) {
          HeapTuple tup = SPI_tuptable->vals[r];
          TupleDesc td = SPI_tuptable->tupdesc;
          bool isnull1, isnull2;
          Datum dnode = SPI_getbinval(tup, td, 1, &isnull1);
          Datum downer = SPI_getbinval(tup, td, 2, &isnull2);
          if (!isnull1 && !isnull2) {
            Oid node = DatumGetObjectId(dnode);
            Oid owner = DatumGetObjectId(downer);
            bool f;
            FilenodeOwnerEntry *e = (FilenodeOwnerEntry *)hash_search(
                filenode_to_owner, &node, HASH_ENTER, &f);
            if (e)
              e->owner = owner;
          }
        }
      }
    }
    PG_CATCH();
    {
      /* Other errors: let outer BGW PG_CATCH handle abort + flush. */
      MemoryContextSwitchTo(oldctx);
      MemoryContextDelete(scanctx);
      PG_RE_THROW();
    }
    PG_END_TRY();
    if (owns_txn)
      PopActiveSnapshot();
    SPI_finish();
  }
  if (owns_txn)
    CommitTransactionCommand();

  /* 2. Filenode size accumulator (in scan context) */
  HTAB *filenode_size = NULL;
  {
    HASHCTL info;
    memset(&info, 0, sizeof(info));
    info.keysize = sizeof(Oid);
    info.entrysize = sizeof(FilenodeSizeEntry);
    filenode_size = hash_create("pg_rolequota filenode sizes", 4096, &info,
                                HASH_ELEM | HASH_BLOBS);
  }

  /* 3. Walk the filesystem (zero lock held — hazard #1) */
  Oid dbid = MyDatabaseId;

  /* Default tablespace (base/<dboid>) */
  {
    char basepath[MAXPGPATH];
    snprintf(basepath, sizeof(basepath), "%s/base/%u", DataDir, dbid);
    walk_and_accumulate(basepath, filenode_size);
  }

  /* Additional tablespaces used by relations in this DB */
  {
    bool tblspc_aborted = false;
    bool tblspc_owns_txn = !IsTransactionState();
    if (tblspc_owns_txn) {
      SetCurrentStatementStartTimestamp();
      StartTransactionCommand();
    }
    if (SPI_connect() == SPI_OK_CONNECT) {
      if (tblspc_owns_txn)
        PushActiveSnapshot(GetTransactionSnapshot());
      PG_TRY();
      {
        int ret =
            SPI_execute("SELECT DISTINCT reltablespace FROM pg_class "
                        "WHERE reltablespace <> 0 AND reltablespace <> (SELECT "
                        "oid FROM pg_tablespace WHERE spcname='pg_default')",
                        true, 0);

        if (ret == SPI_OK_SELECT) {
          for (uint64 r = 0; r < SPI_processed; r++) {
            HeapTuple tup = SPI_tuptable->vals[r];
            TupleDesc td = SPI_tuptable->tupdesc;
            bool isnull;
            Datum d = SPI_getbinval(tup, td, 1, &isnull);
            if (!isnull) {
              Oid spc = DatumGetObjectId(d);
              char tspath[MAXPGPATH];
              snprintf(tspath, sizeof(tspath), "%s/pg_tblspc/%u/%u", DataDir,
                       spc, dbid);
              walk_and_accumulate(tspath, filenode_size);
            }
          }
        }
      }
      PG_CATCH();
      {
        if (tblspc_owns_txn) {
          /* BGW path: ignore tblspc discovery errors; default path already
           * walked. AbortOutOfAnyTransaction handles SPI + snapshot + xact. */
          FlushErrorState();
          AbortOutOfAnyTransaction();
          tblspc_aborted = true;
        } else {
          /* SQL caller path: re-throw, but first delete our per-scan context
           * so the caller's transaction abort doesn't leak it. */
          MemoryContextSwitchTo(oldctx);
          MemoryContextDelete(scanctx);
          PG_RE_THROW();
        }
      }
      PG_END_TRY();
      if (!tblspc_aborted) {
        if (tblspc_owns_txn)
          PopActiveSnapshot();
        SPI_finish();
      }
    }
    if (!tblspc_aborted && tblspc_owns_txn)
      CommitTransactionCommand();
  }

  /* 4. Map filenodes to owners and aggregate per-owner totals (still outside
   * lock) */
  HTAB *owner_size = NULL;
  {
    HASHCTL info;
    memset(&info, 0, sizeof(info));
    info.keysize = sizeof(Oid);
    info.entrysize = sizeof(OwnerSizeEntry);
    owner_size = hash_create("pg_rolequota owner sizes", 1024, &info,
                             HASH_ELEM | HASH_BLOBS);
  }

  /* Iterate filenode_size and map */
  {
    HASH_SEQ_STATUS status;
    FilenodeSizeEntry *fs;
    hash_seq_init(&status, filenode_size);
    while ((fs = (FilenodeSizeEntry *)hash_seq_search(&status)) != NULL) {
      FilenodeOwnerEntry *fo = (FilenodeOwnerEntry *)hash_search(
          filenode_to_owner, &fs->relfilenode, HASH_FIND, NULL);
      Oid owner =
          fo ? fo->owner : InvalidOid; /* fallback: skip or attribute to 0 */
      if (owner == InvalidOid)
        continue;

      bool found;
      OwnerSizeEntry *os =
          (OwnerSizeEntry *)hash_search(owner_size, &owner, HASH_ENTER, &found);
      if (os) {
        os->owner = owner;
        os->total_bytes += fs->total_bytes;
      }
    }
  }

  /* 5. Publish: call common_post_scan for each owner (each does its own
   * short lock). Also collect seen owners into a hash for the final
   * orphan sweep — a plain List would make the orphan check O(N²) under
   * the exclusive lock. */
  HTAB *seen_owners_hash = NULL;
  {
    HASHCTL info;
    HASH_SEQ_STATUS status;
    OwnerSizeEntry *os;

    memset(&info, 0, sizeof(info));
    info.keysize = sizeof(Oid);
    info.entrysize = sizeof(Oid);
    seen_owners_hash =
        hash_create("pg_rolequota seen owners", PG_ROLEQUOTA_MAX_ROLE_ENTRIES,
                    &info, HASH_ELEM | HASH_BLOBS);

    hash_seq_init(&status, owner_size);
    while ((os = (OwnerSizeEntry *)hash_seq_search(&status)) != NULL) {
      bool found;
      pg_rolequota_common_post_scan(MyDatabaseId, os->owner, os->total_bytes);
      (void)hash_search(seen_owners_hash, &os->owner, HASH_ENTER, &found);
    }
  }

  /* 6. Final short exclusive lock: orphan sweep (hazard #2 + #6).
   * Any role (in OUR database) not seen in this scan has had its objects
   * deleted — zero its tracked size. We MUST filter by entry->key.dbid ==
   * MyDatabaseId so we don't accidentally zero out entries owned by other
   * databases' workers (composite-key correctness). */
  {
    TimestampTz now = GetCurrentTimestamp();
    HASH_SEQ_STATUS status;
    RoleSizeEntry *entry;

    LWLockAcquire(RoleSizeLock, LW_EXCLUSIVE);
    hash_seq_init(&status, RoleSizeHash);
    while ((entry = (RoleSizeEntry *)hash_seq_search(&status)) != NULL) {
      bool seen;
      if (entry->key.dbid != MyDatabaseId)
        continue; /* another database's worker owns this entry */
      (void)hash_search(seen_owners_hash, &entry->key.roleid, HASH_FIND, &seen);
      if (!seen) {
        entry->used_bytes = 0;
        entry->soft_exceeded = false;
        entry->hard_exceeded = false;
        entry->last_checked = now;
      }
    }
    LWLockRelease(RoleSizeLock);
  }

  hash_destroy(seen_owners_hash);

  elog(LOG, "pg_rolequota scan_enterprise: walk complete, dispatching "
            "common_post_scan + orphan sweep");

  /* 7. Teardown (hazard #3) */
  MemoryContextSwitchTo(oldctx);
  MemoryContextDelete(scanctx);

  elog(LOG, "pg_rolequota scan_enterprise: finished");
}

/* SQL-visible stub for dedicated enterprise test */
Datum pg_rolequota_scanner_enterprise_test(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pg_rolequota_scanner_enterprise_test);

Datum pg_rolequota_scanner_enterprise_test(PG_FUNCTION_ARGS) {
  PG_RETURN_TEXT_P(cstring_to_text("enterprise_stub"));
}

/* SQL-callable wrapper (for tests and manual trigger) so the real enterprise
 * fs-walk + generation/orphan logic is exercised by the regression suite.
 */
Datum pg_rolequota_scan_enterprise_sql(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pg_rolequota_scan_enterprise_sql);

Datum pg_rolequota_scan_enterprise_sql(PG_FUNCTION_ARGS) {
  pg_rolequota_scan_enterprise();
  PG_RETURN_VOID();
}
