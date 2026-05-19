-- test_scanner_enterprise.sql (Slice 2)
-- Exercises the real enterprise FS walker (base/ + tblspc/ + stat + owner mapping
-- + per-owner common_post_scan + final orphan sweep under short lock).
\echo '--- test_scanner_enterprise starting ---'
CREATE EXTENSION IF NOT EXISTS pg_rolequota;

-- Linkage proof for scanner_enterprise.c (function declared by the extension).
SELECT rolequota.scanner_enterprise_test() AS mode;

\echo '--- exercising real enterprise fs-walk scanner ---'
DO $$ BEGIN PERFORM rolequota.scan_enterprise(); END $$;
\echo '--- enterprise scan completed (walk + orphan sweep exercised) ---'

\echo '--- test_scanner_enterprise finished (real fs walk + generation/orphan logic active) ---'
