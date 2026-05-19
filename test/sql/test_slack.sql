-- test_slack.sql (Phase 7)
\echo '--- test_slack starting ---'
CREATE EXTENSION IF NOT EXISTS pg_rolequota;

-- Linkage proof for slack.c (function declared by the extension).
SELECT rolequota.slack_test() AS slack_ok;

\echo '--- test_slack finished (C symbol always linked; HAVE_CURL affects only slack_send() compile-time path) ---'
