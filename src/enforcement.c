/* src/enforcement.c
 * ExecutorCheckPerms_hook + ClientAuthentication_hook.
 * Phase 4 heart of the feature.
 * 2-space.
 */

#include "postgres.h"
#include "fmgr.h"
#include "executor/executor.h"
#include "libpq/auth.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"           /* GetUserId */
#include "nodes/parsenodes.h"    /* RangeTblEntry, RTE_RELATION, ACL_* */
#include "utils/syscache.h"      /* SearchSysCache1, RELOID */
#include "catalog/pg_class.h"    /* Form_pg_class */
#include "access/htup_details.h" /* GETSTRUCT, HeapTupleIsValid */
#include "storage/lwlock.h"
#include "utils/timestamp.h"     /* TimestampTz for duplicated RoleSizeEntry */
#include "catalog/pg_authid.h"   /* for get_role_oid in ClientAuthentication */
#include "commands/dbcommands.h" /* get_database_oid */

#include "compat.h"

/* Previous hook pointers for chaining (standard pattern) */
static ExecutorCheckPerms_hook_type prev_ExecutorCheckPerms_hook = NULL;
static ClientAuthentication_hook_type prev_ClientAuthentication_hook = NULL;

/* Shmem aliases (defined in shmem.c, populated after ensure) */
extern HTAB *RoleSizeHash;
extern HTAB *BlockedRolesHash;
extern LWLock *RoleSizeLock;

/* Ensure shmem (from shmem.c) */
void pg_rolequota_ensure_shmem(void);

/* Struct definitions duplicated for member access (must match shmem.c) */
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

/* Hook implementations */

/* Bitmask of permissions that count as "writes" (i.e. operations that can
 * grow the role's on-disk footprint and must therefore be blocked when the
 * role is over its hard quota). SELECT is intentionally excluded — reads
 * cannot increase storage and must always be allowed (in particular,
 * blocking SELECT would break catalog access, including the scanner's own
 * SPI queries and any admin tooling). */
#define PG_ROLEQUOTA_WRITE_PERMS                                               \
  (ACL_INSERT | ACL_UPDATE | ACL_DELETE | ACL_TRUNCATE)

/*
 * Core violation check logic.
 *
 * Semantics (the production-correct rule):
 *   - The quota is per-role: it limits how much storage that role can own.
 *   - We block the *current user* from writing once they are over their
 *     hard limit. We do NOT block reads (SELECT), and we do NOT block writes
 *     based on the table's owner. A role can still write to tables owned by
 *     other roles (because that doesn't grow this role's footprint), and
 *     other roles can still read this role's tables.
 *   - The current user is taken from GetUserId() (respects SET ROLE).
 *
 * Returns true iff the current user is hard-exceeded AND the statement
 * touches at least one relation with a write-class permission requirement.
 */
static bool pg_rolequota_check_for_quota_violation(List *rangeTable,
                                                   List *rtePermInfos) {
  /* C89 / PG -Wdeclaration-after-statement: all decls at absolute top of block
   */
  RoleQuotaKey lookup_key;
  bool any_write;
  bool hard_exceeded;
  ListCell *lc;
  RoleSizeEntry *entry;

  (void)
      rangeTable; /* range table is consulted only via rtePermInfos in PG18+ */

  /* Fast bail: if no permission info, we can't tell read vs write — be safe
   * and don't block (avoids breaking commands that don't go through the
   * normal perms pipeline). */
  if (rtePermInfos == NIL)
    return false;

  any_write = false;
  foreach (lc, rtePermInfos) {
    RTEPermissionInfo *perminfo = (RTEPermissionInfo *)lfirst(lc);
    if (perminfo->requiredPerms & PG_ROLEQUOTA_WRITE_PERMS) {
      any_write = true;
      break;
    }
  }
  if (!any_write)
    return false;

  /* Read this backend's quota state under a brief shared lock. The hash
   * is keyed by (MyDatabaseId, GetUserId()) — a role's quota is scoped to
   * the database whose worker BGW populated the entry. */
  lookup_key.dbid = MyDatabaseId;
  lookup_key.roleid = GetUserId();
  hard_exceeded = false;
  LWLockAcquire(RoleSizeLock, LW_SHARED);
  entry =
      (RoleSizeEntry *)hash_search(RoleSizeHash, &lookup_key, HASH_FIND, NULL);
  if (entry != NULL && entry->hard_bytes > 0 &&
      entry->used_bytes > entry->hard_bytes)
    hard_exceeded = true;
  LWLockRelease(RoleSizeLock);

  return hard_exceeded;
}

/* Modern hook signature (PG 14+): bool (List*, List*, bool)
 * We always use this version because the project requires PG 14+ and
 * the typedef in executor.h matches on all supported versions.
 */
static bool pg_rolequota_ExecutorCheckPerms(List *rangeTable,
                                            List *rtePermInfos,
                                            bool ereport_on_violation) {
  pg_rolequota_ensure_shmem();

  if (RoleSizeHash == NULL || RoleSizeLock == NULL) {
    /* shmem unavailable (not in shared_preload or startup) — chain only */
    if (prev_ExecutorCheckPerms_hook)
      return prev_ExecutorCheckPerms_hook(rangeTable, rtePermInfos,
                                          ereport_on_violation);
    return true;
  }

  bool violation =
      pg_rolequota_check_for_quota_violation(rangeTable, rtePermInfos);

  if (violation) {
    if (ereport_on_violation)
      ereport(ERROR,
              (errcode(ERRCODE_DISK_FULL),
               errmsg("disk quota exceeded for a role under hard limit")));
    /* Respect hook contract: return denial status when not ereporting. */
    return false;
  }

  /* Chain to previous hook (standard safe pattern) */
  if (prev_ExecutorCheckPerms_hook)
    return prev_ExecutorCheckPerms_hook(rangeTable, rtePermInfos,
                                        ereport_on_violation);
  return true;
}

static void pg_rolequota_ClientAuthentication(Port *port, int status) {
  /* Real 'lock' policy enforcement.
   * Only act on successful auth paths (STATUS_OK or after password/etc).
   * Resolve user name to role oid outside the lock, then do a brief
   * shared lock probe of BlockedRolesHash.
   */
  pg_rolequota_ensure_shmem();

  if (BlockedRolesHash == NULL || RoleSizeLock == NULL) {
    if (prev_ClientAuthentication_hook)
      prev_ClientAuthentication_hook(port, status);
    return;
  }

  /* Only check after the authentication phase has basically succeeded */
  if (status != STATUS_OK && status != STATUS_EOF) {
    if (prev_ClientAuthentication_hook)
      prev_ClientAuthentication_hook(port, status);
    return;
  }

  /* Resolve role name to oid safely via syscache (outside lock) */
  Oid roleid = InvalidOid;
  Oid dbid = InvalidOid;
  {
    HeapTuple roletup =
        SearchSysCache1(AUTHNAME, CStringGetDatum(port->user_name));
    if (HeapTupleIsValid(roletup)) {
      roleid = ((Form_pg_authid)GETSTRUCT(roletup))->oid;
      ReleaseSysCache(roletup);
    }
  }
  if (!OidIsValid(roleid)) {
    if (prev_ClientAuthentication_hook)
      prev_ClientAuthentication_hook(port, status);
    return;
  }

  /* Resolve database name → oid. The lock is scoped per-(database, role)
   * so a role can be locked in one DB without affecting their access to
   * another DB they own data in. ClientAuthentication runs before
   * MyDatabaseId is set; port->database_name is the target DB the client
   * is connecting to. */
  dbid = get_database_oid(port->database_name, true /* missing_ok */);
  if (!OidIsValid(dbid)) {
    if (prev_ClientAuthentication_hook)
      prev_ClientAuthentication_hook(port, status);
    return;
  }

  /* Brief shared lock check on the composite key. */
  {
    RoleQuotaKey lookup_key;
    bool blocked = false;
    BlockedRoleEntry *entry;

    lookup_key.dbid = dbid;
    lookup_key.roleid = roleid;

    LWLockAcquire(RoleSizeLock, LW_SHARED);
    entry = (BlockedRoleEntry *)hash_search(BlockedRolesHash, &lookup_key,
                                            HASH_FIND, NULL);
    if (entry != NULL)
      blocked = true;
    LWLockRelease(RoleSizeLock);

    if (blocked) {
      ereport(FATAL,
              (errcode(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION),
               errmsg("role \"%s\" is locked due to disk quota violation in "
                      "database \"%s\"",
                      port->user_name, port->database_name)));
    }
  }

  if (prev_ClientAuthentication_hook)
    prev_ClientAuthentication_hook(port, status);
}

/* Install hooks - called from _PG_init in main module (Phase 4) */
void pg_rolequota_install_hooks(void) {
  prev_ExecutorCheckPerms_hook = ExecutorCheckPerms_hook;
  ExecutorCheckPerms_hook = pg_rolequota_ExecutorCheckPerms;

  prev_ClientAuthentication_hook = ClientAuthentication_hook;
  ClientAuthentication_hook = pg_rolequota_ClientAuthentication;

  elog(LOG, "pg_rolequota enforcement: hooks installed (real "
            "ExecutorCheckPerms hard-limit checks active for Slice 1)");
}

/* Test helper */
Datum pg_rolequota_enforcement_test(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(pg_rolequota_enforcement_test);

Datum pg_rolequota_enforcement_test(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(true);
}
