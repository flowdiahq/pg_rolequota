-- test_scanner_shared.sql (Slice 1)
-- Exercises the real SPI aggregate implementation in scanner_shared.c plus the
-- SQL wrapper. Also calls scan_shared() so that scanner_common.c decision
-- logic and shmem update are covered by the automated regression (AGENTS.md §3).
\echo '--- test_scanner_shared starting ---'
CREATE EXTENSION IF NOT EXISTS pg_rolequota;

\echo '--- creating non-storage relations to exercise relkind filter (SIGSEGV regression) ---'
-- A view, a composite type, and a partitioned-table parent. Before the
-- relkind filter was added these caused pg_total_relation_size() to NULL-deref
-- in the BGW on first scan (signal 11). Including all three here proves the
-- filter holds.
CREATE TABLE rolequota_storage_table (id int);
INSERT INTO rolequota_storage_table SELECT generate_series(1, 100);
CREATE VIEW rolequota_test_view AS SELECT 1 AS a;
CREATE TYPE rolequota_test_ct AS (a int, b text);
CREATE TABLE rolequota_test_parent (id int) PARTITION BY RANGE (id);
CREATE TABLE rolequota_test_child PARTITION OF rolequota_test_parent
  FOR VALUES FROM (1) TO (100);

\echo '--- exercising real shared_hosting scanner (SPI + common_post_scan) ---'
SELECT rolequota.scan_shared();
\echo '--- scan_shared completed without error (shmem may now contain role sizes) ---'

\echo '--- verifying no garbage byte counts (numeric/int64 conversion regression) ---'
-- SUM(bigint) returns numeric in PG; the old DatumGetInt64 misread that as a
-- pointer-as-int64, producing 14-digit garbage values. After the fix every
-- used_bytes must fit a reasonable storage size (well under 2^50 ≈ 1 PB).
SELECT bool_and(used_bytes >= 0 AND used_bytes < (1::bigint << 50)) AS reasonable
  FROM rolequota.status();

\echo '--- second scan_shared call (transaction-state regression test) ---'
-- Proves the PG_CATCH cleanup paths call AbortCurrentTransaction before the
-- next iteration's StartTransactionCommand. Without that fix the second call
-- would crash with "another command is already in progress".
SELECT rolequota.scan_shared();

\echo '--- cleanup ---'
DROP TABLE rolequota_test_parent CASCADE;
DROP TYPE rolequota_test_ct;
DROP VIEW rolequota_test_view;
DROP TABLE rolequota_storage_table;

\echo '--- test_scanner_shared finished (real aggregate path active) ---'
