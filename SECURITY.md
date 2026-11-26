# Security Policy

KeronisEngine can sign and submit live exchange orders when configured with venue credentials. We take security issues seriously.

## Reporting a Vulnerability

Please **do not** open a public GitHub issue for security vulnerabilities (especially anything involving key handling, order signing, risk-limit bypasses, or control-server access).

Instead, report vulnerabilities privately via GitHub Security Advisories on this repository (`Security` → `Report a vulnerability`).

## Operational Safety Recommendations

- Keep `ENGINE_CONTROL_SERVER_BIND=127.0.0.1` and `ENABLE_ENGINE_CONTROL_MUTATIONS=0` unless you explicitly need remote control behind a trusted network boundary.
- Start new deployments with `IS_ORDER_CREATION_DISABLED=1` and paper/testnet credentials before enabling live execution.
- Restrict permissions on your `ACCOUNTS_FILE` (`chmod 600`) and never commit it to version control.
