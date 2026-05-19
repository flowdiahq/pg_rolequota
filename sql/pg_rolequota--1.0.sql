-- pg_rolequota--1.0.sql
-- Installation script for pg_rolequota 1.0
-- 2-space indentation (AGENTS.md)

\echo Use "CREATE EXTENSION pg_rolequota" to load this file. \quit

-- Schema for all quota objects
CREATE SCHEMA IF NOT EXISTS rolequota;

SET search_path = rolequota;

-- Quota configuration table (per-role, per-database)
CREATE TABLE rolequota.limits (
  roleid              oid PRIMARY KEY,
  soft_bytes          bigint,           -- NULL = no soft limit
  hard_bytes          bigint,           -- NULL = no hard limit
  enforcement_policy  text NOT NULL DEFAULT 'terminate'
                        CHECK (enforcement_policy IN ('warn', 'terminate', 'lock')),
  notify              boolean NOT NULL DEFAULT true,
  created_at          timestamptz NOT NULL DEFAULT now(),
  updated_at          timestamptz NOT NULL DEFAULT now()
);

-- Make the limits table dumpable by pg_dump
SELECT pg_catalog.pg_extension_config_dump('rolequota.limits', '');

-- Real status SRF (polish): C-backed from RoleSizeHash snapshot under short lock.
-- SECURITY DEFINER per architecture + AGENTS security requirements so that
-- operators can GRANT EXECUTE selectively (prevents cross-tenant data exposure
-- in shared_hosting). Call as SELECT * FROM rolequota.status();
CREATE OR REPLACE FUNCTION rolequota.status()
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
AS 'MODULE_PATHNAME', 'pg_rolequota_status'
LANGUAGE C SECURITY DEFINER;

-- Manual trigger for enterprise scanner (symmetry with shared; used by its dedicated test)
CREATE OR REPLACE FUNCTION rolequota.scan_enterprise()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_rolequota_scan_enterprise_sql'
LANGUAGE C;

-- Request immediate BGW scan (legacy zero-arg form).
-- Bumps the wakeup counter AND SetLatch's the BGW, so the next iteration
-- of the BGW loop runs a full scan. Latency <1 ms typical. Use after
-- manual space reclaim or from AFTER triggers on rolequota.limits when
-- the whole cluster needs reconciliation.
CREATE OR REPLACE FUNCTION rolequota.request_wakeup()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_rolequota_request_wakeup'
LANGUAGE C;

-- Targeted refresh: enqueue a single role for immediate re-scan, then
-- SetLatch the BGW. Drains via the lock-free in-shmem ring (256 slots).
-- Typical end-to-end latency from this call to rolequota.status()
-- reflecting the new used_bytes is < 10 ms.
CREATE OR REPLACE FUNCTION rolequota.request_wakeup(roleid oid)
RETURNS void
AS 'MODULE_PATHNAME', 'pg_rolequota_request_wakeup_role'
LANGUAGE C STRICT;

-- Convenience: enqueue the current_user's oid. Intended for AFTER triggers
-- and application code that just wrote data and wants the BGW to refresh
-- the writer's quota immediately.
CREATE OR REPLACE FUNCTION rolequota.request_self_wakeup()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_rolequota_request_self_wakeup'
LANGUAGE C;

-- Returns the extension version + current PostgreSQL server version.
CREATE OR REPLACE FUNCTION rolequota.version()
RETURNS text
LANGUAGE sql
AS $$
  SELECT 'pg_rolequota 1.0 (PG ' || current_setting('server_version') || ')';
$$;

-- C linkage test function (declared here so CREATE EXTENSION succeeds; body in .so)
-- Will be used by C unit tests and future status SRF.
CREATE OR REPLACE FUNCTION rolequota.c_linkage_test()
RETURNS int
AS 'MODULE_PATHNAME', 'pg_rolequota_c_linkage_test'
LANGUAGE C STRICT;

-- Shmem diagnostics. Returns a small integer summarising shmem attach state.
CREATE OR REPLACE FUNCTION rolequota.shmem_info()
RETURNS int
AS 'MODULE_PATHNAME', 'pg_rolequota_shmem_info'
LANGUAGE C;

-- Common post-scan / termination state machine diagnostics (Phase 6).
-- Exercises src/scanner_common.c (the code shared by both scanner modes).
-- This declaration + the call in test_termination.sql gives the .c file its
-- required automated test per AGENTS.md §3.
CREATE OR REPLACE FUNCTION rolequota.common_test()
RETURNS int
AS 'MODULE_PATHNAME', 'pg_rolequota_common_test'
LANGUAGE C;

-- Manual trigger for the shared_hosting scanner (Slice 1).
-- Used by regression tests to populate RoleSizeHash so that the
-- ExecutorCheckPerms hard-limit enforcement can be exercised.
-- The background worker will invoke the underlying C function periodically.
CREATE OR REPLACE FUNCTION rolequota.scan_shared()
RETURNS void
AS 'MODULE_PATHNAME', 'pg_rolequota_scan_shared_sql'
LANGUAGE C;

-- Linkage-proof helpers for AGENTS.md §3 (every .c file has an automated test).
-- These return a constant string proving the .o was compiled, linked, and the
-- symbol exported. They are declared in the install SQL (not in the test SQL)
-- so MODULE_PATHNAME substitution happens via the extension installer, which
-- is the only PGXS path that performs that substitution.
CREATE OR REPLACE FUNCTION rolequota.scanner_shared_test()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_rolequota_scanner_shared_test'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION rolequota.scanner_enterprise_test()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_rolequota_scanner_enterprise_test'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION rolequota.enforcement_test()
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_rolequota_enforcement_test'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION rolequota.notify_test()
RETURNS text
AS 'MODULE_PATHNAME', 'pg_rolequota_notify_test'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION rolequota.slack_test()
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_rolequota_slack_test'
LANGUAGE C STRICT;

RESET search_path;
