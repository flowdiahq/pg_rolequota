---
name: Bug report
about: Report a problem with pg_rolequota
title: ''
labels: bug
assignees: ''
---

**Describe the bug**
A clear and concise description of what the bug is.

**To Reproduce**
Steps to reproduce the behavior:

1. ...
2. ...

**Expected behavior**
What you expected to happen.

**Environment**
- PostgreSQL version: (e.g. 16.4)
- pg_rolequota version / commit:
- Operating system:
- `shared_preload_libraries` setting:

**Additional context**
- Relevant logs from the background worker
- `SELECT * FROM rolequota.status;`
- Any custom GUCs or configuration

**AGENTS.md compliance note**
If this is a code change you are proposing, please confirm you have run:
```bash
make verify-whitespace && make lint && make test
```
