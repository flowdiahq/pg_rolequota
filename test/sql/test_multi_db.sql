-- test_multi_db.sql
-- Exercises the multi-database launcher + per-DB worker architecture.
-- Creates two extra databases (rq_a, rq_b), installs the extension in each,
-- sets distinct quotas, writes data, and verifies that rolequota.status()
-- (called from any DB) shows both (database, role) pairs with the correct
-- used_bytes and hard_exceeded. AGENTS.md §3 ownership: this is the
-- dedicated test for the launcher / worker split in src/pg_rolequota.c
-- and the composite (dbid, roleid) hash key.
\echo '--- test_multi_db starting ---'

-- Always start with the extension visible in this DB (contrib_regression).
CREATE EXTENSION IF NOT EXISTS pg_rolequota;

-- Clean up any leftovers from prior test runs.
DROP DATABASE IF EXISTS rq_a;
DROP DATABASE IF EXISTS rq_b;
DROP ROLE IF EXISTS rq_multi_role_a;
DROP ROLE IF EXISTS rq_multi_role_b;

CREATE ROLE rq_multi_role_a LOGIN;
CREATE ROLE rq_multi_role_b LOGIN;

CREATE DATABASE rq_a;
CREATE DATABASE rq_b;

-- Install the extension + a limit row + a small owned table in rq_a.
\c rq_a
CREATE EXTENSION pg_rolequota;
GRANT CREATE ON SCHEMA public TO rq_multi_role_a;
CREATE TABLE public.rq_a_data (pad text);
ALTER TABLE public.rq_a_data OWNER TO rq_multi_role_a;
INSERT INTO public.rq_a_data
  SELECT repeat('a', 200) FROM generate_series(1, 100);
INSERT INTO rolequota.limits (roleid, hard_bytes, enforcement_policy, notify)
  VALUES ('rq_multi_role_a'::regrole::oid, 1024, 'terminate', false);
-- Force the first scan in rq_a so the role appears in shmem immediately
-- (otherwise we'd wait up to launcher_interval + scan_interval).
SELECT rolequota.scan_shared();

-- Same setup in rq_b with a different role + different limit.
\c rq_b
CREATE EXTENSION pg_rolequota;
GRANT CREATE ON SCHEMA public TO rq_multi_role_b;
CREATE TABLE public.rq_b_data (pad text);
ALTER TABLE public.rq_b_data OWNER TO rq_multi_role_b;
INSERT INTO public.rq_b_data
  SELECT repeat('b', 200) FROM generate_series(1, 100);
INSERT INTO rolequota.limits (roleid, hard_bytes, enforcement_policy, notify)
  VALUES ('rq_multi_role_b'::regrole::oid, 1024, 'terminate', false);
SELECT rolequota.scan_shared();

-- Switch back to contrib_regression and verify BOTH (db, role) pairs are
-- visible. The status() SRF returns rows from the in-shmem hash (which is
-- shared across all DBs), so we can see them from any database that has
-- the extension installed.
\c contrib_regression
\echo '--- status() shows both databases ---'
-- Filter to only the test roles so the row count is deterministic
-- (other roles owning catalog tables in each DB also show up, with
-- non-deterministic ordering across PG versions).
SELECT database, used_bytes > 0 AS used_nonzero, hard_exceeded
  FROM rolequota.status() s
  JOIN pg_roles r ON r.oid = s.roleid
 WHERE database IN ('rq_a', 'rq_b')
   AND r.rolname IN ('rq_multi_role_a', 'rq_multi_role_b')
 ORDER BY database;

-- Cleanup. We must drop objects + revoke before dropping the database.
-- Terminate any pg_rolequota worker BGWs connected to rq_a / rq_b first;
-- DROP DATABASE WITH (FORCE) waits for backends to accept a
-- ProcSignalBarrier, but a BGW stuck in InitPostgres doesn't process
-- barriers — so we kill them explicitly via pg_terminate_backend.
\c rq_a
DROP TABLE public.rq_a_data;
REVOKE CREATE ON SCHEMA public FROM rq_multi_role_a;
\c rq_b
DROP TABLE public.rq_b_data;
REVOKE CREATE ON SCHEMA public FROM rq_multi_role_b;
\c contrib_regression
SELECT pg_terminate_backend(pid)
  FROM pg_stat_activity
 WHERE application_name = 'pg_rolequota worker'
    OR backend_type     = 'pg_rolequota worker';
-- Give the terminated workers a moment to release their connections.
SELECT pg_sleep(0.5);
DROP DATABASE rq_a WITH (FORCE);
DROP DATABASE rq_b WITH (FORCE);
DROP ROLE rq_multi_role_a;
DROP ROLE rq_multi_role_b;

\echo '--- test_multi_db finished (composite-key launcher + workers active) ---'
