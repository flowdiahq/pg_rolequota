-- examples/quick-demo.sql
-- Canonical end-to-end pg_rolequota workflow.
--
-- Usage:
--   1. In postgresql.conf:
--        shared_preload_libraries = 'pg_rolequota'
--      Then restart the cluster.
--   2. From psql (any database):
--        CREATE DATABASE demo;
--        \c demo
--        \i examples/quick-demo.sql
--
-- What you should see at the end: one row with hard_exceeded = true and
-- soft_exceeded = true, used_bytes well above the 6 MB hard limit.

CREATE EXTENSION IF NOT EXISTS pg_rolequota;

-- A throwaway role that owns the table we will fill.
DROP ROLE IF EXISTS demo_user;
CREATE ROLE demo_user LOGIN;

DROP TABLE IF EXISTS demo_t;
CREATE TABLE demo_t (pad text);
ALTER TABLE demo_t OWNER TO demo_user;

-- 60 kB soft / 6 MB hard. 'terminate' policy means writes are rejected by
-- the ExecutorCheckPerms_hook once we are over hard.
INSERT INTO rolequota.limits
  (roleid, soft_bytes, hard_bytes, enforcement_policy)
VALUES
  ('demo_user'::regrole::oid, 60000, 6000000, 'terminate')
ON CONFLICT (roleid) DO UPDATE
  SET soft_bytes = EXCLUDED.soft_bytes,
      hard_bytes = EXCLUDED.hard_bytes,
      enforcement_policy = EXCLUDED.enforcement_policy;

-- Write ~20 MB to put us comfortably over the 6 MB hard limit.
INSERT INTO demo_t
SELECT repeat('x', 200)
FROM generate_series(1, 100000);

-- Wake the per-database worker up immediately instead of waiting for the
-- next launcher tick (default 60 s). p50 latency from this call to the
-- status() update is < 10 ms; we sleep 200 ms to leave generous headroom.
SELECT rolequota.request_self_wakeup();
SELECT pg_sleep(0.2);

SELECT database,
       roleid::regrole AS role,
       pg_size_pretty(used_bytes) AS used,
       pg_size_pretty(soft_bytes) AS soft,
       pg_size_pretty(hard_bytes) AS hard,
       soft_exceeded,
       hard_exceeded,
       enforcement_policy
FROM rolequota.status()
WHERE roleid = 'demo_user'::regrole::oid;

-- The next INSERT as demo_user should be rejected by the hook with:
--   ERROR: disk quota exceeded for a role under hard limit
--
-- Try it interactively:
--   SET ROLE demo_user;
--   INSERT INTO demo_t VALUES ('blocked');
