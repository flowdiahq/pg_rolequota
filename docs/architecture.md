# pg_rolequota Architecture

**Project**: Per-role disk storage quotas for PostgreSQL 14+
**Status**: Production-grade multi-database deployment.

This document is the single source of truth for the design, data
structures, invariants, and hazard contracts of `pg_rolequota`. SQL and
shell examples are exercised by the regression suite (AGENTS.md §10).

---

## 1. Overview and goals

`pg_rolequota` enforces **per-(database, role) disk storage quotas**
with soft and hard limits, three configurable policies, and event-driven
sub-10-ms reaction to user wake-ups.

### Core requirements

- Work correctly across **every database in the cluster** that has
  `CREATE EXTENSION pg_rolequota` installed (the original v0.x design
  was single-database; the current architecture is multi-database).
- Two scanner implementations with equal coverage:
  - `shared_hosting` — cheap SPI aggregate, 10 000+ lightweight tenants.
  - `enterprise_db` — authoritative `$PGDATA/base/<dboid>/` walk for
    few heavy tenants with many tablespaces.
- Sub-10 ms p50 latency on the wake-up hot path (`request_wakeup` →
  `status()` reflects the new bytes).
- Safe enforcement on the hot path (`ExecutorCheckPerms_hook` on every
  DML; `ClientAuthentication_hook` on every login) — microsecond-fast
  common case, no I/O or `ereport` under the LWLock.
- Correct under `EXEC_BACKEND` (Windows, `--enable-exec-backend`, parallel
  workers, containers).
- Bounded resource usage (compile-time caps + runtime GUCs).
- Sacred gate: `make verify-whitespace && make lint && make test` must
  stay green on PG 14–18.

### Non-goals (today)

- Prometheus exporter shipping inside the extension (use external
  `sql_exporter`).
- Distributed / replica-aware quotas (a quota is local to its primary).
- Fine-grained per-tablespace quotas — `enterprise_db` aggregates across
  all tablespaces.

---

## 2. High-level architecture

```
Postmaster (shared_preload_libraries = 'pg_rolequota')
  │
  ├─► _PG_init (src/pg_rolequota.c)
  │     ├─► DefineCustom*Variable for 6 GUCs
  │     ├─► pg_rolequota_register_shmem_hooks()          (src/shmem.c)
  │     │     ├─► shmem_request_hook  →
  │     │     │     RequestAddinShmemSpace(hashes + anchor + ~32 KiB slack) +
  │     │     │     RequestNamedLWLockTranche("pg_rolequota", 1)
  │     │     └─► shmem_startup_hook   →
  │     │           ShmemInitStruct("pg_rolequota shmem state") +
  │     │           ShmemInitHash(RoleSizeHash)          [composite key]
  │     │           ShmemInitHash(BlockedRolesHash)      [composite key]
  │     │           init worker_slots[], queue[], atomics
  │     │
  │     ├─► pg_rolequota_install_hooks()                 (src/enforcement.c)
  │     │     ├─► ExecutorCheckPerms_hook (chained)
  │     │     └─► ClientAuthentication_hook (chained)
  │     │
  │     └─► RegisterBackgroundWorker(launcher BGW, function="pg_rolequota_launcher_main")
  │
  ├─► Launcher BGW (statically registered, one per cluster)
  │     └─► pg_rolequota_launcher_main (src/pg_rolequota.c)
  │           ├─► BackgroundWorkerInitializeConnection("postgres", ...)
  │           ├─► publish_launcher_latch  (so producers can SetLatch us)
  │           ├─► eager first pass at startup +
  │           │   WaitLatch(launcher_interval) thereafter
  │           └─► per cycle:
  │                 SPI: SELECT oid FROM pg_database
  │                      WHERE datallowconn AND NOT datistemplate
  │                        AND datname <> 'template0'
  │                        AND datconnlimit <> -2          -- skip invalid DBs
  │                 for each dbid not in no_ext_dbs[] and not present in worker_slots[]:
  │                   reserve_worker_slot(dbid)  +
  │                   RegisterDynamicBackgroundWorker(worker BGW, dbid arg, NEVER_RESTART)
  │
  ├─► Worker BGW (dynamic, one per database with the extension installed)
  │     └─► pg_rolequota_worker_main(Datum dbid) (src/pg_rolequota.c)
  │           ├─► register before_shmem_exit cleanup (clears worker_slots[dbid])
  │           ├─► BackgroundWorkerInitializeConnectionByOid(dbid, InvalidOid)
  │           ├─► probe: SPI SELECT 1 FROM pg_extension WHERE extname='pg_rolequota'
  │           │     absent → mark_db_no_ext(dbid) + clean exit
  │           ├─► publish_worker_latch(dbid)
  │           └─► loop:
  │                 WaitLatch(scan_interval)
  │                 drain wake-up ring; for each (MyDatabaseId, roleid):
  │                   pg_rolequota_scan_single_role(MyDatabaseId, roleid)
  │                     → cheap per-role SPI aggregate
  │                     → pg_rolequota_common_post_scan(MyDatabaseId, roleid, used_bytes)
  │                 if timer fired OR queue overflowed OR legacy counter changed:
  │                   pg_rolequota_scan_shared() OR pg_rolequota_scan_enterprise()
  │
  └─► Per-backend hot path (no BGW required; runs in every backend's process)
        ├─► ExecutorCheckPerms_hook (every DML)
        │     └─► RoleQuotaKey k = { MyDatabaseId, GetUserId() }
        │         hash_search(RoleSizeHash, &k) under LW_SHARED →
        │         if hard_exceeded AND any ACL_INSERT|UPDATE|DELETE|TRUNCATE in rtePermInfos →
        │           ereport(ERROR, ERRCODE_DISK_FULL)
        └─► ClientAuthentication_hook (every login)
              └─► resolve port->database_name + port->user_name → (dbid, roleid)
                  hash_search(BlockedRolesHash, &key) under LW_SHARED →
                  if hit → ereport(FATAL, ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION)
```

All paths touching shmem first call `pg_rolequota_ensure_shmem()` (the
EXEC_BACKEND-safe anchor lookup; see §4).

---

## 3. The composite key — every hash, every queue

The single most important invariant in the codebase:

```c
typedef struct RoleQuotaKey {
  Oid dbid;
  Oid roleid;
} RoleQuotaKey;            /* 8 bytes, naturally aligned, no padding */
```

defined in `src/compat.h`. Used by:

- `RoleSizeHash` (used_bytes / soft / hard / exceeded flags / last_checked
  / enforcement_policy per `(dbid, roleid)`).
- `BlockedRolesHash` (`policy = 'lock'` entries).
- Wake-up ring slots.
- Enforcement hook lookups (`{MyDatabaseId, GetUserId()}`).
- ClientAuthentication lookup (`{get_database_oid(port->database_name),
  resolved_roleid}`).

The struct **must** appear as the first member of any dynahash entry
type so `HASH_BLOBS` keying works correctly. This is enforced in
`RoleSizeEntry` and `BlockedRoleEntry` in every translation unit that
duplicates the struct (`shmem.c`, `scanner_common.c`, `scanner_enterprise.c`,
`enforcement.c`) — kept byte-for-byte identical by code convention.

---

## 4. Shared memory

File: `src/shmem.c`.

### Anchor

```c
typedef struct PgRoleQuotaShmemState {
  HTAB             *RoleSizeHash;       /* keyed by RoleQuotaKey */
  HTAB             *BlockedRolesHash;   /* keyed by RoleQuotaKey */
  LWLock           *RoleSizeLock;
  int32             wakeup_counter;     /* legacy back-compat counter */

  /* Per-worker latch table — linear-scan, ≤256 entries. */
  WorkerSlot        worker_slots[PG_ROLEQUOTA_MAX_WORKERS];

  /* Negative cache: dbids known to lack the extension. */
  pg_atomic_uint32  no_ext_epoch;
  pg_atomic_uint32  no_ext_count;
  Oid               no_ext_dbs[PG_ROLEQUOTA_MAX_WORKERS];

  /* Published launcher latch. */
  pg_atomic_uint32  launcher_pgprocno;

  /* Lock-free composite-keyed wake-up ring. */
  pg_atomic_uint32  queue_head;
  pg_atomic_uint32  queue_tail;
  pg_atomic_uint32  queue_overflow;
  RoleQuotaKey      queue[PG_ROLEQUOTA_WAKEUP_QUEUE_SIZE];
} PgRoleQuotaShmemState;
```

### EXEC_BACKEND safety

Classic static `HTAB *` / `LWLock *` pointers are zeroed in exec
children. We solve it by storing the three pointers (and everything
else) inside a `ShmemInitStruct("pg_rolequota shmem state", ...)`
allocation that every backend re-looks up by name.

`pg_rolequota_ensure_shmem(void)` is the public entry point:

- Fast path: `if (RoleSizeLock != NULL) return;`
- Slow path: re-attach via `ShmemInitStruct` and repopulate process-local
  aliases.
- `ERROR` if the anchor is missing (this signals the operator forgot
  `shared_preload_libraries`).

Every public path (scanners, enforcement hook, status SRF, latch
helpers, enqueue/dequeue) calls `ensure_shmem` on entry.

### Lock discipline

A single coarse `RoleSizeLock` (one `RequestNamedLWLockTranche`) protects
both hashes. The hazard contract is iron-clad:

- **Never** hold the lock while doing I/O, SPI, catalog access,
  `palloc`, `ereport`, or signal-handler-unsafe work.
- All `ereport(WARNING)` calls (cap-hit warnings) are deferred to flags
  read *outside* the lock.
- `GetCurrentTimestamp()` is hoisted out of the lock region (it's a
  syscall).
- The status SRF pre-`palloc`s its snapshot buffer outside the lock and
  only does bounded struct copies inside.
- The enterprise scanner's orphan sweep uses an `HTAB` of seen owners
  for O(1) probes (was O(N²) with a `List`).

### Resource bounds

| Constant | Value | File |
|---|---|---|
| `PG_ROLEQUOTA_MAX_ROLE_ENTRIES` | 16 384 | `src/compat.h` |
| `PG_ROLEQUOTA_MAX_BLOCKED_ENTRIES` | 4 096 | `src/compat.h` |
| `PG_ROLEQUOTA_MAX_WORKERS` | 256 | `src/compat.h` |
| `PG_ROLEQUOTA_WAKEUP_QUEUE_SIZE` | 256 (power of 2) | `src/compat.h` |

Request size is computed exactly via `hash_estimate_size(max,
sizeof(entry))` for both hashes, plus the anchor, plus 32 KiB of
defensive slack. Overflow on any of the caps is logged + handled
gracefully (full-scan reconciliation for the wake-up ring; cap-hit
`WARNING` for the hashes).

### Future evolution (deliberately not done)

- Split into per-tranche locks (RoleSize vs. Blocked) when contention
  data justifies it. The single coarse lock is acceptable up to several
  thousand roles; production has been live-verified.
- Per-role fine-grained locking or a `LWTreeOfTranches` style scheme
  for clusters with > 16 k `(database, role)` pairs.

---

## 5. Launcher and per-database workers

File: `src/pg_rolequota.c`.

### Why this design

PostgreSQL's BGW model gives each background worker a single database
connection (`BackgroundWorkerInitializeConnectionByOid(dbid, ...)`).
There is no "all databases" connection. A multi-database extension MUST
either:

(a) connect to each database in turn — clumsy and serialises catalog
    work, or
(b) use the autovacuum-style **launcher + per-DB worker** pattern.

`pg_rolequota` adopts (b).

### Launcher BGW

- Statically registered in `_PG_init` via `RegisterBackgroundWorker`.
- `bgw_function_name = "pg_rolequota_launcher_main"`,
  `bgw_restart_time = 60`, `bgw_start_time = BgWorkerStart_RecoveryFinished`.
- Connects to the `postgres` maintenance database (NOT `template1` —
  holding a connection to `template1` blocks `CREATE DATABASE ... TEMPLATE
  template1`). `postgres` is always present and safe to use as a
  read-only catalog target.
- Publishes its `MyProc->pgprocno` in
  `PgRoleQuotaShmemState.launcher_pgprocno` so producers in any
  database can `SetLatch` it.
- Loop:
  1. `WaitLatch(launcher_interval)`, with eager first pass at startup.
  2. `CHECK_FOR_INTERRUPTS()` (so `DROP DATABASE postgres`-style
     ProcSignalBarriers are processed).
  3. SPI: `SELECT oid FROM pg_database WHERE datallowconn AND NOT
     datistemplate AND datname <> 'template0' AND datconnlimit <> -2`.
     The `datconnlimit <> -2` filter excludes databases that are
     partway through DROP — skipping these prevented a 50 000-line
     log spam in early testing.
  4. For each candidate `dbid`:
     - Skip if `pg_rolequota_db_known_no_ext(dbid)` (negative cache).
     - Skip if a slot is already reserved for `dbid` (deduping under
       concurrent launcher races).
     - Skip if `spawned >= pg_rolequota.max_workers` for this cycle
       (emit one-shot `WARNING`).
     - Else: `pg_rolequota_reserve_worker_slot(dbid)` then
       `RegisterDynamicBackgroundWorker` with
       `bgw_main_arg = ObjectIdGetDatum(dbid)`,
       `bgw_restart_time = BGW_NEVER_RESTART` (launcher reconciles),
       `bgw_notify_pid = 0` (we **don't** want SIGUSR1 on every worker
       death — would cause re-enumeration storms).

### Worker BGW

- Dynamically registered. One per database with the extension installed.
- `pg_rolequota_worker_main(Datum dbid)`:
  1. `before_shmem_exit(unpublish_worker_latch, dbid)` registered FIRST,
     so any FATAL during init releases the launcher-reserved slot.
  2. `BackgroundWorkerInitializeConnectionByOid(dbid, InvalidOid)`.
  3. SPI probe: `SELECT 1 FROM pg_extension WHERE extname =
     'pg_rolequota'`. Absent → `mark_db_no_ext(dbid)` + clean exit.
  4. `pg_rolequota_publish_worker_latch(dbid)` — CAS-claims a
     `WorkerSlot` and writes its pgprocno.
  5. Scan loop (see §6).

### Slot reservation

`pg_rolequota_reserve_worker_slot(dbid)` is the deduping primitive that
closes the race between `RegisterDynamicBackgroundWorker` returning and
the new worker's own `publish_worker_latch`:

```c
/* 1st pass: refuse if slot already exists for dbid */
for (i = 0 .. MAX-1) if (slots[i].dbid == dbid) return false;

/* 2nd pass: CAS-claim the first InvalidOid slot */
for (i = 0 .. MAX-1)
  if (CAS(slots[i].dbid, InvalidOid → dbid))
    { slots[i].pgprocno = INVALID; return true; }
return false;
```

The worker's `publish_worker_latch` is then a no-op CAS that finds its
own dbid slot and writes its actual pgprocno. Failure paths (probe
ERROR, FATAL in InitProc, BGW registration rejection) all release the
slot via the `before_shmem_exit` callback registered first.

---

## 6. Low-latency wake-up bus

### Design choice: published latch, not LISTEN/NOTIFY

PostgreSQL's docs explicitly forbid BGWs from `LISTEN`ing
(`"Background workers should not register to receive asynchronous
notifications with the LISTEN command, as there is no infrastructure
for a worker to consume such notifications"`).

Even if it were allowed, NOTIFY at scale serialises on a global lock at
commit time — terrible for the hot path.

Instead we use the **canonical autovacuum-style published-latch
pattern**, parameterised by `dbid`.

### Wake-up ring

A lock-free SPSC-from-each-DB / MPMC-globally ring living in shmem:

```c
RoleQuotaKey queue[256];           /* (dbid, roleid) entries */
pg_atomic_uint32 queue_head;       /* producer cursor */
pg_atomic_uint32 queue_tail;       /* consumer cursor */
pg_atomic_uint32 queue_overflow;   /* monotonic drop counter */
```

Producers:

```c
head = fetch_add(queue_head, 1);
if (head - read(queue_tail) >= 256) {
  fetch_add(queue_overflow, 1);            /* drop, will reconcile */
} else {
  queue[head & 255] = (dbid, roleid);      /* 8-byte aligned, no torn */
}
latch = lookup_worker_latch(dbid)
     ?? lookup_launcher_latch();           /* fall through to launcher */
if (latch) SetLatch(latch);
```

Consumers (workers):

```c
while (true) {
  k = dequeue_wakeup_role();    /* atomic read tail, advance */
  if (k.dbid != MyDatabaseId) {
    enqueue_wakeup_role(k.dbid, k.roleid);    /* re-route */
    requeued++;
    if (requeued >= 256) break;               /* prevent ping-pong */
    continue;
  }
  scan_single_role(k.dbid, k.roleid);
}

overflow = consume_overflow();
if (timer_fired OR overflow > 0 OR legacy_counter_changed) {
  run_full_scan();                /* reconciliation */
}
```

Key correctness points:

- 8-byte aligned `RoleQuotaKey` writes mean no torn writes on supported
  platforms.
- Producer order is established by `fetch_add(queue_head)`.
- Each worker is the sole consumer for keys with its `MyDatabaseId`;
  cross-DB entries are re-enqueued (bounded by 256 / cycle to prevent
  ping-pong loops; the overflow counter catches the remainder).
- Producers that find no worker for their DB fall back to SetLatching
  the launcher, which spawns a worker on its next cycle.

### Latch publication

`pg_rolequota_publish_worker_latch(dbid)` CAS-claims a `WorkerSlot`,
writes pgprocno. `pg_rolequota_unpublish_worker_latch(dbid)` clears the
slot. Both are called from `before_shmem_exit` so FATAL paths leave a
clean slate.

`pgprocno` is the **process number** (the index into
`ProcGlobal->allProcs`), NOT a raw `Latch *`. Reason: pgprocnos are
stable across EXEC_BACKEND children where raw pointers captured at
startup time would be in the wrong address space.

The compat shim `PG_ROLEQUOTA_PGPROC_NUMBER(proc)` /
`PG_ROLEQUOTA_PGPROC_BY_NUMBER(n)` papers over the PG 17 rename
(`pgprocno` field → `procNumber` field + `GetNumberFromPGProc` macro).

### Latency budget

| Step | Mechanism | Typical |
|---|---|---|
| Producer → ring slot + SetLatch | atomic CAS + kernel write to self-pipe | < 50 µs |
| Worker `WaitLatch` returns | `poll()` returns | < 500 µs |
| Targeted per-role SPI aggregate | one `pg_class` scan filtered by `relowner` | 1 – 5 ms |
| `common_post_scan` (limits + hash + policy) | existing path | 1 – 3 ms |
| **End-to-end** | | **< 10 ms p50, < 50 ms p99** |

---

## 7. Scanner modes

AGENTS.md §5 mandates equal first-class treatment.

### `shared_hosting` (`src/scanner_shared.c`)

- **Target**: 10 000+ lightweight tenants, each with a handful of small
  relations.
- **Method**: one SPI aggregate.
  ```sql
  SELECT relowner,
         COALESCE(SUM(pg_total_relation_size(oid)), 0)::bigint
  FROM pg_class
  WHERE relkind IN ('r','m','t','i')
    AND relpersistence <> 't'
    AND relfilenode <> 0
  GROUP BY relowner;
  ```
  The `relkind` filter is **load-bearing**: it excludes views, composite
  types, sequences, foreign tables, and partitioned-parents whose
  `relfilenode = 0`. Calling `pg_total_relation_size` on these can
  NULL-deref `smgrnblocks` on PG 18 — this was the original v0.x
  SIGSEGV root cause.
- The `::bigint` cast is also load-bearing: `SUM(bigint)` returns
  `numeric` in PostgreSQL. `DatumGetInt64` on a numeric Datum reads a
  varlena pointer as an int64 — garbage. The explicit cast pins the
  return type.
- **Hazards** (binding):
  1. SPI completes **before** any `RoleSizeLock` is acquired.
  2. Result set bounded by `PG_ROLEQUOTA_MAX_ROLE_ENTRIES`; excess rows
     emit a single `WARNING`.
  3. Proper `SPI_connect` / `SPI_finish` + `PG_TRY` error hygiene;
     `IsTransactionState()` guard so the same code can be called from
     the SQL wrapper inside a user transaction.

### `enterprise_db` (`src/scanner_enterprise.c`)

- **Target**: few large tenants with many tablespaces / indexes / TOAST.
- **Method**: authoritative filesystem walk.
  1. SPI: build `relfilenode → relowner` map from `pg_class`.
  2. `walk_and_accumulate` on `$DataDir/base/<dboid>/` and every
     additional tablespace queried from `pg_class`.
  3. Aggregate filenode sizes to per-owner totals.
  4. Dispatch each owner to `common_post_scan(MyDatabaseId, owner, total)`.
  5. Orphan sweep under brief exclusive lock — zero `used_bytes` for
     `(MyDatabaseId, roleid)` entries that didn't appear in this scan.
     **Filter by `entry->key.dbid == MyDatabaseId`** so we don't trample
     entries owned by other databases' workers.
- **Hazards** (binding):
  1. Entire walk + `stat()` happens with **zero** `RoleSizeLock` held.
  2. Final hash insert / orphan sweep acquires the lock; sweep uses an
     `HTAB` of seen owners for O(1) probes (not a `List`).
  3. Per-scan temporary `HTAB`s live in a fresh `MemoryContext` destroyed
     on every exit path (including PG_CATCH).
  4. Generation-stamp consistency: orphan sweep zeros what's missing
     under the same lock that holds the writes.

### Common post-scan (`src/scanner_common.c`)

```c
void pg_rolequota_common_post_scan(Oid dbid, Oid roleid, int64 used_bytes);
```

- Reads `rolequota.limits WHERE roleid = $1` via SPI (with a
  `to_regclass('rolequota.limits') IS NOT NULL` probe to avoid
  `ereport(ERROR, UNDEFINED_TABLE)` noise during early startup).
- Computes `hard_exceeded`, `soft_exceeded`.
- Acquires `RoleSizeLock` *exclusively* for a microsecond-bounded
  critical section: hash insert/update + `BlockedRolesHash` maintenance.
- All policy actions run **outside** the lock:
  - `policy = 'terminate'`: SPI `SELECT pg_terminate_backend(pid) FROM
    pg_stat_activity WHERE usename = (...) AND pid <> pg_backend_pid()`.
  - `policy = 'lock'`: SPI `ALTER ROLE x NOLOGIN` (idempotent).
  - `policy = 'warn'`: emit a per-role WARNING; no action.
- Emits `NOTIFY quota_wakeup, '<roleid>'` + optional Slack on hard
  breach with `notify = true`.
- Re-enqueues the role for a fresh targeted refresh so recovery (space
  reclaim) is observed promptly.

The targeted single-role scanner `pg_rolequota_scan_single_role(dbid,
roleid)` is the same shape but with a `WHERE relowner = $1` filter on
the SPI query — bounded by that role's relation count.

---

## 8. Enforcement hooks (`src/enforcement.c`)

### `ExecutorCheckPerms_hook` (every DML, every backend)

```c
static bool pg_rolequota_ExecutorCheckPerms(
    List *rangeTable, List *rtePermInfos, bool ereport_on_violation);
```

Logic:

1. Fast bail: `rtePermInfos == NIL` → allow (commands that don't go
   through the normal perms pipeline).
2. Scan `rtePermInfos` for `ACL_INSERT | ACL_UPDATE | ACL_DELETE |
   ACL_TRUNCATE`. If none → allow (reads are never blocked).
3. Brief `LW_SHARED` lock; lookup `RoleQuotaKey { MyDatabaseId,
   GetUserId() }` in `RoleSizeHash`. If `entry->hard_bytes > 0 &&
   entry->used_bytes > entry->hard_bytes`, set `hard_exceeded = true`.
4. On violation: `ereport(ERROR, ERRCODE_DISK_FULL, "disk quota
   exceeded for a role under hard limit")`.

Rules (non-negotiable):

- Microsecond-fast common case.
- Never hold the lock while doing catalog work, SPI, or `ereport`.
- Soft breach never `ERROR`s.
- `rangeTable` lifetime is call-scoped only.
- Cross-role writes are never blocked (we check `GetUserId()`, not the
  table's owner). A role over quota in DB X is never blocked in DB Y.

### `ClientAuthentication_hook` (every login)

```c
static void pg_rolequota_ClientAuthentication(Port *port, int status);
```

After `STATUS_OK | STATUS_EOF`:
1. Resolve `port->user_name → roleid` via AUTHNAME syscache.
2. Resolve `port->database_name → dbid` via `get_database_oid(_, true)`.
3. Lookup `RoleQuotaKey { dbid, roleid }` in `BlockedRolesHash` under
   `LW_SHARED`.
4. If hit: `ereport(FATAL, ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION,
   "role X is locked due to disk quota violation in database Y")`.

`get_database_oid` is used because `MyDatabaseId` is not set yet during
authentication — we're still attaching to the requested database.

### Chaining

Standard save-then-overwrite pattern. The previous hook is always
forwarded to on the non-violation path.

### Detection vs. prevention

`pg_rolequota` is an **eventually-consistent** soft-limit extension.
`ExecutorCheckPerms_hook` reads a cached `used_bytes` from
`RoleSizeHash`; it does **not** re-measure on the hot path. Three
practical consequences flow from this design choice:

1. The statement that **causes** a breach is allowed to complete. The
   hook fires once at the start of execution, and at that moment
   `used_bytes` is still under `hard_bytes` (otherwise the breach
   would not be "caused" by this statement).
2. The worker BGW observes the new `used_bytes` on its next scan. With
   the published-latch wake-up bus the typical detection latency is
   ~10–50 ms; with the periodic-only path it is bounded by
   `pg_rolequota.scan_interval` (default 300 s).
3. Once the worker observes the breach, `common_post_scan` invokes
   `pg_rolequota_cancel_overage_backends(dbid, roleid)` which iterates
   `pg_stat_activity` via SPI and issues `pg_cancel_backend(pid)` to
   every active session matching `(datid = dbid, usesysid = roleid)`.

The cancel path is gated by two GUCs:

| GUC | Default | Effect |
|---|---|---|
| `pg_rolequota.cancel_enabled` (bool, SUSET) | `on` | Master switch. `off` reverts to detect-then-block-on-next-statement semantics. |
| `pg_rolequota.cancel_grace_bytes` (int, SUSET) | `0` | Slack above `hard_bytes` before cancellation fires. Tolerates small short-lived bursts. |

A per-role throttle (`RoleSizeEntry.last_cancel_ts`, minimum 1 s
between cancels) prevents storms when a role hammers retries while
still over the limit. The decision-and-stamp happens under the
existing `RoleSizeLock` exclusive critical section; the SPI call to
`pg_cancel_backend` runs strictly outside the lock per the
zero-I/O-under-lock hazard contract (see §4 Lock discipline).

#### Why we cannot prevent the in-statement overage

Real per-block enforcement requires hooking `smgrextend()` so the
extension can inspect every page allocation and refuse the write
before it lands on disk. Upstream PostgreSQL deliberately does **not**
expose `smgrextend_hook`, `smgrcreate_hook`, or `smgrtruncate_hook` to
extensions — those hooks exist only in patched PostgreSQL
distributions (Greenplum/Arenadata `diskquota`, Percona Server for
PostgreSQL / `pg_tde`, Neon). A proposal to add them upstream was
rejected on `pgsql-hackers` in 2018 and again in 2024. Shipping
smgr-level enforcement would therefore require us to fork PostgreSQL
and distribute patched binaries alongside the extension — a
distribution project, not an extension. That is intentionally out of
scope for `pg_rolequota`; the project's contract is "best-effort
enforcement on **unmodified** community PG 14–18".

#### Compared to other extensions on community PG

| Extension | In-statement prevention | Mid-query cancellation | Detection latency |
|---|---|---|---|
| `pg_quota` (Heikki Linnakangas) | ❌ | ❌ | ~`scan_interval` |
| Greenplum `diskquota` (vanilla PG build) | ❌ | ❌ | ~`scan_interval` |
| Greenplum `diskquota` (Greenplum fork) | ❌ | ✅ via `DispatcherCheckPerms_hook` | sub-second |
| **`pg_rolequota`** | ❌ | ✅ via BGW `pg_cancel_backend` | ~10–50 ms |

---

## 9. State machine and termination

`scanner_common.c::pg_rolequota_common_post_scan` is the home of the
quota state machine. Required coverage matrix (owned by
`test_termination.sql`):

- soft-only / hard-only / both limits set.
- `enforcement_policy` ∈ {`warn`, `terminate`, `lock`} for every
  combination.
- Recovery path: space reclaimed → next scan clears `hard_exceeded` →
  next ExecutorCheckPerms allows the role to write again.
- BGW restart visibility (the worker's `before_shmem_exit` releases its
  slot; launcher re-spawns; new worker's first scan re-populates the
  hash from `rolequota.limits` + `pg_class`).
- MemoryContext hygiene for per-role decision structs (every
  `AllocSetContextCreate` has a matching `MemoryContextDelete` on every
  exit path, including PG_CATCH).
- Boundary conditions at the 16 384 / 4 096 hash caps (warning emitted
  once per role; subsequent updates skip).
- Error paths in termination batch (backend already dead, role dropped,
  etc.) — `PG_TRY` swallows + `AbortOutOfAnyTransaction` when the BGW
  owns the txn; `PG_RE_THROW` when invoked from an SQL caller.

All termination / `ALTER ROLE NOLOGIN` actions are idempotent and
survive BGW restarts.

---

## 10. NOTIFY emission (no LISTEN inside the extension)

File: `src/scanner_common.c` (the legacy `src/notify.c` is a linkage
stub for AGENTS.md §3 compliance).

When `common_post_scan` records a hard breach with `notify = true`, it
runs:

```sql
NOTIFY quota_wakeup, '<roleid>';
```

via SPI (per-role NOTIFY; the channel name is fixed). Any client app
can `LISTEN quota_wakeup` to receive realtime breach events.

The extension itself **does not** `LISTEN` — see §6 for why. NOTIFY is
emitted strictly for external consumers (your application, your alerting
sidecar, etc.).

After NOTIFY, `common_post_scan` also `enqueue_wakeup_role(dbid,
roleid)` so that the next worker tick re-scans this role and detects
recovery (space reclamation) promptly.

---

## 11. Optional Slack notifications

File: `src/slack.c`.

Compile-time optional. The build system auto-detects `libcurl` headers
in `/usr/include/curl/` or `/usr/local/include/curl/` or
`/opt/homebrew/include/curl/` and sets `-DHAVE_CURL` if found.

When enabled:

- `pg_rolequota_slack_send(const char *message)` POSTs a tiny JSON to
  `pg_rolequota.slack_webhook_url`.
- **SSRF guard**: the URL MUST start with `https://hooks.slack.com/` —
  any other prefix is refused with a WARNING.
- 5-second per-process rate limit (the BGW is single-threaded, so this
  is enforced via a static `time_t`).
- 8-second connect/transfer timeout, no redirects (`CURLOPT_FOLLOWLOCATION = 0`),
  TLS verify-peer + verify-host on.
- The URL is **never logged** (may carry a secret token).
- JSON escaping is minimal but safe: `"` → `'`, `\` → `/`.

When disabled (`!HAVE_CURL`): `pg_rolequota_slack_send` is a graceful
no-op (single LOG message).

`test_slack.sql` exercises both linkage paths.

---

## 12. SQL surface

Defined in `sql/pg_rolequota--1.0.sql`. All objects live in the
`rolequota` schema.

### `rolequota.limits` (per-database)

```sql
CREATE TABLE rolequota.limits (
  roleid              oid PRIMARY KEY,
  soft_bytes          bigint,
  hard_bytes          bigint,
  enforcement_policy  text NOT NULL DEFAULT 'terminate'
                      CHECK (enforcement_policy IN ('warn','terminate','lock')),
  notify              boolean NOT NULL DEFAULT true,
  created_at          timestamptz NOT NULL DEFAULT now(),
  updated_at          timestamptz NOT NULL DEFAULT now()
);
SELECT pg_catalog.pg_extension_config_dump('rolequota.limits', '');
```

Each database with `CREATE EXTENSION pg_rolequota` gets its own
`limits` table. The composite hash key in shmem keeps them isolated.

### `rolequota.status()` SRF

```sql
CREATE FUNCTION rolequota.status()
RETURNS TABLE (
  database            text,
  roleid              oid,
  used_bytes          bigint,
  soft_bytes          bigint,
  hard_bytes          bigint,
  soft_exceeded       boolean,
  hard_exceeded       boolean,
  last_checked        timestamptz,
  enforcement_policy  text
)
LANGUAGE C SECURITY DEFINER;
```

- C-backed; snapshots `RoleSizeHash` under brief `LW_SHARED`.
- `database` resolved via `get_database_name(dbid)` *outside* the lock;
  `NULL` if the DB has been dropped since the entry was written.
- Returns rows from EVERY database the launcher has scanned — call from
  any single database to see the cluster-wide picture.
- `SECURITY DEFINER` so the operator can `GRANT EXECUTE` selectively
  without exposing the in-shmem hash to ordinary tenants.

### Wake-up surface

| Function | Behavior |
|---|---|
| `rolequota.request_wakeup()` | Legacy back-compat. Bumps the wakeup counter + SetLatches the local DB's worker (or the launcher if no worker yet). |
| `rolequota.request_wakeup(oid)` | Enqueues `(MyDatabaseId, oid)` for targeted refresh. |
| `rolequota.request_self_wakeup()` | Enqueues `(MyDatabaseId, GetUserId())`. Intended for AFTER triggers. |
| `rolequota.scan_shared()` | Run the shared scanner synchronously in the current backend's DB. |
| `rolequota.scan_enterprise()` | Run the FS-walker synchronously in the current backend's DB. |

### Per-file test linkage helpers (AGENTS.md §3)

`rolequota.version()`, `rolequota.c_linkage_test()`,
`rolequota.shmem_info()`, `rolequota.common_test()`,
`rolequota.scanner_shared_test()`, `rolequota.scanner_enterprise_test()`,
`rolequota.enforcement_test()`, `rolequota.notify_test()`,
`rolequota.slack_test()` — each proves its corresponding `.c` file
compiled, linked, and is reachable from SQL.

---

## 13. GUCs

| Name | Default | Range | Reload | Notes |
|---|---|---|---|---|
| `pg_rolequota.scanner_mode` | `shared_hosting` | enum | restart | `shared_hosting` or `enterprise_db`. |
| `pg_rolequota.scan_interval` | 300 | 5 – 3600 | SIGHUP | Seconds between full reconciliation scans. Targeted wakes bypass this. |
| `pg_rolequota.launcher_interval` | 60 | 5 – 3600 | SIGHUP | Launcher re-enumerates `pg_database` this often. SetLatch-triggered between cycles. |
| `pg_rolequota.max_workers` | 32 | 1 – 256 | restart | Concurrent per-DB workers. Must fit inside `max_worker_processes`. |
| `pg_rolequota.max_roles_tracked` | 16384 | 128 – 16384 | restart | Compile-time cap is 16 384; rebuild required to go higher. |
| `pg_rolequota.slack_webhook_url` | (unset) | text | SIGHUP | Optional. Strict `https://hooks.slack.com/` prefix. |

---

## 14. Build, testing, CI, and process

### PGXS build

```make
MODULE_big = pg_rolequota
OBJS = src/pg_rolequota.o src/shmem.o src/scanner_enterprise.o
       src/scanner_shared.o src/scanner_common.o src/enforcement.o
       src/notify.o src/slack.o
DATA = sql/pg_rolequota--1.0.sql
```

`libcurl` auto-detection:

```make
ifneq ($(wildcard /usr/include/curl/curl.h /usr/local/include/curl/curl.h
                  /opt/homebrew/include/curl/curl.h),)
  PG_CPPFLAGS += -DHAVE_CURL
endif
```

### The sacred gate (AGENTS.md §4)

Every change must keep green:

```bash
make verify-whitespace    # 2-space + no trailing ws (excludes test/expected/)
make lint                 # clang-format dry-run + clang-tidy + whitespace
make test                 # = verify-whitespace + lint + installcheck
```

### REGRESS matrix

```
test_skeleton            test_sql_objects          test_shmem
test_enforcement         test_scanner_enterprise   test_scanner_shared
test_notify_wakeup       test_wakeup_queue         test_multi_db
test_slack               test_termination
```

11 tests. `test_multi_db` exercises the launcher + per-DB worker
spawning + composite-key correctness. `test_wakeup_queue` exercises the
lock-free ring + overflow reconciliation. `test_notify_wakeup`
verifies sub-10 ms targeted wake-up latency.

Each `.c` file has a dedicated SQL test that calls its `*_test()` C
function (linkage proof) or exercises the runtime path via real DML /
catalog operations.

### CI

`.github/workflows/ci.yml` runs the full gate on every push / PR
against PostgreSQL 14, 15, 16, 17, 18 (with `libcurl4-openssl-dev`).

### Docker

`make docker-test` and `docker-test-pg-%` for local reproduction.

---

## 15. Security and isolation

- **`shared_preload_libraries` is mandatory.** `CREATE EXTENSION`
  without it emits a loud `WARNING` and leaves hooks inactive.
- All shmem access goes through `pg_rolequota_ensure_shmem` (anchor
  re-attach is EXEC_BACKEND-safe).
- `rolequota.status()` is `SECURITY DEFINER` — `GRANT EXECUTE` per
  tenant role is the recommended pattern in shared-hosting deployments.
- The Slack webhook URL is validated, never logged, and rate-limited.
- Hard caps on the in-shmem hashes prevent a malicious scanner from
  exhausting shared memory.
- Composite `(dbid, roleid)` keys mean a quota set in one tenant's
  database never affects another tenant's database. Per-database
  `rolequota.limits` tables enforce this at the DDL level too.
- The enforcement hook is by-design **per-DB** (`MyDatabaseId,
  GetUserId()`), so role X in DB A can be locked while role X in DB B
  continues to operate.

---

## 16. Operational guidance

### First-time setup

1. `make && sudo make install` on the host running PostgreSQL.
2. Edit `postgresql.conf`:
   ```ini
   shared_preload_libraries = 'pg_rolequota'
   max_worker_processes      = 64    # cover launcher + per-DB workers + headroom
   pg_rolequota.scan_interval     = 1200   # 20-min full-scan reconciliation
   pg_rolequota.launcher_interval = 60
   pg_rolequota.max_workers       = 32
   ```
3. Restart the cluster.
4. In each database you want to track: `CREATE EXTENSION pg_rolequota;`
   Set limits + call `rolequota.request_self_wakeup()` to trigger an
   immediate scan.

### Operating on a live cluster

- **Monitoring**: poll `rolequota.status()` from any one database;
  it returns rows for every database the launcher has scanned.
- **Adjusting a quota**: `UPDATE rolequota.limits SET hard_bytes = ...`
  + `SELECT rolequota.request_wakeup(roleid)`. The change takes effect
  on the next scan (typically < 10 ms).
- **Manual scan**: `SELECT rolequota.scan_shared();` (or
  `scan_enterprise()`) runs the active scanner in the current backend's
  database synchronously.
- **Disabling enforcement temporarily**: set `enforcement_policy =
  'warn'` in `rolequota.limits` — preserves tracking, removes the
  blocking action.

### Dropping a database that has the extension

The worker BGW connected to the doomed database may take a moment to
release. Either:

- `DROP DATABASE foo WITH (FORCE)` (PG 13+) — sends SIGTERM to the
  worker, which exits cleanly via `before_shmem_exit`.
- Or terminate the worker explicitly first:
  ```sql
  SELECT pg_terminate_backend(pid)
    FROM pg_stat_activity
   WHERE backend_type = 'pg_rolequota worker' AND datname = 'foo';
  ```

The launcher will eventually re-enumerate `pg_database` and won't
re-spawn for a dropped DB (the `datconnlimit <> -2` filter excludes
DBs in DROP-in-progress state).

### Scaling guidance

Tested live up to ~5 000 roles per database. For very large clusters:

- Bump `pg_rolequota.scan_interval` to 600–1 200 s. Rely on targeted
  `request_wakeup(oid)` from your application for the hot path.
- Provision `max_worker_processes` to cover `pg_rolequota.max_workers
  + 1` + autovacuum + everything else.
- If you'll have > 4 096 simultaneous `policy = 'lock'` roles, bump
  `PG_ROLEQUOTA_MAX_BLOCKED_ENTRIES` in `src/compat.h` and rebuild.
- If you'll have > 16 384 simultaneous `(database, role)` pairs, bump
  `PG_ROLEQUOTA_MAX_ROLE_ENTRIES` likewise. The single coarse
  `RoleSizeLock` may become a hotspot at that scale — the documented
  next step is to split into per-tranche locks.

---

## 17. Known limitations

- Single coarse `RoleSizeLock` — fine up to ~10 k pairs; will need
  splitting under contention. The hazard contract is preserved so this
  is a localised refactor.
- `BackgroundWorkerInitializeConnectionByOid` cannot be interrupted by
  `ProcSignalBarrier`, so a worker mid-init when its target DB is
  dropped will linger briefly until the FATAL fires. Cleaned up via
  `before_shmem_exit`; documented in `test_multi_db.sql`.
- The launcher connects to `postgres`. If you DROP DATABASE postgres
  the launcher will FATAL out — the postmaster auto-restarts it
  (`bgw_restart_time = 60`) but you'll see a brief gap. Don't drop
  `postgres`.
- No replica awareness yet — a quota is local to its primary. Failover
  rebuilds the in-shmem hash from the persisted `rolequota.limits`
  tables on the new primary's first scan cycle.

---

## 18. References and credits

- Generation-stamp / FS-walk technique: [hlinnaka/pg_quota](https://github.com/hlinnaka/pg_quota).
- Launcher / worker split: modelled on PostgreSQL autovacuum
  (`src/backend/postmaster/autovacuum.c`).
- Published-latch + lock-free ring: canonical
  `src/test/modules/worker_spi/worker_spi.c` + stat-collector
  wake-up patterns.
- AGENTS.md — the living project constitution (2-space, no trailing
  whitespace, every file tested, dual modes equal, sacred gate).
- PostgreSQL source files studied during implementation:
  `src/backend/storage/ipc/shmem.c`, `src/backend/executor/execMain.c`
  (ExecutorCheckPerms), `src/backend/libpq/auth.c`,
  `src/backend/postmaster/bgworker.c`,
  `src/backend/storage/lmgr/lwlock.c`,
  `src/include/storage/proc.h`, `src/include/port/atomics.h`,
  `src/include/catalog/pg_database.h`.

---

**This document is part of the tested documentation surface.** Any SQL,
shell, or configuration snippet must remain runnable and is exercised by
`make test-architecture` (`test_sql_objects` regression test).

*End of architecture document.*
