# Contributing to pg_rolequota

Thank you for your interest in contributing!

This project follows a **strict engineering discipline** defined in [AGENTS.md](AGENTS.md). Please read it before making changes — it is the single source of truth for this repository.

## The Sacred Gate (Non-Negotiable)

Before any change is considered complete, the following **must** be green:

```bash
make verify-whitespace
make lint
make test
```

If any of these are red, the change is not finished.

## Before You Start

1. Make sure you can build and run the tests locally:
   ```bash
   make
   make test
   ```
2. Familiarize yourself with the two scanner modes (`enterprise_db` vs `shared_hosting`) — they receive equal treatment (see AGENTS.md §5).
3. Every file you significantly modify or create **must** have automated test coverage (AGENTS.md §3).

## Adding or Changing Code

- **Implementation files** (`.c`, `.h`, `.sql` that define schema objects or behavior) require new or updated tests.
- Pure documentation and configuration files created for open-source hygiene (this file, `.gitignore`, issue templates, etc.) do **not** require new SQL regression tests.
- When touching scanner or enforcement logic, be extremely careful with the hazard contracts documented in the source (lock discipline, `ensure_shmem`, generation stamps, MemoryContext hygiene, etc.).
- Run the full gate (`make verify-whitespace && make lint && make test`) before opening a PR.

## Pull Request Checklist

- [ ] `make verify-whitespace && make lint && make test` is green
- [ ] Every modified `.c` / `.sql` implementation file has test coverage
- [ ] Both scanner modes are exercised if the change affects common behavior (see `test-all-modes`)
- [ ] 2-space indentation and no trailing whitespace (enforced by the gate)
- [ ] `docs/architecture.md` is updated if you changed design or invariants
- [ ] The PR description references which test targets were added or updated

## Reporting Bugs

Please use the GitHub issue templates. Include:

- PostgreSQL version
- `pg_rolequota` version / commit
- Exact reproduction steps (ideally a minimal SQL script)
- Relevant logs from the background worker or hooks

## Code of Conduct

This project follows the [Contributor Covenant Code of Conduct](CODE_OF_CONDUCT.md). By participating, you are expected to uphold this code. Please report unacceptable behavior to the maintainers as described in [SECURITY.md](SECURITY.md).

## Questions?

Open a GitHub Discussion or an issue with the `question` label.

We value rigorous, well-tested contributions that respect the project's constitution. Welcome aboard!
