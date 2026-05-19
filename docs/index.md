# Documentation

This directory contains project documentation for `pg_rolequota`.

## Available Documents

- **[Architecture](architecture.md)** — Comprehensive design document covering:
  - Multi-database launcher + per-database worker architecture
  - Composite `(dbid, roleid)` hash key throughout shmem and the wake-up queue
  - Low-latency wake-up bus (published latch + lock-free SPSC ring queue)
  - Dual scanner modes (`enterprise_db` vs `shared_hosting`)
  - Shared memory model and `EXEC_BACKEND` safety
  - Enforcement hooks and hot-path rules
  - State machine, termination, and recovery
  - Hazard contracts and invariants
  - Scaling guidance up to 5 000 roles per cluster
  - Build, testing, and security considerations

## Top-level references

- [`../README.md`](../README.md) — quick start, SQL surface, GUCs, latency profile.
- [`../CHANGELOG.md`](../CHANGELOG.md) — release notes and migration steps.
- [`../AGENTS.md`](../AGENTS.md) — project constitution (2-space, no trailing
  whitespace, mandatory tests, sacred `make test` gate).

## Contributing

For contribution guidelines, please see the root [`CONTRIBUTING.md`](../CONTRIBUTING.md) and [`AGENTS.md`](../AGENTS.md).

All documentation changes should follow the project's 2-space indentation and no-trailing-whitespace rules.
