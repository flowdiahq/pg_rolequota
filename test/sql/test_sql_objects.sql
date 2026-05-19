-- test_sql_objects.sql
-- Comprehensive test for all SQL objects created by pg_rolequota--1.0.sql
-- Phase 1: exercises schema, limits table, check constraints, status view, version().
-- Follows AGENTS.md strictly: this file tests the sql/ module.
-- 2-space indentation everywhere.

\echo '--- test_sql_objects starting ---'

CREATE EXTENSION IF NOT EXISTS pg_rolequota;

SET search_path = rolequota, public;

-- Test 1: limits table structure and constraints
\echo '--- limits table columns ---'
SELECT column_name, data_type, is_nullable
FROM information_schema.columns
WHERE table_schema = 'rolequota' AND table_name = 'limits'
ORDER BY ordinal_position;

-- Test 2: enforcement_policy CHECK constraint
\echo '--- valid policy insert ---'
INSERT INTO rolequota.limits (roleid, soft_bytes, hard_bytes, enforcement_policy)
VALUES (10, 1000000, 2000000, 'terminate');

-- invalid policy must fail (terse verbosity hides the timestamped DETAIL line
-- which would otherwise drift between runs).
\echo '--- invalid policy rejected by CHECK ---'
\set VERBOSITY terse
\set ON_ERROR_STOP off
INSERT INTO rolequota.limits (roleid, soft_bytes, hard_bytes, enforcement_policy)
VALUES (11, 1000, 2000, 'invalid');
\set ON_ERROR_STOP on
\set VERBOSITY default

-- Verify only the good row exists
SELECT count(*) AS good_rows FROM rolequota.limits WHERE roleid = 10;

-- Test 3: status SRF (real)
\echo '--- status SRF (shmem-backed) ---'
SELECT count(*) >= 0 AS status_srf_ok FROM rolequota.status();

-- Test 4: version() function. We only check the leading prefix so the test
-- is stable across PG releases (the trailing portion includes
-- current_setting('server_version') which differs per cluster).
\echo '--- version function ---'
SELECT rolequota.version() LIKE 'pg_rolequota 1.0 (PG %' AS v_ok;

-- Test 4b: C linkage function (proves .so was loaded and symbol resolved)
\echo '--- C linkage test ---'
SELECT rolequota.c_linkage_test() AS c_result;

-- Test 5: pg_extension_config_dump registered (smoke).
-- pg_extension_config_dump is a function, not a view — the registration is
-- visible via pg_extension.extconfig (a regclass[] of dump-marked relations).
SELECT 'rolequota.limits'::regclass = ANY(extconfig) AS dump_entry_exists
FROM pg_extension WHERE extname = 'pg_rolequota';

-- Cleanup: drop the limits row this test inserted so it does not leak into
-- the subsequent tests (which share contrib_regression). Leaving a hard limit
-- on role 10 would cause later tests' scan_shared() to mark that role as
-- hard_exceeded and the enforcement hook would then block all writes by it.
DELETE FROM rolequota.limits WHERE roleid = 10;

RESET search_path;

\echo '--- test_sql_objects finished successfully ---'
