<!--
Thanks for contributing! A few ground rules:
- Keep the PR focused on one change. Separate refactors from behavior changes.
- Verify on real hardware when the change touches radar, alarm logic, OTA, or MQTT.
- All code, comments, and UI strings must be in English.
- Do NOT commit secrets.h, known_devices.h, or any file with real credentials / IPs / MACs.
-->

## Summary

<!-- 1–3 sentences describing what this PR changes and why. -->

## Type of change

- [ ] Bug fix (non-breaking)
- [ ] New feature (non-breaking)
- [ ] Breaking change (config key, MQTT topic, API shape, or behavior)
- [ ] Documentation / tooling only
- [ ] Build / CI

## Affected areas

- [ ] Radar (LD2412) / zones / sensitivity
- [ ] Alarm state machine (arm, disarm, entry/exit delay, supervised heartbeat)
- [ ] Scheduled arm / disarm / timezone
- [ ] Web dashboard / API
- [ ] Home Assistant / MQTT
- [ ] Telegram bot
- [ ] Event log / history
- [ ] OTA / update flow
- [ ] Build system / CI

## Hardware tested on

<!-- Board, build env, firmware version, sensor FW version. -->

- Board:
- Build env (e.g. `esp32_type_B`):
- Firmware version:
- Sensor FW version:

## Test plan

<!-- How did you verify this works? Manual steps, API snapshots, HA entity checks, etc. -->

- [ ]
- [ ]

## Checklist

- [ ] I ran `pio run -e esp32_type_B` (or the relevant environment) and the build succeeds.
- [ ] I verified behavior on real hardware (or marked the PR as doc-only / CI-only).
- [ ] I updated `CHANGELOG.md` if this is user-visible.
- [ ] I updated `README.md` if I introduced new API endpoints, MQTT topics, config keys, or build flags.
- [ ] I did **not** commit secrets (`include/secrets.h`, `include/known_devices.h`, credentials, tokens, private IPs).
- [ ] All new strings (web UI, Telegram messages, comments) are in English.
