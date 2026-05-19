# AGENTS.md — pg_rolequota Project Constitution

This file is the single source of truth for all contributors (human or AI) working on the `pg_rolequota` PostgreSQL extension.

**You MUST follow every rule below. No exceptions.**

---

## 1. Indentation Style (Strict)

- **Use exactly 2 spaces for indentation in every file.**
- Never use 4 spaces.
- Never use tabs for indentation (except in Makefiles — see below).
- This rule applies to:
  - All C source (`.c`, `.h`)
  - All SQL (`.sql`)
  - YAML, JSON, Markdown, shell scripts, Python helper scripts
  - Documentation and examples
- In **Makefiles only**: recipe command lines (the lines under a target that actually execute) **must** begin with a single literal tab character, as required by GNU Make. All other indentation in Makefiles (variable assignments, comments, target names, etc.) must use 2 spaces.

Example of correct C style:
```c
int
my_function(int x)
{
  if (x > 0)
  {
    do_something();
    return 42;
  }
  return 0;
}
```

Bad (4 spaces or tabs) will be rejected by `make verify-whitespace` and `make lint`.

---

## 2. No Trailing Whitespace — Ever

- Every line in every committed file must end with a newline and must **not** contain any trailing spaces or tabs.
- Run `make verify-whitespace` before considering any change complete.
- The CI pipeline will fail on any trailing whitespace.

---

## 3. Every Single File Must Have Comprehensive Automated Tests

**Core unbreakable rule:**

> **If you create or significantly modify a file, you are not done until there is an automated, passing test that exercises the new/changed behavior.**

This applies to:
- Every `.c` file (including `compat.h` changes)
- Every `.sql` file (schema objects, functions, views)
- Every documentation example in `README.md` or `docs/`
- Every exporter YAML snippet
- New GUCs, new hooks, new background worker logic
- Makefile targets themselves (they must be exercised by `make test-*`)

See the "Per-File Test Ownership" table in the implementation plan (or the latest version in the repo) for the current mapping.

---

## 4. The `make test` Gate

Before any change is considered complete:

```bash
make verify-whitespace
make lint
make test
```

All three must be green.

The root `Makefile` provides many convenience targets:
- `make test-enterprise`
- `make test-shared`
- `make test-enforcement`
- `make test-notify`
- `make test-prom`
- `make test-readme`
- `make test-all-modes`
- `make docker-test`
- etc.

Never bypass the test gate. If a test is red, the change is not finished.

---

## 5. Two Scanner Modes — Equal Treatment

We maintain two distinct scanner implementations (`enterprise_db` and `shared_hosting`).

- Both must have dedicated, non-shared test suites.
- Code that is common must still be covered by tests that run in both modes.
- Changing behavior in one mode without updating the corresponding test target is forbidden.

---

## 6. Soft / Hard Limits + Termination + Locking Behavior

Any change that affects quota enforcement, session termination, account locking via `ClientAuthentication_hook`, or the state machine in `scanner_common.c` requires explicit test coverage for **all** combinations of:
- soft only / hard only / both
- policy = 'warn' / 'terminate' / 'lock'
- recovery path (space reclaimed → automatic unlock)

---

## 7. PostgreSQL Version Compatibility

- Code must build and pass tests on **unmodified** PostgreSQL 14, 15, 16, 17, and 18.
- Use `compat.h` for any version differences.
- The CI matrix is the final arbiter — if it is red on any supported version, the change is not done.

---

## 8. LISTEN / NOTIFY Wake-up Path

The `notify.c` mechanism is a first-class feature for reducing detection latency.

Any change here must include a test (`test_notify_wakeup.sql` or equivalent) that proves a `NOTIFY` causes a **targeted** refresh and that the periodic poll fallback still works.

---

## 9. Optional Features (Slack, Future Embedded Metrics)

- Features guarded by `HAVE_CURL` or compile-time conditionals must have a test that exercises both the "feature present" and "feature absent / gracefully disabled" paths.
- Documentation for optional features must still be tested via `make test-readme` or `make test-prom`.

---

## 10. Documentation Is Code

- Every SQL snippet, shell command, or configuration example that appears in `README.md` or `docs/*.md` must be runnable and is automatically executed by `make test-readme`.
- If you update docs, you must update (or add) the corresponding test.

---

## 11. Git Hygiene

- Never commit files with trailing whitespace.
- Run the full test suite locally before pushing.
- PR title / description must reference which test targets were added or updated for the changed files.

---

## 12. LLM / Agent Usage Notes

When an LLM or coding agent is used:
- Set token limits high enough (the original Claude.md mentioned 200,000).
- Always run `make test` after the agent finishes and before the human approves.
- The agent must be told to follow this `AGENTS.md` file.

---

## 13. Evolving This File

If you need to change a rule in `AGENTS.md`:
1. Propose the change with a clear rationale.
2. Update the corresponding tests and Makefile targets first.
3. Only after the new rule is itself tested and enforced, merge the `AGENTS.md` update.

---

**Summary for the impatient (still must obey the full text above):**

- 2 spaces, no trailing whitespace, ever.
- Every file you touch needs a test.
- `make verify-whitespace && make lint && make test` must be green.
- Both scanner modes are first-class citizens.
- Documentation examples are executable tests.
- The test gate is non-negotiable.

Violations of this file are treated as bugs.

Last updated: 2026-05 — 2-space indentation rule explicitly added per maintainer request.
