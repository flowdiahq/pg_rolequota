# Makefile for pg_rolequota
# 2-space indentation everywhere except literal tabs for Make recipes (GNU Make requirement).
#
# Follows AGENTS.md strictly.

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)

# Optional libcurl for Slack (AGENTS.md §9). Auto-detect common locations so
# CI (which apt-gets libcurl4-openssl-dev) and macOS Homebrew builds both get
# HAVE_CURL without manual -D. When absent the graceful no-op path in slack.c
# is still compiled (both paths covered at build time).
ifneq ($(wildcard /usr/include/curl/curl.h /usr/local/include/curl/curl.h /opt/homebrew/include/curl/curl.h),)
  PG_CPPFLAGS += -DHAVE_CURL
endif

# Extra files to remove on make clean (standard PGXS hook, see pgxs.mk).
EXTRA_CLEAN = results/ tmp/ src/*.o

# Helpful error for common macOS + Homebrew situation
ifeq ($(wildcard $(PGXS)),)
  $(error pgxs.mk not found at $(PGXS). \
This usually happens on macOS when pg_config comes from Homebrew's "libpq" (client-only) instead of a full PostgreSQL server. \
\
Try one of these: \
  make PG_CONFIG=/opt/homebrew/opt/postgresql@18/bin/pg_config \
  make PG_CONFIG=/opt/homebrew/opt/postgresql@17/bin/pg_config \
\
Or run: \
  export PATH="/opt/homebrew/opt/postgresql@18/bin:$$PATH" && make)
endif

# --------------------------------------------------------------------
# Extension metadata (PGXS) -- MUST be before the include so pgxs.mk
# generates correct rules for MODULE_big, install, installcheck, clean etc.
# --------------------------------------------------------------------
MODULE_big = pg_rolequota
OBJS = \
  src/pg_rolequota.o \
  src/shmem.o \
  src/scanner_enterprise.o \
  src/scanner_shared.o \
  src/scanner_common.o \
  src/enforcement.o \
  src/notify.o \
  src/slack.o

EXTENSION = pg_rolequota
DATA = sql/pg_rolequota--1.0.sql
PGFILEDESC = "pg_rolequota - per-role storage quotas with soft/hard limits, termination, and dual-mode scanning"

# Standard regression test matrix (sacred gate). Set before include so
# the installcheck rule in pgxs.mk sees REGRESS and REGRESS_OPTS.
REGRESS = test_skeleton test_sql_objects test_shmem test_enforcement test_scanner_enterprise test_scanner_shared test_notify_wakeup test_wakeup_queue test_multi_db test_slack test_termination test_mid_query_cancel
# --inputdir=test makes pg_regress look in test/sql and test/expected
# instead of the default ./sql and ./expected, matching this project's layout.
REGRESS_OPTS = --inputdir=test --temp-config=test/skeleton.conf --load-extension=pg_rolequota

include $(PGXS)

# --------------------------------------------------------------------
# Build
# --------------------------------------------------------------------
# (PGXS supplies the real "all" that builds the .so via all-lib for MODULE_big.
#  We no longer override with a bare "all: $(OBJS)".)

# After include we can safely augment targets that pgxs.mk already defined.
installcheck: install

# --------------------------------------------------------------------
# Testing - the sacred gate (AGENTS.md)
# --------------------------------------------------------------------

test: verify-whitespace installcheck
	@echo "✅ All tests passed."

# Whitespace police - no trailing spaces or tabs allowed anywhere
verify-whitespace:
	@echo "Checking for trailing whitespace (2-space rule enforced)..."
	@dirty=$$( \
		git ls-files --cached --others --exclude-standard \
		| grep -v '^test/expected/' | grep -v '^results/' | grep -v '^tmp/' \
		| xargs -I {} sh -c 'test -f "{}" && grep -l "[ 	]$$" "{}"' 2>/dev/null \
	); \
	if [ -n "$$dirty" ]; then \
		echo "❌ Trailing whitespace found in:"; \
		echo "$$dirty"; \
		exit 1; \
	fi
	@echo "✅ No trailing whitespace."

# --------------------------------------------------------------------
# Code formatting and static analysis (modern C tooling)
# --------------------------------------------------------------------
CLANG_FORMAT ?= clang-format
CLANG_TIDY   ?= clang-tidy

# Reformat all C sources in place according to .clang-format
format:
	@echo "Running clang-format..."
	@$(CLANG_FORMAT) -i -style=file src/*.c src/*.h 2>/dev/null || \
		echo "clang-format not found or no changes needed."

# Run static analysis + formatting check + existing whitespace gate
lint:
	@echo ""
	@echo "=== clang-format check (would fail CI if dirty) ==="
	@$(CLANG_FORMAT) --dry-run --Werror -style=file src/*.c src/*.h 2>&1 || \
		(echo "⚠️  Formatting issues detected — run 'make format' to fix." && exit 1)

	@echo ""
	@echo "=== Existing project lint (whitespace) ==="
	@$(MAKE) --no-print-directory verify-whitespace

	@echo ""
	@echo "✅ Lint passed."

# (REGRESS already defined before the include; the block below is intentionally
#  left as a visual marker only.  The old "installcheck: all / $(PGXS) ..." recipe
#  that was here has been deleted — it is now correctly augmented via
#  "installcheck: install" right after the include.)

# --------------------------------------------------------------------
# Per-mode and feature test targets
# --------------------------------------------------------------------

test-unit:
	@$(MAKE) installcheck REGRESS=test_shmem
	@echo "✅ test-unit (C shmem / compat)"

test-enterprise:
	@$(MAKE) installcheck REGRESS=test_scanner_enterprise
	@echo "✅ test-enterprise"

test-shared:
	@$(MAKE) installcheck REGRESS=test_scanner_shared
	@echo "✅ test-shared"

test-enforcement:
	@$(MAKE) installcheck REGRESS=test_enforcement
	@echo "✅ test-enforcement"

test-notify:
	@$(MAKE) installcheck REGRESS=test_notify_wakeup
	@echo "✅ test-notify"

test-wakeup:
	@$(MAKE) installcheck REGRESS="test_notify_wakeup test_wakeup_queue"
	@echo "✅ test-wakeup (low-latency wake-up + ring queue overflow)"

test-multi-db:
	@$(MAKE) installcheck REGRESS=test_multi_db
	@echo "✅ test-multi-db (launcher + per-DB worker + composite key)"

test-prom:
	@echo "✅ test-prom - docs + exporter YAML (future)"

test-readme:
	@$(MAKE) installcheck REGRESS=test_sql_objects
	@echo "✅ test-readme - README + docs exercised via test_sql_objects (covers architecture.md SQL examples)"

test-all-modes: test-enterprise test-shared
	@echo "✅ test-all-modes"

test-slack:
	@$(MAKE) installcheck REGRESS=test_slack
	@echo "✅ test-slack"

test-architecture:
	@$(MAKE) installcheck REGRESS=test_sql_objects
	@echo "✅ test-architecture (docs/architecture.md examples covered by test_sql_objects)"

# --------------------------------------------------------------------
# Future-proofing targets (placeholders)
# --------------------------------------------------------------------

test-stress:
	@echo "test-stress: placeholder"

test-asan test-sanitizers:
	@echo "test-asan / test-sanitizers: placeholder"

test-concurrent:
	@echo "test-concurrent: placeholder"

# --------------------------------------------------------------------
# Docker / multi-version
# --------------------------------------------------------------------

docker-test:
	@$(MAKE) test

docker-test-pg-%:
	@echo "Would run full suite inside official postgres:$* image"

# --------------------------------------------------------------------
# Convenience
# --------------------------------------------------------------------

# We do not define our own "clean:" target.
# EXTRA_CLEAN (set earlier) + pgxs.mk's clean target already remove results/, tmp/, src/*.o etc.
# This avoids the "overriding recipe for target 'clean'" warning.
distclean: clean
	rm -f *.o src/*.o 2>/dev/null || true

.PHONY: \
  test verify-whitespace lint format \
  test-unit test-enterprise test-shared test-enforcement test-notify \
  test-wakeup test-multi-db test-prom test-readme test-all-modes test-slack \
  test-architecture test-stress test-asan test-sanitizers test-concurrent \
  docker-test docker-test-pg-% \
  clean distclean
