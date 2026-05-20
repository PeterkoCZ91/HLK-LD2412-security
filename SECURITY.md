# Security Policy

## Supported Versions

Only the latest released version receives security fixes.

| Version | Supported |
|---|---|
| Latest release | Yes |
| Older releases | No |

## Reporting a Vulnerability

If you discover a security vulnerability in this project, please report it through **responsible disclosure**:

1. **Do not** open a public GitHub issue for security vulnerabilities.
2. Send a description of the issue to the repository maintainer via the email address listed on the GitHub profile.
3. Include as much detail as possible: affected component, reproduction steps, potential impact, and any suggested mitigations.
4. You will receive an acknowledgement within **7 days**. If you have not heard back after 7 days, follow up to confirm the report was received.
5. We will work with you to understand and resolve the issue before any public disclosure.

## Scope

The following components are in scope for vulnerability reports:

- **Web UI** — HTTP Basic Auth implementation, session handling, input validation
- **MQTT / MQTTS** — broker authentication, TLS configuration, topic access control
- **OTA updates** — password-protected OTA endpoint, firmware integrity

## Out of Scope

The following are explicitly out of scope:

- **Physical access attacks** — an attacker with physical access to the device is outside the threat model
- **HLK-LD2412 radar module firmware** — the radar firmware is proprietary and maintained by HLK; vulnerabilities in it should be reported directly to the manufacturer
- **Third-party libraries** — ArduinoJson, PubSubClient, ESPAsyncWebServer, AsyncTelegram2, etc. Vulnerabilities in these should be reported to their respective maintainers
- **Denial-of-service via hardware** (RF jamming, power cycling)
- **Issues in forks or derivative works** not maintained in this repository

## Disclosure Timeline

| Day | Action |
|---|---|
| 0 | Report received |
| ≤ 7 | Acknowledgement sent |
| ≤ 30 | Fix developed and tested |
| ≤ 45 | Fix released; CVE requested if applicable |
| ≤ 90 | Public disclosure (coordinated with reporter) |

Timelines may be extended by mutual agreement for complex issues.
