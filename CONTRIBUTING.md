# Contributing to HLK-LD2412 Security Firmware

Thank you for your interest in contributing. This document covers setup, build, testing, and code style guidelines.

## Table of Contents

- [Getting Started](#getting-started)
- [Building the Firmware](#building-the-firmware)
- [Running Tests](#running-tests)
- [Branch Naming](#branch-naming)
- [Code Style](#code-style)
- [What We Look For](#what-we-look-for)
- [What We Do Not Accept](#what-we-do-not-accept)

---

## Getting Started

### Prerequisites

- [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html) (CLI or IDE plugin)
- Python 3.8+
- Git

### Setup

```bash
git clone https://github.com/PeterkoCZ91/HLK-LD2412-security.git
cd HLK-LD2412-security

# Create required configuration files from examples
cp include/secrets.h.example include/secrets.h
cp include/known_devices.h.example include/known_devices.h
```

Edit `include/secrets.h` and `include/known_devices.h` with your local values.
**Never commit these files** — they are listed in `.gitignore`.

---

## Building the Firmware

| Environment | GPIOs | Command |
|---|---|---|
| `esp32_type_A` | RX=16, TX=17 | `pio run -e esp32_type_A` |
| `esp32_type_B` | RX=18, TX=19, OUT=21 | `pio run -e esp32_type_B` |
| `esp32_custom` | configurable | `pio run -e esp32_custom` |
| `production` | RX=18, TX=19 | `pio run -e production` |

Build all environments at once:

```bash
pio run -e esp32_type_A -e esp32_type_B -e esp32_custom -e production
```

Upload to a connected device:

```bash
pio run -e esp32_type_A --target upload
```

---

## Running Tests

Tests run on the host (no hardware required) using the Unity framework:

```bash
pio test -e test_native
```

Test sources live in:
- `test/test_native_parser/` — radar frame parser unit tests
- `test/test_native_security/` — security logic unit tests

All tests must pass before a pull request can be merged.

---

## Branch Naming

| Type | Prefix | Example |
|---|---|---|
| New feature | `feature/` | `feature/mqtt-tls-verify` |
| Bug fix | `fix/` | `fix/ota-reboot-loop` |
| Documentation | `docs/` | `docs/wiring-diagrams` |
| Refactoring | `refactor/` | `refactor/parser-cleanup` |

Keep branch names lowercase, hyphen-separated, and descriptive.

---

## Code Style

- **Indentation:** 4 spaces — no tabs
- **Line endings:** LF
- **Naming:** `camelCase` for variables and functions, `PascalCase` for classes and types, `UPPER_SNAKE_CASE` for macros and constants
- **No dynamic allocation in hot paths** — avoid `new`/`delete`, `malloc`/`free`, and `String` concatenation inside loop functions or ISR handlers; prefer fixed-size buffers and stack allocation
- **No `Serial.print` in production builds** — use the `debug.h` macros which compile away when `SERIAL_DEBUG` is not defined
- **Header guards** — use `#pragma once` in all headers
- **File encoding:** UTF-8, no BOM
- **Max line length:** 120 characters (soft limit)

---

## What We Look For

- Bug fixes with a minimal, targeted change and a clear explanation of the root cause
- New features that align with the project scope (LD2412 radar integration, security automation, MQTT/web UI)
- Unit tests for new parser logic or security rules
- Improvements to CI reliability or build reproducibility
- Documentation corrections

---

## What We Do Not Accept

- Commits that include `include/secrets.h` or `include/known_devices.h`
- `Serial.print` / `Serial.println` calls outside of `#ifdef SERIAL_DEBUG` guards
- Dynamic memory allocation (`String`, `new`, `malloc`) in `loop()` or radar parsing hot paths
- Changes to `.github/ISSUE_TEMPLATE/` or `.github/pull_request_template.md`
- Code that breaks any existing `pio test -e test_native` test without a corresponding fix
- Unrelated reformatting bundled into a functional change
