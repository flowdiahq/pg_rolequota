-- test_skeleton.sql
-- Trivial test that must pass even in the earliest skeleton phase.
-- This proves the extension can be loaded (via shared_preload_libraries),
-- CREATE EXTENSION succeeds, and basic SQL objects from pg_rolequota--1.0.sql exist.
-- Follows AGENTS.md: every file has a test. Updated in Phase 1.

\echo '--- pg_rolequota skeleton test starting ---'

-- Real CREATE EXTENSION now works (Phase 1 completion)
CREATE EXTENSION IF NOT EXISTS pg_rolequota;

-- Verify core objects from the SQL script
\echo '--- checking schema and table ---'
SELECT nspname FROM pg_namespace WHERE nspname = 'rolequota';

\echo '--- checking limits table ---'
SELECT tablename FROM pg_tables WHERE schemaname = 'rolequota' AND tablename = 'limits';

\echo '--- checking status SRF (real shmem-backed) ---'
SELECT proname FROM pg_proc WHERE pronamespace = (SELECT oid FROM pg_namespace WHERE nspname='rolequota') AND proname = 'status';

\echo '--- checking version() helper ---'
SELECT rolequota.version() LIKE '%pg_rolequota%' AS version_ok;

\echo '--- querying status SRF (real implementation) ---'
SELECT count(*) >= 0 AS status_srf_ok FROM rolequota.status();

\echo '--- skeleton test finished successfully ---'
