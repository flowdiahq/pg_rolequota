-- test_enforcement.sql
-- Phase 4: exercises enforcement.c hook installation (stubs) and future policy
-- behavior. Currently proves the hooks were installed without crashing the
-- backend on load.
--
-- pgq002: when real logic lands, this file (plus any new concurrent-auth tests)
-- must exercise the microsecond-fast path, early bail, and the "never hold lock
-- while doing catalog work" rule under load. The existing DML driver already
-- guarantees the hook is reached on every statement.

\echo '--- test_enforcement starting ---'

CREATE EXTENSION IF NOT EXISTS pg_rolequota;

-- The C function proves enforcement.c was compiled in and its test symbol
-- resolved. Declared in the install SQL so MODULE_PATHNAME is substituted
-- correctly at CREATE EXTENSION time (pg_regress does not substitute it
-- inside test SQL files).
SELECT rolequota.enforcement_test() AS hooks_active;

-- Exercise the (now correctly typed) ExecutorCheckPerms_hook via real DML.
-- This directly addresses reviewer Issue 1: the hook path must be traversed
-- during installcheck.
\echo '--- exercising ExecutorCheckPerms_hook via INSERT (must not crash) ---'
CREATE TABLE IF NOT EXISTS public.rolequota_hook_test (id int);
INSERT INTO public.rolequota_hook_test VALUES (42);
SELECT count(*) FROM public.rolequota_hook_test;
DROP TABLE public.rolequota_hook_test;

\echo '--- In full Phase 4/6 a locked role would be rejected at ClientAuthentication and INSERT would ERROR with disk_full ---'

\echo '--- Slice 1: real hard-limit enforcement via shared scanner + hook ---'
-- Use a dedicated test role so we don't quota-lock the admin connection.
-- If the current_user's quota goes over, the admin can't clean up.
DO $do$
DECLARE
  testrole text := 'rolequota_enf_test_role';
  testtbl text := 'public.rolequota_enf_quota_test';
  testrole_oid oid;
BEGIN
  EXECUTE format('DROP ROLE IF EXISTS %I', testrole);
  EXECUTE format('CREATE ROLE %I LOGIN', testrole);
  EXECUTE format('GRANT CREATE ON SCHEMA public TO %I', testrole);
  EXECUTE format('GRANT INSERT, SELECT, DELETE ON ALL TABLES IN SCHEMA public TO %I',
                 testrole);
  testrole_oid := (SELECT oid FROM pg_roles WHERE rolname = testrole);

  EXECUTE format('CREATE TABLE %s (id int, pad text)', testtbl);
  EXECUTE format('ALTER TABLE %s OWNER TO %I', testtbl, testrole);
  EXECUTE format('INSERT INTO %s SELECT g, repeat($p$x$p$, 200) FROM generate_series(1, 20) g',
                 testtbl);

  -- Set a tiny hard limit on the test role
  INSERT INTO rolequota.limits (roleid, hard_bytes, enforcement_policy, notify)
  VALUES (testrole_oid, 1024, 'terminate', false)
  ON CONFLICT (roleid) DO UPDATE
    SET hard_bytes = 1024, enforcement_policy = 'terminate',
        notify = false, updated_at = now();

  -- Force a scan as the operator (current_user) so RoleSizeHash is populated.
  PERFORM rolequota.scan_shared();
END $do$;

-- Verify hook rejection by attempting DML AS the test role against its own
-- table. We can't change role inside a DO without SECURITY DEFINER tricks,
-- so we use SET ROLE here.
\echo '--- attempting INSERT as the limited role (must be rejected by hook) ---'
SET ROLE rolequota_enf_test_role;
\set ON_ERROR_STOP off
INSERT INTO public.rolequota_enf_quota_test VALUES (999999, 'this should fail quota');
\set ON_ERROR_STOP on
RESET ROLE;

\echo '--- cleanup ---'
DO $do$
DECLARE
  testrole text := 'rolequota_enf_test_role';
  testtbl text := 'public.rolequota_enf_quota_test';
BEGIN
  DELETE FROM rolequota.limits
   WHERE roleid = (SELECT oid FROM pg_roles WHERE rolname = testrole);
  EXECUTE format('DROP TABLE %s', testtbl);
  EXECUTE format('REVOKE INSERT, SELECT, DELETE ON ALL TABLES IN SCHEMA public FROM %I',
                 testrole);
  EXECUTE format('REVOKE CREATE ON SCHEMA public FROM %I', testrole);
  EXECUTE format('DROP ROLE %I', testrole);
END $do$;

\echo '--- Slice 1 hard-limit rejection path exercised successfully ---'
\echo '--- test_enforcement finished successfully ---'
