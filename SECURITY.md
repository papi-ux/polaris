# Security Policy

## Supported Versions

- Latest tagged release
- Current `master`

## Reporting a Vulnerability

Please do not open a public issue for authentication bypasses, pairing bypasses, certificate or TLS flaws, client-command escalation, trusted-subnet mistakes, or unintended data exposure.

Prefer GitHub Security Advisories if they are enabled for the repository. If private reporting is not available, open a minimal public issue asking for a private follow-up channel and avoid posting exploit details or secrets.

## Deployment Notes

- Keep the Polaris web UI on a trusted network. Do not publish it directly to the public internet without your own reverse-proxy authentication and TLS controls.
- Review `trusted_subnets` carefully. Devices on those subnets can pair automatically.
- Treat `client_commands` and related shell hooks as privileged configuration.
- The AI optimizer is optional and disabled by default. Review provider settings before enabling hosted backends.
