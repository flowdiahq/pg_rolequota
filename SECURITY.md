# Security Policy

## Supported Versions

We currently support the following versions with security updates:

| Version | Supported          |
| ------- | ------------------ |
| latest  | :white_check_mark: |

## Reporting a Vulnerability

**pg_rolequota** is a PostgreSQL extension that runs with elevated privileges via `shared_preload_libraries`. We take security issues seriously.

### Preferred Reporting Method

Please report security vulnerabilities **privately** using one of the following methods:

1. **GitHub Private Vulnerability Reporting** (recommended)
   - Go to the repository's **Security** tab → **Report a vulnerability**
   - This is the fastest and most secure channel.

2. **Email**
   - Send details to **support@flowdia.ai** — delivered to the project maintainers.
   - Please prefix the subject with `[pg_rolequota security]` so it routes correctly.

### What to Include

When reporting, please provide as much information as possible:

- Description of the vulnerability
- Steps to reproduce
- Potential impact (especially regarding data access or denial of service)
- Any suggested fixes or mitigations

### Our Commitment

- We will acknowledge receipt of your report within **48 hours**.
- We will provide a detailed response within **7 days** (including whether we accept the report and our planned timeline).
- We will keep you informed of our progress.
- We will credit you in the release notes (unless you prefer to remain anonymous).

### Scope

Particular areas of concern for this project include:

- Memory safety issues in the C code (especially around shared memory, hook paths, and background workers)
- Information disclosure between roles (cross-tenant data leaks in shared hosting scenarios)
- Denial of service via quota enforcement logic or resource exhaustion
- Injection or privilege escalation paths through the `limits` table or SPI usage

We appreciate responsible disclosure and will not pursue legal action against researchers who follow this policy.

Thank you for helping keep the PostgreSQL ecosystem safe.
