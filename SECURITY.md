# Security Policy

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| 0.1.x   | :white_check_mark: |

## Reporting a Vulnerability

Please follow responsible disclosure:

1. **Do NOT** open a public GitHub issue.
2. Email the maintainer at: `YOUR_EMAIL@example.com`
3. Include:
   - Description of the vulnerability
   - Steps to reproduce
   - Potential impact
   - Suggested fixes (optional)

We will acknowledge receipt within 48 hours and aim to provide a fix or mitigation within 14 days for critical issues.

## Scope

AsyncSD is an embedded SD card library. Security considerations include:
- No network code
- No NVS or hidden persistent storage side effects
- Deterministic, bounded execution to reduce lockups

## Best Practices for Users

- Validate paths and request parameters before enqueuing
- Use a shared SPI guard if multiple devices use the bus
- Keep dependencies (SdFat, Arduino core) up to date
