# LD2412 Security Node

**ESP32 + HLK-LD2412 24 GHz FMCW mmWave radar — full security alarm system**

[![Version](https://img.shields.io/badge/version-v3.12.1-blue)](CHANGELOG.md)
[![Platform](https://img.shields.io/badge/platform-ESP32-green)](platformio.ini)
[![License](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)

This is not a presence lamp switch. It is a standalone intruder-detection node with a full alarm state machine, configurable alert zones, anti-masking tamper detection, Telegram bot control, MQTT Home Assistant integration, and a multi-language web GUI — all running on a single ESP32.

---

## Table of Contents

- [Features at a Glance](#features-at-a-glance)
- [Alarm State Machine](#alarm-state-machine)
- [Alert Zones](#alert-zones)
- [Security Features](#security-features)
- [Gates (Radar Sensitivity)](#gates-radar-sensitivity)
- [Event Log](#event-log)
- [Scheduled Arm / Disarm](#scheduled-arm--disarm)
- [MQTT + Home Assistant](#mqtt--home-assistant)
- [Telegram Bot](#telegram-bot)
- [Web GUI](#web-gui)
- [Hardware](#hardware)
- [Building](#building)
- [First Boot](#first-boot)
- [MQTT Topics](#mqtt-topics)
- [HTTP API](#http-api)
- [Changelog](#changelog)
- [License](#license)

---

## Features at a Glance

| Feature | Where to configure |
|---|---|
| 5-state alarm (DISARMED → ARMING → ARMED → PENDING → TRIGGERED) | Tab **Security** → Alarm Delay |
| Alert zones with entry delay / immediate / ignore modes | Tab **Zones** |
| Anti-masking / tamper detection | Tab **Security** → Anti-Masking |
| Pet immunity filter (low-energy objects < 2 m) | Tab **Security** → Pet Immunity |
| Loitering detection | Tab **Security** → Loitering |
| WiFi signal monitoring (jamming detection) | Tab **Security** → WiFi Security |
| Supervision heartbeat | Tab **Security** → Heartbeat |
| 14-gate radar sensitivity editor with presets | Tab **Gates** |
| Persistent event log (LittleFS, survives reboot) | Tab **History** |
| Scheduled arm / disarm + auto-arm on inactivity | Tab **Security** → Scheduled Arm/Disarm |
| MQTT auto-discovery for Home Assistant | Tab **Network & Cloud** → MQTT Broker |
| Telegram bot (`/arm`, `/disarm`, `/status`) | Tab **Network & Cloud** → Telegram |
| OTA firmware update | Tab **Network & Cloud** → Firmware Update |
| CZ / EN language toggle | Header button |
| Auto-calibration (noise baseline, 60 s) | Tab **Basic** → Calibrate Noise |

---

## Alarm State Machine

```
DISARMED ──[arm]──► ARMING (exit delay) ──► ARMED
                                              │
                                        [detection]
                                              │
                                           PENDING (entry delay)
                                              │
                                        [timeout/no disarm]
                                              │
                                         TRIGGERED
```

- **ARMING** — exit delay countdown lets you leave the protected area before the alarm activates.
- **PENDING** — entry delay gives you time to disarm after re-entering.
- **TRIGGERED** — siren / Telegram alert / MQTT event fired.

Controls: ARM/DISARM button in the main dashboard, Telegram commands, or MQTT `alarm/set` topic.

---

## Alert Zones

Each zone covers a distance range on the radar beam and defines what happens when motion is detected there.

| Zone behavior | Effect |
|---|---|
| **Entry delay** (default) | Starts PENDING countdown before triggering |
| **Immediate** | Triggers alarm instantly, no entry delay |
| **Ignore** | Detection in this range is silently ignored (e.g. a pet's fixed spot) |
| **Ignore Static Only** | Filters passive reflectors (furniture, metal) but still responds to motion |

Each zone has: name, distance range (cm), behavior, and an optional **entry path** — a required previous zone the target must have crossed first (e.g. window zone must have crossed hallway zone).

---

## Security Features

### Anti-Masking (Tamper Detection)
Detects when the sensor is physically covered or blocked. Raises a tamper alarm if the sensor is silent for longer than the configured threshold.
- Default: **off** (useful for sites where silence is normal, e.g. a storage unit).
- Configure: **Security → Anti-Masking** (timeout in minutes).

### Pet Immunity
Filters small, low-energy objects moving within 2 m of the sensor — cats, dogs, etc.
- Configure: **Security → Pet Immunity** (energy threshold 0–100).

### Loitering Detection
Raises an alarm if someone lingers near the sensor (< 2 m) beyond a set duration.
- Configure: **Security → Loitering** (timeout in seconds).

### WiFi Security Monitoring
Detects sustained RSSI drops that could indicate WiFi jamming. Uses an EWMA-smoothed baseline (α = 0.2, ~5 s window) plus two-consecutive-sample confirmation, so single noisy spikes do not trigger false alarms.
- Configure: **Security → WiFi Security** (RSSI threshold dBm, max allowed drop dB).

### Supervision Heartbeat
Sends a periodic "alive and guarding" message via Telegram to confirm the node is running.
- Configure: **Security → Heartbeat** (interval in hours, 0 = off).

---

## Gates (Radar Sensitivity)

The HLK-LD2412 divides its 10 m range into 14 detection gates (0–10 m, 0.71 m each). Each gate has independent sensitivity for **motion** and **static** presence.

- Visual editor in the **Gates** tab.
- Built-in presets: **Indoor**, **Outdoor**, **Pets**.

> **Gates vs Zones:** Gates are a physical radar parameter (what the sensor sees). Alert Zones are a logical layer on top (what the alarm does with detections).

---

## Event Log

Persistent security event log stored in LittleFS — survives reboots.

| Event type | Examples |
|---|---|
| **Alarm** | Triggered, armed, disarmed |
| **Presence** | Detected, cleared |
| **Tamper** | Sensor covered, cleared |
| **Heartbeat** | Supervision pulse |
| **Network** | WiFi RSSI anomaly, MQTT reconnect |
| **System** | Boot, OTA update, calibration |

- Filter by type via dropdown (All / Alarm / Presence / Tamper / Heartbeat / System / Network).
- Display: **Timeline** (visual) or **Table**.
- Export: **CSV download** via `/api/events/csv`.

---

## Scheduled Arm / Disarm

- Set a daily arm time and disarm time (HH:MM format).
- **Auto-arm**: arm automatically after X minutes of no presence detected.
- Requires correct timezone — configure in **Network & Cloud → Timezone** (UTC offset + DST).

---

## MQTT + Home Assistant

Auto-discovery: the node publishes HA discovery payloads on first connect. No manual YAML configuration needed.

Entities created automatically:

| Entity | HA type |
|---|---|
| Alarm control panel | `alarm_control_panel` |
| Presence | `binary_sensor` |
| Distance | `sensor` |
| Moving / static energy | `sensor` |
| Active zone | `sensor` |
| RSSI | `sensor` |
| Free heap | `sensor` |
| Health score | `sensor` |
| Hold time | `number` |
| LED enable | `switch` |

Configure broker: **Network & Cloud → MQTT Broker**.

---

## Telegram Bot

Interactive bot for notifications and remote control.

| Command | Action |
|---|---|
| `/arm` | Arm with exit delay |
| `/arm_now` | Arm immediately (no delay) |
| `/disarm` | Disarm |
| `/status` | Report current alarm state, distance, zone |

Notifications sent for: alarm trigger, tamper alert, heartbeat pulse.

Configure: **Network & Cloud → Telegram** (bot token + chat ID).

---

## Web GUI

- Responsive dark-mode dashboard.
- Real-time telemetry via SSE (Server-Sent Events).
- **CZ / EN** language toggle — state persisted in `localStorage`, EN default.
- Tabs: Basic | Gates | Zones | Security | History | Network & Cloud.
- Default credentials: `admin` / `admin` — **change on first boot**.

---

## Hardware

### Pin assignments

| Board type | RADAR_RX | RADAR_TX | OUT (alarm) |
|---|---|---|---|
| **Type B** (default) | GPIO 18 | GPIO 19 | GPIO 21 |
| **Type A** | GPIO 16 | GPIO 17 | GPIO 21 |
| Custom (e.g. ext. antenna) | GPIO 19 | GPIO 18 | GPIO 21 |

**Status LED:** GPIO 25

### Recommended components

- ESP32 DevKit — 4 MB flash minimum, 8 MB recommended
- HLK-LD2412 radar module (24 GHz FMCW, 0–10 m, UART)
- Power supply: 5 V / 500 mA or more

---

## Building

### Requirements

```
PlatformIO Core or VSCode + PlatformIO extension
Platform: espressif32 v6.9.0+
```

### Setup

```bash
# 1. Copy and fill in credentials
cp include/secrets.h.example include/secrets.h
# Edit: WiFi SSID/password, MQTT broker, Telegram bot token

# 2. Copy known_devices (optional, for multi-node deployments)
cp include/known_devices.h.example include/known_devices.h

# 3. Build and flash
pio run -e esp32_type_B --target upload

# 4. Monitor serial output
pio device monitor -b 115200
```

### Build environments

| Environment | Board | Notes |
|---|---|---|
| `esp32_type_B` | GPIO 18/19, OUT 21 | Default; serial debug enabled |
| `esp32_type_A` | GPIO 16/17, OUT 21 | Older boards |
| `esp32_ext_ant` | GPIO 19/18 (TX/RX swapped), OUT 21 | External antenna variant |

OTA environments (once the device is on the network):

```bash
pio run -e ota_<device_name> --target upload
```

---

## First Boot

1. ESP32 creates a setup access point: **`LD2412_Setup_XXXX`** (password: `ld2412setup`).
2. Connect and open `http://192.168.4.1`.
3. Set WiFi credentials and MQTT broker address.
4. Device connects, publishes HA auto-discovery, and starts guarding.

Web GUI: `http://<device-ip>/` — default login: `admin` / `admin` (**change immediately**).

---

## MQTT Topics

Base topic: `security/<device_id>/`

| Topic | Direction | Values / type |
|---|---|---|
| `presence/state` | → HA | `idle` / `detected` / `hold` / `tamper` |
| `presence/distance` | → HA | distance in cm |
| `presence/energy_mov` | → HA | moving energy 0–100 |
| `presence/energy_stat` | → HA | static energy 0–100 |
| `presence/zone` | → HA | active alert zone name |
| `alarm/state` | → HA | `disarmed` / `arming` / `armed_away` / `pending` / `triggered` |
| `alarm/set` | ← HA | `arm` / `arm_away` / `arm_home` / `disarm` |
| `alarm/event` | → HA | JSON: zone, reason, distance, timestamp |
| `rssi` | → HA | WiFi RSSI in dBm |
| `heap` | → HA | free RAM in bytes |
| `health_score` | → HA | sensor health 0–100 % |

---

## HTTP API

All endpoints require HTTP Basic Auth.

| Endpoint | Method | Description |
|---|---|---|
| `/api/health` | GET | Uptime, WiFi, MQTT, heap, hostname, reset history |
| `/api/telemetry` | GET | Radar state, distance, energy, UART stats |
| `/api/config` | GET / POST | Full device configuration |
| `/api/config/export` | GET | Export config as JSON |
| `/api/config/import` | POST | Import config from JSON |
| `/api/alarm/status` | GET | Current alarm state |
| `/api/alarm/arm` | POST | Arm (with exit delay) |
| `/api/alarm/disarm` | POST | Disarm |
| `/api/events` | GET | Event log (`?type=alarm\|presence\|tamper\|…`) |
| `/api/events/csv` | GET | Event log as CSV download |
| `/api/zones` | GET / POST | Alert zone definitions |
| `/api/schedule` | GET / POST | Scheduled arm/disarm times, auto-arm timeout |
| `/api/timezone` | GET / POST | UTC offset, DST offset |

---

## Changelog

See [CHANGELOG.md](CHANGELOG.md).

---

## License

MIT — see [LICENSE](LICENSE).

---

## Acknowledgements

Radar protocol: HLK-LD2412 datasheet + community reverse engineering.

Built with [Claude Code](https://claude.ai/claude-code).
