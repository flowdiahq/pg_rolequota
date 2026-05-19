# pg_rolequota

Per-role disk storage quotas for PostgreSQL 14+.

[![CI](https://github.com/flowdiahq/pg_rolequota/actions/workflows/ci.yml/badge.svg)](https://github.com/flowdiahq/pg_rolequota/actions/workflows/ci.yml)
[![License: PostgreSQL](https://img.shields.io/badge/License-PostgreSQL-blue.svg)](LICENSE)

`pg_rolequota` tracks how much on-disk storage each role owns inside each
database, enforces configurable soft / hard limits, and reacts to breaches
in well under 10 ms via a published-latch wake-up bus.

The extension is **multi-database**: a launcher BGW discovers every
database that has the extension installed and spawns a dedicated worker
per database. A composite `(database, role)` shmem hash keeps state
correctly isolated across tenants.

---

## Highlights

- **Soft + hard quotas per `(database, role)` pair.**
- **Low-latency wake-up.** Any backend can call
  `rolequota.request_wakeup(roleid)` and the responsible worker reacts in
  < 10 ms p50 (published-latch + lock-free SPSC ring; no LISTEN/NOTIFY
  global lock).
- **Multi-database, multi-tenant aware.** One launcher BGW + one worker
  BGW per database with the extension installed. Composite
  `(databaseid, roleid)` keys throughout shmem, the wake-up ring, and the
  enforcement hooks. Workers spawn on demand.
- **Two scanner implementations**, both first-class:
  - `shared_hosting` — cheap SPI aggregate over `pg_class` +
    `pg_total_relation_size`, ideal for 10 000+ lightweight tenants.
  - `enterprise_db` — authoritative filesystem walk under
    `$PGDATA/base/<dboid>/` + every `pg_tblspc/`, ideal for a few heavy
    tenants with many relations and tablespaces.
- **Three enforcement policies**: `warn`, `terminate`, `lock`.
- **Mid-query cancellation.** Once a worker detects a hard breach it
  issues `pg_cancel_backend()` to every active session owned by the
  offending role — typically ~10–50 ms after the breaching statement
  returns. Tuneable via `pg_rolequota.cancel_enabled` /
  `pg_rolequota.cancel_grace_bytes`.
- **Enforcement on the hot path** via `ExecutorCheckPerms_hook` (every
  DML) and `ClientAuthentication_hook` (every login) — both
  microsecond-fast common case, no I/O under the lock.
- **EXEC_BACKEND-safe** shmem (`PgRoleQuotaShmemState` anchor +
  process-local cache repopulated via `ShmemInitStruct`).
- **Optional Slack notifications** (libcurl, compile-time optional with
  strict SSRF + rate-limit guards).
- **Tested.** 12 regression tests, the full
  `make verify-whitespace && make lint && make test` gate must stay
  green on PG 14–18.

---

## Quick start

1.  Build + install the extension on the host where PostgreSQL runs:

    ```bash
    make
    sudo make install        # uses pg_config from $PATH; override with PG_CONFIG=...
    ```

2.  Add to `shared_preload_libraries` and restart the cluster
    (required — the extension is inert without it; you'll see a `WARNING`
    if you forget):

    ```ini
    # postgresql.conf
    shared_preload_libraries = 'pg_rolequota'
    ```

3.  In **each database** you want to monitor, run:

    ```sql
    CREATE EXTENSION pg_rolequota;
    ```

    The launcher BGW polls `pg_database` every
    `pg_rolequota.launcher_interval` seconds (default 60) and is
    immediately woken via SetLatch by the first `request_wakeup` call
    from a new database — so workers come up within seconds of
    `CREATE EXTENSION`.

4.  Set a limit on a role:

    ```sql
    INSERT INTO rolequota.limits
      (roleid, soft_bytes, hard_bytes, enforcement_policy)
    VALUES
      ('app_user'::regrole::oid,
       10 * 1024 * 1024,            -- soft: 10 MB
       100 * 1024 * 1024,           -- hard: 100 MB
       'terminate');
    SELECT rolequota.request_self_wakeup();   -- ask for an immediate refresh
    ```

5.  Inspect the live state from any database that has the extension
    (the in-memory hash is cluster-wide; `status()` returns every
    `(database, role)` pair tracked):

    ```sql
    SELECT database, roleid::regrole::text AS role,
           pg_size_pretty(used_bytes::bigint) AS used,
           pg_size_pretty(hard_bytes::bigint) AS hard,
           hard_exceeded, soft_exceeded, last_checked
      FROM rolequota.status()
     ORDER BY database, roleid;
    ```

---

## SQL surface

All objects live in the `rolequota` schema, created by `CREATE EXTENSION
pg_rolequota`. The schema is per-database; `rolequota.limits` is also
per-database (each DB manages its own role list).

### Table

| Column | Type | Notes |
|---|---|---|
| `roleid` | `oid` PRIMARY KEY | from `pg_roles.oid` (use `'foo'::regrole::oid`) |
| `soft_bytes` | `bigint` NULL | `NULL` ⇒ no soft limit |
| `hard_bytes` | `bigint` NULL | `NULL` ⇒ no hard limit |
| `enforcement_policy` | `text` NOT NULL | one of `warn`, `terminate`, `lock` |
| `notify` | `boolean` NOT NULL | emit `NOTIFY quota_wakeup` + Slack on breach |
| `created_at` | `timestamptz` | auto |
| `updated_at` | `timestamptz` | auto |

`pg_extension_config_dump('rolequota.limits', '')` is registered so
`pg_dump` includes the table contents.

### Functions

| Function | Returns | Use |
|---|---|---|
| `rolequota.status()` | SRF, 9 cols: `database, roleid, used_bytes, soft_bytes, hard_bytes, soft_exceeded, hard_exceeded, last_checked, enforcement_policy` | Live snapshot of the in-shmem hash across all databases the launcher has scanned. |
| `rolequota.request_wakeup()` | `void` | Bump the legacy counter + SetLatch this database's worker (or the launcher if no worker yet). Triggers a full scan ASAP. |
| `rolequota.request_wakeup(oid)` | `void` | Enqueue `(MyDatabaseId, roleid)` for **targeted** refresh (per-role SPI aggregate). < 10 ms p50. |
| `rolequota.request_self_wakeup()` | `void` | Equivalent to `request_wakeup(current_user::regrole::oid)`. Intended for `AFTER` triggers. |
| `rolequota.scan_shared()` | `void` | Manually run the shared-hosting scanner in the current backend's database (useful from triggers or admin scripts). |
| `rolequota.scan_enterprise()` | `void` | Manually run the enterprise filesystem-walk scanner in the current database. |
| `rolequota.version()` | `text` | Extension version + PostgreSQL version string. |
| `rolequota.shmem_info()` | `int` | Diagnostic: returns `1` if shmem is wired correctly in this backend. |

### Channels

- `NOTIFY quota_wakeup, '<roleid>'` — emitted by `common_post_scan` when
  a hard breach + `notify=true` is recorded. Any backend can `LISTEN
  quota_wakeup` to react.

---

## Configuration GUCs

All GUCs live under the `pg_rolequota.` namespace. Set in
`postgresql.conf` or via `ALTER SYSTEM`.

| GUC | Default | Range | Reload | Meaning |
|---|---|---|---|---|
| `pg_rolequota.scanner_mode` | `shared_hosting` | `shared_hosting` \| `enterprise_db` | restart | Which scanner workers run on the periodic tick. |
| `pg_rolequota.scan_interval` | `300` (s) | 5 – 3600 | SIGHUP | Seconds between **full** reconciliation scans per worker. Targeted wake-ups (`request_wakeup(oid)`) bypass this entirely. |
| `pg_rolequota.launcher_interval` | `60` (s) | 5 – 3600 | SIGHUP | How often the launcher re-enumerates `pg_database` looking for newly-installed extension instances. Set-latched immediately by producers in databases without a worker. |
| `pg_rolequota.max_workers` | `32` | 1 – 256 | restart | Cap on concurrent per-database worker BGWs. Must be ≤ `max_worker_processes - 1` (one slot for the launcher). |
| `pg_rolequota.max_roles_tracked` | `16384` | 128 – 16384 | restart | Hard cap on `(database, role)` pairs in the in-shmem hash. Cap-hit emits a `WARNING`; excess pairs are silently skipped. |
| `pg_rolequota.slack_webhook_url` | (unset) | `https://hooks.slack.com/...` | SIGHUP | Optional. Requires `-DHAVE_CURL` build flag (auto-detected on Linux + Homebrew). |
| `pg_rolequota.cancel_enabled` | `on` | `on` \| `off` | SUSET | Master switch for the mid-query `pg_cancel_backend()` path that fires after a hard breach. Off restores v0.x detect-on-next-statement semantics. |
| `pg_rolequota.cancel_grace_bytes` | `0` | 0 – 1e9 | SUSET | Bytes of slack above `hard_bytes` before cancellation fires. Tolerates small short-lived bursts without killing the session. |

### Recommended PG-level config for production

```ini
# Match max_workers + launcher + headroom for autovacuum / parallel.
max_worker_processes = 64

# pg_rolequota tunables for a multi-tenant cluster
pg_rolequota.scan_interval     = 1200   # 20 min — rely on targeted wakes
pg_rolequota.launcher_interval = 60
pg_rolequota.max_workers       = 32
```

---

## Enforcement semantics

The hook fires on every DML and checks **only the current user**:

```
violation = (rtePermInfos has ACL_INSERT|UPDATE|DELETE|TRUNCATE)
         AND hash[(MyDatabaseId, GetUserId())].hard_exceeded
```

Implications:

- **Reads are never blocked.** A role over its hard limit can still
  `SELECT`, run admin queries, query the catalog, etc.
- **Cross-role writes are not affected.** Role A being over quota
  doesn't stop role B from writing to a table A happens to own.
- **Per-database isolation.** The same role with quotas in two databases
  is tracked separately; being over quota in DB X doesn't block writes
  in DB Y.
- **`policy = 'lock'`** additionally blocks new logins for the role in
  the breached database via `ClientAuthentication_hook` → `FATAL`.
- **`policy = 'terminate'`** runs `pg_terminate_backend` against every
  active session owned by the breached role (in that DB) on each scan
  cycle. Combined with the hook this means subsequent writes are
  rejected AND any existing transactions are killed.
- **`policy = 'warn'`** records the breach in `status()` but takes no
  enforcement action; intended for monitoring + alerting.

### Detection vs. prevention

`pg_rolequota` is an **eventually-consistent** soft-limit extension. The
enforcement hook checks a cached `used_bytes` value updated by the worker
BGW; it does not re-measure the role's footprint on the hot path.

What that means in practice for a role capped at `hard_bytes = N`:

- The single statement that **causes** the breach (e.g. one big
  `INSERT … SELECT generate_series(…)` that pushes `used_bytes` from
  `N - 100` MB to `N + 900` MB) is **not pre-empted**. It started below
  the limit and completes successfully.
- The worker observes the breach on its next scan tick. With the
  published-latch wake-up bus the typical latency from the breaching
  statement returning to the worker's `status()` row being updated is
  **~10–50 ms**.
- Once the worker observes the breach it issues `pg_cancel_backend(pid)`
  to every **active** session owned by the role in that database
  (controlled by `pg_rolequota.cancel_enabled`, default `on`). Any
  follow-up statement by the same role's already-open session hits the
  enforcement hook and is rejected immediately. New connections see the
  refreshed state from the start.
- The `policy = 'terminate'` and `policy = 'lock'` post-scan actions
  layer on top: terminate kills the sessions outright, lock additionally
  bars new logins.

In short, the **breach statement always completes**, but its session is
cancelled within tens of milliseconds and the **next** statement is
rejected. This matches the semantics every other PostgreSQL quota
extension on community PG ultimately delivers — true hard prevention of
a single-statement overage would require per-block (`smgrextend`) hooks
that vanilla PostgreSQL does not expose to extensions (see _Why not
prevent the overage entirely?_ below).

#### Tuning the mid-query cancel path

| GUC                                  | Default | Purpose                                                                                                |
| ------------------------------------ | ------- | ------------------------------------------------------------------------------------------------------ |
| `pg_rolequota.cancel_enabled`        | `on`    | Master switch. Set to `off` to fall back to detect-then-block-on-next-statement semantics.             |
| `pg_rolequota.cancel_grace_bytes`    | `0`     | Slack (bytes) added to `hard_bytes` before cancellation fires. Lets you absorb small short-lived bursts. |

A per-role throttle (1 s) prevents cancellation storms when a role
hammers retries while still over the limit.

#### Mitigating the gap further

If you need tighter than ~50 ms reaction or stricter than "cancel after
the fact":

- Lower `pg_rolequota.scan_interval` (down to 5 s safely) and rely on
  the wake-up bus for first-class events.
- Add `AFTER INSERT / COPY` statement triggers on user tables that
  call `rolequota.request_self_wakeup()`. Their next statement then
  sees fresh state with no scan-cycle wait.
- Pair `pg_rolequota` with statement-level safeguards:
  `statement_timeout`, `work_mem`, `temp_file_limit`.
- For adversarial workloads, place per-role tablespaces on a filesystem
  with quota support (XFS / ZFS quotas) — the OS rejects writes once
  the role's tablespace hits the FS limit, irrespective of the
  extension. `pg_rolequota` then becomes the policy / notification
  layer on top of FS-enforced bytes.

#### Why not prevent the overage entirely?

Real per-block enforcement requires hooking `smgrextend()` (the storage
manager's "extend relation by one page" call) so the extension can
inspect every page allocation and refuse the write before it lands on
disk. Upstream PostgreSQL does **not** expose `smgrextend_hook`,
`smgrcreate_hook`, or `smgrtruncate_hook` to extensions — these hooks
exist only in **patched** PostgreSQL distributions:

- Greenplum / Arenadata `diskquota` (Greenplum fork).
- Percona `pg_tde` (Percona Server for PostgreSQL — Community PG plus
  two patches).
- Neon's pluggable SMGR backend (Neon's fork).

A proposal to add these hooks to upstream PG was discussed on
`pgsql-hackers` in 2018 and again in 2024; both attempts were rejected.
Shipping smgr-level enforcement therefore would require us to ship a
patched PostgreSQL binary alongside the extension — i.e. a distribution
project, not an extension. That's intentionally out of scope for
`pg_rolequota` v1.x; we focus on giving the best detect-and-cancel
story possible on **unmodified** community PostgreSQL 14–18.

### Recovery

Once a role's usage drops below the hard limit (admin deletes data,
relaxes the quota, etc.), the next scan clears `hard_exceeded`. The
enforcement hook reacts immediately (next DML); a locked role becomes
loginable again on the next scan + manual `ALTER ROLE ... LOGIN` (the
extension only flips to NOLOGIN, never silently back).

---

## Latency profile

Measured end-to-end (`request_wakeup(roleid)` → shmem update visible in
`status()`):

| Scenario | p50 | p99 |
|---|---|---|
| Worker already alive for caller's DB | < 5 ms | < 20 ms |
| First call from a fresh `CREATE EXTENSION` DB (launcher must spawn worker) | ~ 50 ms | < 500 ms |
| Bursted 300 wake-ups (queue overflow → forced full scan) | < 250 ms (full scan) | depends on `pg_class` size |

Sources of latency in the targeted path:

1. `enqueue_wakeup_role` — atomic fetch_add + slot write + `SetLatch`. Sub-µs.
2. Worker's `WaitLatch` returns. Microseconds.
3. `pg_rolequota_scan_single_role` — one SPI aggregate over `pg_class`
   filtered by `relowner = $1`. Typically 1–5 ms depending on schema
   size.
4. `pg_rolequota_common_post_scan` — limits lookup + hash mutation
   under brief exclusive LWLock. Sub-ms.

---

## Scaling guidance

The implementation is comfortable up to the documented caps:

| Resource | Cap | Headroom at typical scale |
|---|---|---|
| Tracked `(database, role)` pairs | 16 384 | 5 000 tenants ≈ 30 % |
| Blocked `(database, role)` pairs | 4 096 | 1 000 locked roles ≈ 25 % |
| Per-DB worker BGWs | 256 (compile-time), `max_workers` GUC at runtime | 100 quota'd DBs ≈ default |
| Wake-up ring | 256 slots | Overflow triggers full scan — non-fatal |

For very large clusters (thousands of roles, dozens of databases):

- **Bump `pg_rolequota.scan_interval` to 600 – 1200 s.** Full scans
  become a safety-net; the hot path is targeted wake-ups from your
  application or triggers.
- **Provision `max_worker_processes`** to cover
  `pg_rolequota.max_workers + 1` (the launcher) + autovacuum + any
  other extensions. Default `max_worker_processes = 8` is too small for
  more than a handful of quota'd databases.
- **If you'll have > 4 096 `policy = 'lock'` roles concurrently**, bump
  `PG_ROLEQUOTA_MAX_BLOCKED_ENTRIES` in `src/compat.h` and rebuild. The
  cap is conservative DoS protection, not a fundamental limit.

The single `RoleSizeLock` is a documented future hotspot. At the
hash-cap scale (16 k pairs) the longest exclusive section is the
enterprise scanner's orphan sweep — bounded but not free. Production
splits into per-tranche locks if you observe contention; see
[architecture.md §4](docs/architecture.md#4-shared-memory).

### Hard ceiling — number of databases per cluster

**pg_rolequota v1.x supports up to 256 databases per cluster.** Past
that, enforcement silently degrades for the excess.

The launcher spawns one BGW per database that has the extension
installed. `PG_ROLEQUOTA_MAX_WORKERS` (compile-time constant in
`src/compat.h`) caps the worker pool at 256. If your cluster has, say,
500 databases with `pg_rolequota` installed, the first ~256 by
`pg_database.oid` order get workers; the remaining ~244 do not. For
those DBs:

- `rolequota.status()` returns no rows.
- The enforcement hook reads a stale (empty) `RoleSizeHash` entry, so
  writes are never blocked.
- Mid-query cancellation never fires.

The launcher emits one `WARNING` per cycle when it can't spawn for a
candidate database; check the postmaster log if you're near the limit.

This is an **architectural** ceiling, not a tuning knob. Raising
`PG_ROLEQUOTA_MAX_WORKERS` past 256 is not recommended because:

1. **PostgreSQL's `max_worker_processes` settles around a few hundred
   in practice.** Each BGW costs 3–10 MB of RSS and one PGPROC slot.
   1 000 workers ≈ 5–10 GB of memory used purely for the BGW pool,
   plus a proportional bump in every `O(max_connections)` shmem
   structure (lock tables, snapshot arrays). Cluster startup memory
   grows by hundreds of MB.
2. **OS process limits.** macOS's default `kern.maxproc` is 2 048;
   common Linux defaults under systemd are ~4 000. You hit those
   ceilings before PG does.
3. **One-BGW-per-DB is the standard PostgreSQL pattern.** Autovacuum
   limits itself to 3 workers by default for the same reason; Greenplum
   `diskquota` settles around a few hundred DBs per cluster. Past that,
   the pattern itself is wrong, not the cap.

#### Workarounds for clusters that genuinely need >256 tenants

| Situation | Recommended approach |
|---|---|
| You designed for "1 DB per tenant" but the tenants are roles/users, not full isolation units | Use **one DB with N roles**. pg_rolequota's composite `(database, role)` key tracks 5 000+ roles in one DB comfortably (16 384 hash-entry cap). |
| You need namespace isolation but not catalog isolation | Use **one DB with N schemas**. Per-schema quotas are not in v1.0 but are tracked for v1.1 — see [#per-schema-quotas](https://github.com/flowdiahq/pg_rolequota/issues). |
| You need full per-tenant Postgres clusters (separate catalogs, separate WAL, etc.) | A single-cluster extension cannot fit. Use one Postgres instance per tenant (k8s operator, managed service, etc.). |
| You're stuck with thousands of DBs in one cluster and need quotas now | The pool-of-workers redesign (planned v2.0) decouples worker count from DB count via time-slicing. Trade-off: mid-query cancellation latency degrades from ~50 ms to ~30 s. Open a GitHub issue with your scale + workload and we'll prioritise accordingly. |

In short: **pg_rolequota v1.x is designed for tens to a few hundred
databases per cluster.** If your deployment is structured around
thousands of databases, prefer one of the workarounds above to
re-shaping the extension.

---

## Building

### Prerequisites

- `postgresql-server-dev` matching your PG version (provides `pg_config`
  and headers).
- A C11 compiler (`clang` / `gcc`).
- `make`.
- *Optional* but recommended for development: `clang-format`,
  `clang-tidy`.

**macOS (Homebrew):**
```bash
brew install postgresql@18 clang-format
```

**Debian / Ubuntu:**
```bash
sudo apt install postgresql-server-dev-17 build-essential \
                 clang-format clang-tidy libkrb5-dev libcurl4-openssl-dev
```

(`libcurl4-openssl-dev` is optional; it enables the Slack webhook path
via auto-detected `-DHAVE_CURL`.)

### Build + install

```bash
make
sudo make install
```

If you have multiple PostgreSQL versions installed, point at a specific
`pg_config`:

```bash
make PG_CONFIG=/opt/homebrew/opt/postgresql@18/bin/pg_config
sudo make install PG_CONFIG=/opt/homebrew/opt/postgresql@18/bin/pg_config
```

### Migration / upgrades

`pg_rolequota` v1.0 is the **first stable release**. Earlier `0.x` builds
were preview / beta only and used incompatible shared-memory and SQL
layouts. There is no `ALTER EXTENSION pg_rolequota UPDATE` path from a
preview build — the supported migration is:

```sql
DROP EXTENSION pg_rolequota;   -- in every database that had a preview build
CREATE EXTENSION pg_rolequota; -- after restarting the cluster on v1.0
```

The cluster must be restarted after upgrading the on-disk library because
the in-shmem binary layout (composite key + worker latch table + launcher
latch) is not backwards compatible with preview versions. Existing
`rolequota.limits` row data is preserved across the `DROP` / `CREATE` —
the table schema itself did not change between preview and v1.0.

---

## Testing

The project's sacred gate is:

```bash
make verify-whitespace && make lint && make test
```

All three must be green for any change to land. `make test` runs
`verify-whitespace`, `lint`, and the full regression matrix:

| Test | Owns / exercises |
|---|---|
| `test_skeleton` | extension load + base SQL objects |
| `test_sql_objects` | schema, table, CHECK constraint, version() |
| `test_shmem` | EXEC_BACKEND-safe anchor + ensure |
| `test_enforcement` | `ExecutorCheckPerms_hook` rejection on hard breach |
| `test_scanner_shared` | shared_hosting SPI aggregate, relkind filter, numeric→bigint correctness |
| `test_scanner_enterprise` | enterprise FS-walker + orphan sweep |
| `test_notify_wakeup` | published-latch + targeted refresh path latency |
| `test_wakeup_queue` | lock-free ring + overflow reconciliation |
| `test_multi_db` | launcher + per-DB workers + composite `(dbid, roleid)` key |
| `test_slack` | libcurl path linkage (HAVE_CURL on / off) |
| `test_termination` | full soft/hard × warn/terminate/lock × recovery matrix |
| `test_mid_query_cancel` | BGW-side `pg_cancel_backend` path + `cancel_enabled` / `cancel_grace_bytes` GUC semantics |

Convenience targets:

```bash
make test-shared        # shared_hosting scanner only
make test-enterprise    # enterprise FS scanner only
make test-enforcement   # hook path
make test-wakeup        # wake-up latency + queue overflow
make test-multi-db      # launcher + per-DB workers
make test-all-modes     # shared + enterprise
make format             # auto-format C sources (clang-format)
make lint               # clang-tidy + clang-format check + whitespace
```

### Supported PostgreSQL versions

| Version | Status |
|---|---|
| 14 | ✅ CI |
| 15 | ✅ CI |
| 16 | ✅ CI (live-verified on a 5 k-role cluster) |
| 17 | ✅ CI |
| 18 | ✅ CI |

---

## Contributing

This project follows a strict process documented in [AGENTS.md](AGENTS.md):

- 2-space indentation, no trailing whitespace, anywhere.
- Every `.c` and `.sql` file you touch needs (or must update) a regression test.
- Both scanner modes are first-class — keep coverage symmetric.
- `make verify-whitespace && make lint && make test` must be green before opening a PR.

See [CONTRIBUTING.md](CONTRIBUTING.md) for the full process and
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) for community standards.

For design rationale, hazard contracts, and the multi-database wire
protocol, read [docs/architecture.md](docs/architecture.md).

---

## License

`pg_rolequota` is released under the [PostgreSQL License](LICENSE) — the
same license PostgreSQL itself uses.

---

## Credits

- Generation-stamp / FS-walk technique adapted from
  [hlinnaka/pg_quota](https://github.com/hlinnaka/pg_quota).
- Multi-database launcher / worker split modelled on PostgreSQL's
  autovacuum launcher.
- Published-latch + lock-free SPSC ring inspired by the canonical
  `worker_spi.c` and stat collector wake-up patterns in
  `src/backend/postmaster/`.
