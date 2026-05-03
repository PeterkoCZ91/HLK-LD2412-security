# LD2412 Security Node

ESP32-based security node with HLK-LD2412 24GHz FMCW mmWave radar for presence detection.

## Version
**v3.0.6-STABLE-MEM** (2026-02-06)

## 🚀 Recent Updates (v3.0.6)
- **Memory Hardening:** Recovered ~10KB heap to prevent crashes during web/MQTT activity.
- **Task Optimization:** Connectivity and Webhook tasks optimized for lower stack usage.
- **Buffer Management:** Log and Event buffers optimized (50 -> 20 entries).
- **Workspace Cleanup:** Reduced AI context size by archiving legacy files to `cleanup_archive/`.

## 🚀 New Features (v3.0)
- **Modular Architecture:** Core logic separated from Web and Config services.
- **Bluetooth Emergency Provisioning:** Rescue interface available if WiFi is lost for > 2min.
- **Enhanced Stability:** Stabilized memory by removing redundant HTTP Auto-update.

## Features
- 24GHz mmWave radar presence detection (LD2412)
- MQTT with Home Assistant auto-discovery
- Web interface (Responsive Dashboard)
- Security features:
  - Anti-masking/tamper detection
  - Pet immunity filter
  - Auto-calibration

## Hardware

### ESP32 desky v labu

| # | MAC | Zařízení | Piny RX/TX/OUT | Typ | Prostředí |
|---|-----|----------|-----------------|-----|-----------|
| 1 | `04:83:08:59:58:cc` | `senzor-kuchyn` / `ld2412_kuchyn` | **19/18/21** | Type B, prohozené RX/TX | `esp32_ext_ant` |
| 2 | `a0:a3:b3:aa:e6:28` | `senzor-obyvak` / `ld2412_obyvak` | **18/19/21** | B | `esp32_type_B` |
| 3 | `e4:65:b8:7b:6b:98` | `senzor-chodba` / `ld2412_chodba` | **18/19/21** | B | `esp32_type_B` |

Posledni overeni `04:83:08:59:58:cc`: 2026-04-23, IP `192.168.68.92`, firmware `v3.11.1`, radar UART `RUNNING` s profilem `esp32_ext_ant`.

### Společné piny
| Function | GPIO |
|----------|------|
| Status LED | 25 |

### WiFi
**Hardcoded fallback** na lab síť (`secrets.h`) - žádná manuální konfigurace!

## Building

### Requirements
- PlatformIO Core or VSCode + PlatformIO extension
- espressif32 platform v6.9.0

### Build Commands
```bash
# Zjisti MAC připojeného ESP:
esptool.py --port /dev/ttyUSB0 chip_id

# Upload podle typu:
pio run -e esp32_type_A -t upload   # Deska #1 (piny 16/17)
pio run -e esp32_type_B -t upload   # Desky #2, #3 (piny 18/19)

# Monitor
pio device monitor -b 115200
```

## Configuration

### First Boot
1. ESP32 creates AP: `LD2412_Setup_XXXX`
2. Connect and configure WiFi + MQTT settings
3. Device connects and publishes HA discovery

### Web Interface
Access via `http://<device-ip>/`
- Default credentials: `admin` / `admin`

### MQTT Topics
Base: `security/<device_id>/`

| Topic | Description |
|-------|-------------|
| `presence/state` | idle/detected/hold/tamper |
| `presence/distance` | Distance in cm |
| `presence/energy_mov` | Moving energy (0-100) |
| `presence/energy_stat` | Static energy (0-100) |
| `tamper` | Tamper alert state |
| `config/*/set` | Configuration commands |

## Project Structure
```
LD2412/
├── src/
│   ├── main.cpp              # Main application
│   └── services/
│       ├── LD2412Service.cpp # Radar driver wrapper
│       ├── MQTTService.cpp   # MQTT + HA Discovery
│       ├── SecurityMonitor.cpp
│       ├── NotificationService.cpp
│       └── TelegramService.cpp
├── include/
│   ├── secrets.h             # Credentials (not in git)
│   ├── web_interface.h       # Premium GUI (HTML/CSS/JS)
│   └── services/*.h
├── builds/                   # Pre-built firmware binaries
├── docs/                     # Extended documentation
└── platformio.ini
```

## Changelog

### v1.20.2-DEBUG-DIST (2026-01-25)
- **Fixed GUI JavaScript syntax error** - chybějící if statement způsoboval nefunkční GUI
- Odstraněn duplicitní řádek v fetchData()
- GUI nyní správně načítá hold_time z telemetrie

### v1.20.0-LD2412-GOLD (2026-01-25)
- Native Library integrace (tobiastl/LD2412)
- Premium GUI v1.18 style (Dark Mode)
- Engineering Mode (diagnostika)
- Auto-Tune kalibrace (10s)
- Virtuální zóny

### v1.19.0-LD2412-PROD (2026-01-25)
- Complete rebranding from LD2410 to LD2412
- **3 ESP32 desky** - Typ A (16/17) a Typ B (18/19)
- **Hardcoded WiFi fallback** - automatické připojení k lab síti
- Fixed MQTT command callback handling
- Fixed preprocessor MQTTS_ENABLED (use 1/0 instead of true/false)
- Removed radar debug flooding Serial output
- WDT initialization moved after WiFi connection
- Added HTTP Digest authentication on all endpoints
- SecurityMonitor and NotificationService now active
- snprintf buffer overflow protection
- Build-time pin configuration via platformio.ini

## License
Private project

## Author
Built with Claude Code assistance
