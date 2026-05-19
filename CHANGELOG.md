# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added — mid-query cancellation on hard breach

- **BGW-side `pg_cancel_backend()` after every scan.** When
  `common_post_scan` observes `used_bytes > hard_bytes + cancel_grace_bytes`,
  the worker iterates `pg_stat_activity` for matching `(datid, usesysid)`
  and issues `pg_cancel_backend(pid)` to every active session owned by
  the offending role (excluding the worker itself). Cancellation lands at
  the backend's next `CHECK_FOR_INTERRUPTS` tick — typically sub-millisecond
  once issued. End-to-end detection-to-cancel latency is **~10–50 ms** on
  the wake-up bus path.
- **Two new GUCs**:
  - `pg_rolequota.cancel_enabled` (bool, default `on`, SUSET) — master
    switch. Setting to `off` restores the v0.x detect-then-block-on-next-
    statement semantics.
  - `pg_rolequota.cancel_grace_bytes` (int, default `0`, range 0..1e9,
    SUSET) — slack above `hard_bytes` before cancellation fires. Tolerates
    small short-lived bursts without killing the session.
- **Per-role cancel throttle** (`RoleSizeEntry.last_cancel_ts`, minimum
  1 s between cancels per role) prevents cancel storms during retry
  bursts.
- **New regression test** `test_mid_query_cancel.sql` exercises the
  cancel path end-to-end: GUC registration, both GUC values, the
  hard-breach state, the no-op-grace-too-high path, and the disabled
  path. (Real multi-session cancellation is verified by the
  `examples/quick-demo.sql` live demo, which is outside pg_regress'
  single-session model.)

### Changed

- `RoleSizeEntry` struct grew by one `TimestampTz last_cancel_ts` field
  (8 bytes). This is an additive shmem-layout change — a cluster
  restart is required after upgrading the on-disk library, same as for
  every other shmem-anchor change in this release.

### Honest semantics (new documentation)

- README and `docs/architecture.md` gained a **Detection vs. prevention**
  subsection that explicitly documents what the extension does and does
  not do: the breaching statement always completes, but the session is
  cancelled within ~50 ms and follow-up statements are rejected. The
  reason (no upstream `smgrextend_hook` on community PG) and the
  alternatives (FS-level quotas, statement_timeout, etc.) are spelled
  out.

### Added — multi-database launcher (production fix)

- **Per-database BGW architecture.** The single hard-coded
  `postgres`-connected BGW is replaced by a launcher + dynamic
  per-database worker design (modelled on autovacuum).
  - **Launcher** BGW connects to the `postgres` maintenance database,
    enumerates `pg_database` every `pg_rolequota.launcher_interval`
    seconds (default 60), and spawns one dynamic worker per database
    that has `CREATE EXTENSION pg_rolequota` installed.
  - **Worker** BGW per database: probes `pg_extension`, runs the
    targeted-refresh ring + periodic full-scan loop scoped to its DB.
- **Composite `(database, role)` hash key** throughout shmem, the
  wake-up ring, and both enforcement hooks. Roles in different
  databases are correctly isolated.
- **`rolequota.status()` gained a leading `database text` column**
  resolved via `get_database_name(dbid)` outside the lock. Returns rows
  for every database the launcher has scanned.
- **Per-DB latch table** (`worker_slots[256]`) so producers SetLatch
  the correct worker; falls back to SetLatching the launcher when no
  worker yet exists for the caller's DB (triggers immediate spawn).
- **Negative cache** of databases known to lack the extension prevents
  re-spawning probe workers every cycle.
- **Slot reservation** via CAS (`pg_rolequota_reserve_worker_slot`)
  closes the spawn-vs-publish race that previously caused worker-spawn
  storms when a database was repeatedly probed.
- **New regression test** `test_multi_db.sql` exercises the full
  launcher + per-DB worker + composite-key path with two extra
  databases.
- **Two new GUCs**: `pg_rolequota.launcher_interval` (60 s default) and
  `pg_rolequota.max_workers` (32 default, cap 256).

### Fixed — multi-DB correctness

- The original v0.x BGW only saw `pg_class` of the `postgres` database.
  Limits set in any other database were never enforced —
  `rolequota.status()` showed nothing for those roles, the hook never
  fired, and writes were unrestricted. This was the production-blocking
  bug fixed by the launcher / per-DB worker split.
- The shmem hash key was `Oid roleid`, so role X in DB A and role X in
  DB B would have trampled each other had we ever extended to
  multi-DB. The composite `RoleQuotaKey { dbid, roleid }` permanently
  fixes this.
- Launcher's `pg_database` query now filters `datconnlimit <> -2`
  (databases in DROP-in-progress state). Without this filter a
  half-dropped database would cause an endless worker-spawn storm.
- Workers set `bgw_notify_pid = 0` (the default) — previously the
  launcher received SIGUSR1 on every worker death, causing tight
  re-enumeration loops on any batch failure.
- `ClientAuthentication_hook` now resolves `port->database_name →
  dbid` via `get_database_oid` before the composite-key lookup. The
  block decision is correctly per-database.

### Changed

- `pg_rolequota_common_post_scan` signature: `(Oid roleid, int64
  used_bytes)` → `(Oid dbid, Oid roleid, int64 used_bytes)`.
- `pg_rolequota_scan_single_role` signature: `(Oid roleid)` → `(Oid
  dbid, Oid roleid)`.
- `pg_rolequota_enqueue_wakeup_role` signature: `(Oid roleid)` → `(Oid
  dbid, Oid roleid)`.
- `pg_rolequota_dequeue_wakeup_role` now writes a `RoleQuotaKey *`
  instead of `Oid *`.

### Migration

- **Cluster restart required** after upgrading to this release — the
  in-shmem binary layout changed (composite key + worker latch table +
  launcher latch).
- **`DROP EXTENSION pg_rolequota; CREATE EXTENSION pg_rolequota;`** in
  every database that had the old extension, to pick up the
  9-column `rolequota.status()` SRF.
- Existing `rolequota.limits` row data is preserved (the table schema
  did not change).

---

## [0.2.0] — Low-latency wake-up mechanism

### Added

- **Published-latch + lock-free SPSC ring queue** for sub-10 ms wake-up
  latency (canonical autovacuum / worker_spi pattern).
- Worker publishes its `MyProc->pgprocno` in shmem on startup;
  producers `SetLatch` it directly. EXEC_BACKEND-safe (pgprocno is
  stable across address spaces).
- `pg_rolequota.request_wakeup(oid)` — enqueue a single role for
  **targeted** refresh; the worker runs a per-role SPI aggregate
  instead of a full pg_class scan.
- `pg_rolequota.request_self_wakeup()` — enqueue `current_user`'s oid.
- Wake-up ring: 256-slot lock-free SPSC; overflow → bumps counter →
  worker reconciles with one full scan.
- `BackgroundWorkerUnblockSignals()` + `CHECK_FOR_INTERRUPTS()` in
  the BGW main loop (fixes `DROP DATABASE` ProcSignalBarrier hang).
- `before_shmem_exit` callback that unpublishes the latch on any
  worker exit path.
- `BgWorkerStart_RecoveryFinished` (was `ConsistentState`) — avoids
  the SIGSEGV in early-recovery scans of catalog tables without primed
  relcache state.
- `to_regclass('rolequota.limits')` probe in `common_post_scan`
  silently skips the limits query when the extension is not yet
  installed in the worker's DB (avoids per-cycle ERROR log noise).
- Two new regression tests: `test_notify_wakeup` (latency assertion)
  and `test_wakeup_queue` (overflow + reconciliation).

### Fixed (the SIGSEGV)

- `pg_total_relation_size(oid)` is no longer called on relations
  without on-disk storage (views, composite types, foreign tables,
  partitioned-table parents). The `WHERE relkind IN ('r','m','t','i')
  AND relfilenode <> 0` filter prevents the NULL-deref in
  `smgrnblocks` on PG 18.
- `SUM(bigint)` returns `numeric` in PostgreSQL; the result is now
  explicitly cast to `bigint` via `COALESCE(..., 0)::bigint` so
  `DatumGetInt64` reads an actual integer instead of a varlena
  pointer.
- `AbortOutOfAnyTransaction` in the BGW outer `PG_CATCH` so leftover
  transaction state from a failed scan doesn't crash the next cycle.

### Changed

- Default `pg_rolequota.scan_interval` raised 60 s → 300 s (full scans
  are now a periodic safety net; wake-ups are event-driven).
- Per-call `IsTransactionState()` guard so `rolequota.scan_shared()`
  works correctly when invoked from inside a user transaction (the
  SQL wrapper path) AND when invoked from the BGW (which owns its
  own transaction).

---

## [0.1.0] — 2026-05 — Initial Public Preview

### Added

- Skeleton implementation of per-role disk quotas.
- Two scanner modes (`enterprise_db` + `shared_hosting`) with detailed
  hazard contracts.
- Shared memory design with `EXEC_BACKEND` safety
  (`PgRoleQuotaShmemState` anchor + `ShmemInitStruct` re-attach).
- `ExecutorCheckPerms_hook` and `ClientAuthentication_hook` (correct
  signatures, chaining).
- `LISTEN`/`NOTIFY` and optional Slack notification paths (stubs).
- Strict AGENTS.md process (2-space, no trailing whitespace, per-file
  tests, sacred `make test` gate).
- CI matrix on PostgreSQL 14–18.
- `docs/architecture.md` and owning regression test.

### Notes

This was the architectural preview / beta release. Real scanner bodies,
termination logic, and GUC configuration were the remaining work. See
the 0.2.0 and Unreleased sections for the production-grade
implementations that followed.
