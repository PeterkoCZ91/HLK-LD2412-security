# Changelog

All notable changes to the LD2412 Security System firmware are documented in this file.
Format based on [Keep a Changelog](https://keepachangelog.com/).

## [v3.11.0] - 2026-04-12
### Added
- **Entry/exit path validation**: Zones can require a specific previous zone. If an intruder enters through an unexpected path (e.g. window instead of door), the alarm triggers immediately instead of using entry delay.
- **Supervision heartbeat**: Nodes publish periodic heartbeat via MQTT (`security/+/supervision/alive`). If a peer node stops reporting for 3 minutes, a tamper alert is raised on remaining nodes.
- **Multi-sensor mesh**: Cross-node alarm verification via MQTT. When an alarm triggers, nearby nodes are asked to confirm detection. Alarm events include `mesh_verified` status for forensic review.
- **Event timeline UI**: New visual timeline in History tab with color-coded event markers. Toggle between timeline and table view.

## [v3.10.2] - 2026-04-10
### Fixed
- **Live SSE telemetry broken since v3.10.0**: SSE buffer was 256 B but telemetry JSON is ~700-900 B. `serializeJson` returned `sizeof(sseBuf)` on overflow, the `sseLen < sizeof(sseBuf)` guard rejected every event, so the web dashboard never updated distance/energy/state values. Buffer increased to 1024 B (worst case ~930 B with eng_mode arrays). Stack allocation, no heap impact.

## [v3.10.1] - 2026-04-04
### Fixed
- Gate config verification 40s post-boot (ESPHome #13366 V1.26 revert workaround)
- `processRadarData` mutex timeout increased 10ms to 50ms (prevent missed alarm transitions)
- Alarm event queue overflow alerting with `enqueueAlarmEvent` helper (log on drop)
- `enableConfig` serial drain before retry (prevent stale UART data misparse)
- `getAckNonBlocking` early length validation (ESPHome #14297 analogue)

## [v3.10.0] - 2026-04-03
### Added
- **Auto-arm**: configurable idle timeout (`auto_arm_minutes`, 0=disabled) -- when disarmed with no presence for N minutes, auto-arm + Telegram notify
- **Scheduled arm/disarm**: time-based HH:MM scheduling (`sched_arm_time`/`sched_disarm_time`)
- **Timezone**: configurable `tz_offset`/`dst_offset` (was hardcoded GMT+1)
- **CSV export**: `GET /api/events/csv` endpoint
- New API endpoints: `GET/POST /api/schedule`, `GET/POST /api/timezone`

### Changed
- SSE telemetry: `String` replaced with stack `char[256]` (-4 heap alloc/s)
- MQTT Tier 2+3: `String(val)` replaced with `snprintf` (-15 heap alloc/cycle)
- Telegram queue size increased 16 to 32
- SecurityMonitor mutex timeout increased 50ms to 200ms
- MQTT sensitivity: bounds validation on all parameters

## [v3.9.7] - 2026-04-03
### Added
- `safeRestart` saves heap/maxAlloc/minHeap to NVS for postmortem analysis
- `restart_cause` MQTT now includes heap snapshot from previous run
- STAB log extended with MinFreeHeap + MaxAllocHeap

### Fixed
- EventLog snapshot alloc uses `std::nothrow` + null check (prevents crash on fragmented heap)

## [v3.9.6] - 2026-03-28
### Added
- **MQTT Offline Buffer**: LittleFS ring buffer (30 slots, 268B each, ~8KB) persists alarm events across reboots, replays after reconnect
- EventLog capacity increased from 20 to 100 entries (~5.6KB RAM+disk)
- Main loop drains offline buffer every cycle after MQTT reconnect

## [v3.9.5] - 2026-03-28
### Changed
- DMS now tries MQTT disconnect+reconnect before escalating to ESP restart
- Added 60s grace period after reconnect
- Removed stale `connected()` check from DMS condition (catches half-open TCP)
- New `forceReconnect()` method in MQTTService

## [v3.9.4] - 2026-03-25
### Fixed
- Skip DMS restart when RSSI < -85 (restart will not fix weak signal)
- Reset DMS counter only after successful MQTT publish (not just connected)
- After DMS/watchdog restart, restore armed state immediately (no exit delay)

## [v3.9.3] - 2026-03-24
### Changed
- Telegram `sendMessageDirect` mutex timeout reduced 5s to 2s (unblocks command polling)
- Removed `calculateChecksum()` dead code from LD2412 parser
- Restructured test directories: `test_native_parser` + `test_native_security`

### Added
- 28 security logic tests: alarm state machine (8), debounce (5), entry delay (3), trigger timeout (3), millis overflow (2), Telegram queue (4), full cycle (1), edge cases (2)

## [v3.9.2] - 2026-03-24
### Fixed
- Frame rate div/0 guard + 50Hz sanity cap (LD2412.cpp)
- SecurityMonitor `update()` mutex timeout increased 10ms to 50ms + debug log
- Telegram queue increased 8 to 16 + drop counter for observability
- Zone `alert_level` 3 decoupled from `TAMPER_ALERT` cooldown

### Changed
- Batch UART read: `readBytes()` + `ringPushBatch()` -- single syscall instead of N, single critical section for entire batch
- `memcmp()` for header/footer validation in `validateFrame()`

## [v3.9.1] - 2026-03-23
### Added
- **Armed-side debounce**: require 3 consecutive qualifying frames before ARMED to PENDING/TRIGGERED (filters single-frame radar spikes)
- Configurable via `/api/alarm/config?debounce_frames=N` (persisted to NVS)
- Default: 3 frames (150ms at 50ms tick)

## [v3.9.0] - 2026-03-23
### Fixed
- `setArmed()` now idempotent -- rejects PENDING/TRIGGERED, always deactivates siren on disarm
- Dedicated `ALARM_TRIGGERED` NotificationType -- real alarms no longer routed through tamper
- `triggerAlert()` logs explicit distance, not stale `_lastDistance`
- MQTT alarm events use peek/consume -- only consumed after successful publish
- Auto-rearm clears approach log (no stale forensic trail)
- Offline reconnect distinguishes real TRIGGERED from tamper/blind/loitering
- `RadarData.valid` flag -- skip `processRadarData` on mutex timeout
- MQTT `lastPub` gated on publish success for critical retained topics
- Security-critical notifications bypass cooldown
- Webhook payload buffer increased 256 to 512, overlength rejected
- Parser VERIFY_FOOTER inline fallthrough (no extra byte needed)
- EventLog mount without auto-format first
- `flushNow()` for critical security events
- Alarm event queue fully mutex-protected (peek/consume/has)

## [v3.8.5] - 2026-03-22
### Added
- **Approach tracker**: records last 16 detections while ARMED with timestamp, distance, move/static energy; included in ENTRY_DETECTED and ALARM_TRIGGERED notifications for forensic analysis
- Learn duration backend constraint raised from 600s to 28800s (8h)
- GUI options added for 4h and 8h overnight learning
- Approach log clears on ARM, deduplicates entries <2s apart

## [v3.8.4] - 2026-03-22
### Added
- **API credential masking**: GET endpoints return `***` for sensitive fields; POST handlers skip `***` values to prevent overwriting real credentials
- Telegram test: `sendMessageDirect` with `wait=true` returns real success/failure; test message enriched with device name, IP, FW version

### Fixed
- Event log GUI: epoch timestamps displayed as "22.3. 18:39" instead of broken "492832h 19m" (NTP-synced events use `toLocaleString`, fallback to uptime format)

## [v3.8.1] - 2026-03-21
### Changed
- **Atomic RadarSnapshot**: new `RadarSnapshot` struct + `readSnapshot()` -- single `readSerial()` call for all 5 values
- `LD2412Service::update()` uses snapshot instead of 5 separate getters (data consistency)
- Native tests rewritten: test the real LD2412 class, not a parser copy (13 tests PASS)
- ArduinoMock extended with ESP32 portMUX primitives for native build

## [v3.8.0] - 2026-03-21
### Fixed
- Alarm tick interval changed from 1Hz to 50ms (short detections can no longer be missed)
- MQTT reconnect forces full state replay (`lastPub` invalidation)
- WiFi failover returns to SDK saved credentials, not hardcoded
- EventLog flush race -- sequence counter, dirty flag only if unchanged
- Webhook retry 3x with backoff, re-queue on WiFi down
- Telegram queue re-queue on offline, retry on failed send
- Hard recovery of parser restores engineering mode after restart
- `_pendingEvent` overwrite replaced with ring queue[4] for burst alarms
- Radar init passive mode returns false (`_initSuccess=false`)
- Oversized POST body returns 413 instead of silent ignore
- Resolution mapping mode==3 maps to 0.20m (was incorrectly mode==2)

## [v3.7.9] - 2026-03-21
### Added
- MQTT `alarm/event` topic: atomic JSON on PENDING/TRIGGERED/DISARMED (reason, zone, distance, energy, motion_type, uptime, time)
- MQTT `presence/motion_type`: "moving"|"static"|"both"|"none" (Tier 1, retained)
- `static_filtered`: known reflector (behavior=3) suppressed even when DISARMED -- HA automations will not trigger
- `/api/telemetry`: `static_filtered` flag
- `/api/alarm/status`: `current_zone` + `last_event` (persistent, with ISO time)
- `/api/health`: `ntp_synced` + `current_time`
- Telegram test: `sendMessageDirect()` returns real result instead of queue bool
- `sendMessageDirect` mutex (thread-safe Core 0 vs Core 1)
- EventLog: epoch timestamp instead of uptime seconds (when NTP synced)
- `AlarmTriggerEvent`: `iso_time` field ("2026-03-21T22:14:05")
- DNS fallback: 8.8.8.8 as secondary DNS after WiFi connect (fix broken DHCP DNS)

## [v3.7.5] - 2026-03-21
### Changed
- Hostname: NVS takes priority over KNOWN_DEVICES (GUI hostname change survives reboot)
- GUI: added 30 min and 60 min options to auto-learn duration selector

## [v3.7.3] - 2026-03-21
### Added
- Telegram `/status` extended: fw_version, uart_state, frame_rate, static_gate, moving_gate, current_zone
- Telegram `/learn` command: starts static learn (3 min) or shows progress if already running
- Telegram `/start` and `/help` updated with new commands
- Main loop: Telegram notification on learn session completion with results
- Group chat support: negative IDs, @mention stripping

## [v3.7.2] - 2026-03-21
### Added
- **Auto-learn static zone**: `startStaticLearn(duration_s)`, `getLearnResultJson()` -- collects clean static data (mov<10%, stat>15%), counts frequency per gate
- API: `/api/radar/learn-static` POST (start) + GET (status/result)
- Suggests zone +/-1 gate around most frequent static_gate

### Added (GUI)
- SVG zone map: zones colored, live position for static (purple) + moving (blue)
- "Learn static" button with progress + result display
- `applyLearnZone()`: add suggested zone with alarm_behavior=3 in one click

## [v3.7.1] - 2026-03-21
### Added
- Zone editor: "Alarm behavior" selector in GUI (Entry delay, Immediate, Ignore, Ignore static)
- `alarm_behavior` now included in `saveZones()` and persisted to NVS

## [v3.7.0] - 2026-03-21
### Added
- `/api/health` extended: `fw_version`, `build.rx_pin`/`tx_pin`/`out_pin` (compile-time pins for Type A/B identification), `chip.model`/`revision`/`cores`/`mac`/`flash_size`

## [v3.6.9] - 2026-03-21
### Added
- Gate numbers in telemetry output
- `ignore_static_only` alarm behavior mode

## [v3.6.8] - 2026-03-13
### Fixed
- Bluetooth: `/api/bluetooth/start` wrapped in `#ifndef NO_BLUETOOTH` (prevents null-deref crash in production builds)
- OTA: `vTaskSuspend(radarTaskHandle)` before `radar.stop()` in OTA handler (prevents use-after-free race)
- TOCTOU: `LD2412Service::update()` checks `_radar` under mutex instead of before it
- `restart_cause`: `g_prevRestartCause` saved in `setup()`, `loop()` reads RAM (NVS is already overwritten with "none")
- EventLog: `flush()` takes snapshot under mutex, `saveToDisk()` accepts parameters, `_dirty=false` only set on successful write
- Strings: null termination after `strncpy` in WiFiManager callback and KNOWN_DEVICES block; MQTTService buffers unified with SystemConfig (`_server` 40 to 60, `_user`/`_pass` 32 to 40)
- Tests: `test_parser.cpp` fixed to correct LD2412 protocol offsets (payload starts at [8])
- Disarm reminder default changed from true to false

## [v3.6.6] - 2026-03-13
### Fixed
- WebRoutes POST `/api/telegram/config`: `hasParam()` only reads query string, not POST body -- added `getP()` helper that checks body params first
- `TelegramService::begin()`: token default overwrite condition was too broad; now only writes defaults on first boot (key missing)
- GET `/api/telegram/config`: reads from runtime object (`getToken`/`getChatId`) instead of NVS directly (was returning stale/empty values)

## [v3.6.3] - 2026-03-13
### Fixed
- Hostname always force-set from KNOWN_DEVICES table on boot (NVS stored hostname could persist stale/wrong names across reflashes)

## [v3.6.2] - 2026-03-13
### Fixed
- `TelegramService::begin()` was force-writing `tg_direct_en=false` on every boot, making the bot impossible to enable via GUI; now reads the saved NVS value instead

## [v3.6.1] - 2026-03-12
### Added
- New OTA environments: `ota_loznice` (.44), `ota_chodba_krabicka` (.54)
- STAB log extended with MQTT status

### Fixed
- Hostname change in GUI now triggers reboot
- On boot, NVS hostname takes priority over KNOWN_DEVICES table
- MQTT HA discovery: missing `{` in devInfo JSON
- GUI restart button: added `method:'POST'`

### Changed
- Device rename: .44 to `ld2412_obyvak_stul`, .54 to `ld2412_loznice_skrin`

## [v3.6.0] - 2026-02-27
### Added
- **Gates tab overhaul**: batch endpoint `/api/radar/gates` (2 UART commands instead of 84), bulk "set all" controls, range summary, colored legend, presets update sliders in-place

### Fixed
- Resolution commands: `setResolution` cmd 0x05 to 0x01, payload 4B to 8B, mode 0.20m=3 (ESPHome-compatible)
- `getResolution()` query cmd 0x15 to 0x11, ACK length 15 to 20
- Radar restart after resolution change (per ESPHome protocol)
- All POST handlers: `URLSearchParams` body to query params (ESPAsyncWebServer `hasParam()` only checks query)
- `saveAuth()` now works -- password persists across reboots
- Preset route `/api/config/preset` to `/api/preset` (prefix match fix)

## [v3.5.1] - 2026-02-12
### Fixed
- Removed WDT from radarTask/connectivityTask (connectivityTask sleeps 60s in `vTaskDelay` which starves the 60s WDT timeout)

## [v3.5.0] - 2026-02-12
### Added
- `safeRestart()` with NVS logging, `restart_cause` MQTT sensor
- Pet immunity: filter both move and static energy
- WDT registration for radarTask and connectivityTask
- Mutex added to SecurityMonitor, EventLog, LogService

### Fixed
- Replace Google connectivity check with WiFi+MQTT status check
- WiFi failover: 10 attempts with exponential backoff (>30min tolerance)
- DMS: init `_lastPublish=millis()`, require MQTT connected, 30min timeout
- Fix `getTelemetryJson()` deadlock on non-recursive mutex
- Fix pointer comparison for state strings (`strcmp` + `char[]` storage)

### Changed
- Increase webhook task stack 4096 to 8192, `_deviceId[32]` to `[40]`
- WebRoutes: body size limit, JSON validation, memory leak fix

## [v3.4.4] - 2026-02-11
### Added
- **Factory reset**: hold BOOT button (GPIO 0) 5s at boot to clear NVS + WiFi config
- **3-tier MQTT publish-on-change**: critical (immediate), sensor (2s+deadband), diagnostic (30s+deadband)
- Deadband thresholds in `constants.h` to reduce MQTT traffic

## [v3.4.3] - 2026-02-11
### Fixed
- Force-disable ESP Telegram and disarm reminder (fusion Docker handles all Telegram notifications; ESP was still spamming reminders)

## [v3.4.2] - 2026-02-11
### Fixed
- Disable loitering alert on ESP (moved to fusion Docker service; ESP-level alerts were spamming MQTT)

## [v3.4.0] - 2026-02-10
### Added
- V1.26 startup optimization: skip broken UART commands (`setBaudRate`, `setEngineeringMode` ACK but never switch on V1.26)
- Engineering mode MQTT topic + HA Discovery for remote monitoring
- Engineering mode diagnostics and recovery limit (3 attempts then fallback to Basic Mode)

### Fixed
- V1.26 FW: ACKs engineering mode command but never switches -- caused infinite recovery loop; now limits recovery to 3 attempts
- Telegram task stack increased to 10KB (prevent TLS handshake overflow from WiFiClientSecure + mbedTLS)

## [v3.2.0] - 2026-02-10
### Added
- Auto-enable Engineering Mode on boot (3 retries) + auto-recovery every 10s
- Engineering mode loss detection (5 consecutive basic frames trigger flag)
- `eng_mode_lost` flag in telemetry JSON output
- Compile-time `NO_BLUETOOTH` to eliminate NimBLE RAM usage (~40KB saved)
- `radar_type` MQTT topic for sensor fusion identification
- `ota_obyvak` OTA environment

### Changed
- SecurityMonitor: device label prefix in Telegram alerts
- Tamper alert cooldown increased from 1min to 10min
- Stack optimizations: connectivity task 4K to 3K, Telegram task 8K to 6K

## [v3.1.1] - 2026-02-06
### Fixed
- FreeRTOS cross-core mutex assertion crash (`vTaskPriorityDisinheritAfterTimeout`): changed 7 health/status mutex wrappers in LD2412Service to non-blocking (timeout=0)
- Heartbeat unsigned underflow in SecurityMonitor causing immediate heartbeat on boot

### Changed
- Deduplicate Telegram notifications: removed duplicate `sendMessage` from `/arm`, `/arm_now`, `/disarm` handlers (SecurityMonitor is the single source)
- Boot NVS arm restore uses exit delay instead of immediate (prevents instant trigger on reboot)
- Sync ConfigManager defaults with `secrets.h` and persist on first boot
- `ota_chodba` fixed: extends Type A (RX=16/TX=17), not Type B

## [v3.1.0] - 2026-02-06
### Added
- **Alarm state machine**: TRIGGERED timeout (15min default) with auto-rearm option
- Zone-alarm integration: `alarm_behavior` per zone (entry_delay/immediate/ignore)
- Min energy threshold for ARMED to PENDING (reduces false alarms from noise)
- Presence warning at ARMING to ARMED transition
- Siren GPIO output support (activates on TRIGGERED, deactivates on timeout/disarm)
- BLE passkey authentication (encrypted characteristics)
- SPIFFS to LittleFS migration (better wear leveling)
- mDNS registration (`hostname.local` discovery)

### Fixed
- MQTT exponential backoff (5s to 10s to 20s...300s max) instead of fixed 5s
- Mutex protection for 7 LD2412Service health methods (race condition fix)
- `millis()` overflow fix for OTA validation and DMS startup checks
- NVS write wear reduction (uptime save interval 10min to 1h)
- Heartbeat timer: static local to member variable

## [v3.0.7] - 2026-02-06
### Added
- Async Telegram: `sendMessage()` non-blocking via FreeRTOS queue + background task
- Non-blocking MQTT discovery: state machine publishes 1 entity per `update()` cycle (eliminates ~5s blocking delay)
- Split NotificationType: `ALARM_STATE_CHANGE` + `ENTRY_DETECTED` (always send)

### Fixed
- Distance/10 bug in security alerts (was dividing cm by 10)
- WiFi failover counter reset on reconnect
- BLE WiFi namespace (correct keys)
- `millis()` overflow in startup LED and Telegram mute timer
- GUI: HOLD state display, MQTT user field, POST body for Telegram/alarm config
- Reboot only on resolution change (not hold_time/sensitivity)
- Telegram `/restart` via rebootFlag (no more delay+restart in bot task)
- MQTT topic buffers increased 48 to 64 chars (prevent overflow)
- Disarm reminder requires 10s sustained detection (prevents noise triggers)
- Removed duplicate `configManager.load()` call

## [v3.0.6] - 2026-02-06
### Changed
- **Critical memory optimization**: freed ~10KB heap for AsyncWebServer stabilization
- Task stack refactor: reduced stack size for `conn_check` and `webhook_task` from 6KB to 4KB
- Log buffers: ring buffers for `systemLog` and `eventLog` reduced from 50 to 20 entries
- MQTT topic buffers reduced from 80B to 48B
- Webhook payload buffer reduced from 512B to 256B

## [v3.0.5] - 2026-02-06
### Changed
- Async webhooks: Discord/generic webhooks now async via FreeRTOS queue+task (no more blocking loop)
- LogService: `std::deque` replaced with fixed-size ring buffer (~2KB heap savings per 50 entries)
- LD2412Service: added `getGateEnergiesSafe()` for thread-safe energy reads
- BluetoothService: stack-allocated callbacks instead of heap-allocated (fix memory leak)
- WebRoutes: mutex protection on zones JSON, raw char buffers instead of String for body parsing
- Health score uses consecutive errors + UART state instead of accumulated counters

## [v3.0.4] - 2026-02-06
### Added
- `/api/radar/factory_reset` endpoint
- Aggressive (no-ACK) reset/restart fallback in LD2412Service
- "Reset MW" button in web interface
- Light sensor telemetry integration

## [v3.0.3] - 2026-02-05
### Fixed
- **WiFi failover**: implemented missing failover logic for switching to backup network
- **LITE_BUILD**: fixed compilation error -- `server.begin()` moved to correct guard
- **Race condition**: POST body handlers (zones, import) now use per-request buffer instead of static
- **Semaphore bypass**: `setTamperDetected()` no longer modifies state without mutex protection
- **Ring buffer thread safety**: added portMUX for ESP32 multi-core safety
- **Blocking delays**: `delay()` replaced with `vTaskDelay()`/`yield()` in recovery functions
- **SSE reconnect**: GUI automatically restores connection on dropout

### Added
- **Light function/threshold**: configure OUT pin reaction to light (night mode)
- **Query resolution**: read current resolution from radar
- **Presence timeout**: configure unmanned duration in GUI
- **Radar BT management**: Bluetooth module control on radar
- **SSE status icon**: visual indicator of realtime connection status in GUI
- Gate sensitivity endpoint `/api/radar/gate` + fixed GUI function

### Changed
- Cleaned up duplicate code in main.cpp
- Certificate check no longer reconfigures NTP
- Static heartbeat timer correctly initialized

## [v3.0.2-STABLE] - 2026-02-05
### Fixed
- **Memory optimization**: removed HTTP Auto-update which caused boot-loops due to insufficient RAM
- **Bluetooth stabilization**: Bluetooth kept as emergency mode but with memory freed after unsuccessful update attempts

## [v3.0.1] - 2026-02-05
### Added
- **Bluetooth (NimBLE) emergency service**: rescue access via BLE for WiFi configuration during network outage
- **Safety mode**: Bluetooth activates automatically on WiFi loss (>2 min) or manually via `/api/bluetooth/start` to save RAM
- Improved MQTT stability: added 100ms delay between discovery messages to protect network stack

## [v2.9.0] - 2026-02-05
### Changed
- **Final main.cpp refactoring**: all remaining HTTP routes (alarms, logs, exports) moved to `WebRoutes` module
- main.cpp reduced to ~850 lines with cleaner web service initialization
- `zonesJson` correctly passed to `WebRoutes` module via Dependencies

## [v2.8.9] - 2026-02-05
### Fixed
- **WebRoutes crash fix**: lambda capture `[&deps]` caused Guru Meditation Error (LoadProhibited) -- reference to local variable after `setup()` returned; fixed by using a static copy of `Dependencies` struct

## [v2.8.8] - 2026-02-05
### Fixed
- Added missing `checkAuth()` declaration in `WebRoutes.h`, fixed all calls to `WebRoutes::checkAuth()` in main.cpp
- **LD2412 resolution modes**: fixed support for all 3 HW resolution modes (Mode 0: 0.75m/gate, Mode 1: 0.50m/gate, Mode 2: 0.20m/gate)
- Added `ResolutionMode` enum to LD2412 library

### Changed
- Refactoring complete: WebRoutes and ConfigManager fully integrated, main.cpp reduced from ~1600 to ~1080 lines
- API endpoint `/api/config` now accepts resolution values 0.20, 0.50, 0.75

### Added
- `include/constants.h` -- centralized time constants and thresholds
- `include/WebRoutes.h` + `src/WebRoutes.cpp` -- HTTP route handlers
- `include/ConfigManager.h` + `src/ConfigManager.cpp` -- configuration management

## [v2.8.7] - 2026-02-02
### Fixed
- **Serial behind DEBUG flag**: new `include/debug.h` with `DBG()` macro; ~80 `Serial.print*` calls replaced with `DBG()` in services; critical logs (boot banner, WiFi IP, fatal errors) always enabled; `SERIAL_DEBUG=1` for lab, `=0` for production
- **Deprecated `beginResponse_P`**: replaced with `beginResponse()` in main route
- **Blocking delay fixes**: serial init 1s to 500ms, portal fail 3s to 1s, LITE reboot 10s to 2s
- **Heap guard**: MQTT publish and SSE telemetry skip when `ESP.getFreeHeap() < 20000`
- GUI default password warning banner updated

## [v2.8.6] - 2026-02-02
### Fixed
- **Telegram polling revert**: all previous "fixes" (deferred `begin()`, blocking `sendMessage`, `reset()` after each send) broke working polling; reverted TelegramService.cpp to logic from working backup
- Kept 2 new fixes: `PRId64` for group chat ID conversion (negative numbers on ESP32), strip `@BotName` suffix from group commands

## [v2.8.5] - 2026-02-02
### Added
- Telegram commands: `/arm`, `/disarm`, `/arm_now` + `_secMon` reference
- Chat ID conversion via `PRId64` for negative group IDs
- Strip `@BotName` suffix from group commands

### Fixed
- Telegram polling fix attempts (deferred `begin()`, blocking send, `reset()`) -- all broke polling, superseded by v2.8.6

## [v2.8.2] - 2026-02-02
### Added
- **GUI password change**: form for changing access credentials in Network & Cloud tab; password match validation, minimum 4 characters

## [v2.8.1] - 2026-02-02
### Changed
- **GUI localization**: unified all text to Czech -- alarm states (ARMED/DISARMED/ALARM), buttons, labels, placeholders; technical abbreviations (MQTT, RSSI, FPS) kept as-is

## [v2.8.0] - 2026-02-02
### Added
- **Armed/Disarmed mode**: complete alarm system with entry/exit delay (default 30s)
  - States: DISARMED -> ARMING (exit delay) -> ARMED -> PENDING (entry delay) -> TRIGGERED
  - Telegram commands: `/arm`, `/disarm`, `/arm_now`, status in `/status`
  - REST API: `POST /api/alarm/arm`, `POST /api/alarm/disarm`, `GET /api/alarm/status`, `POST /api/alarm/config`
  - MQTT `alarm_control_panel` discovery (HA integration), subscribe `alarm/set`, publish `alarm/state`
  - GUI: ARM/DISARM button on main card, entry/exit delay config in Security tab
  - SSE telemetry: `armed` and `alarm_state` fields
  - NVS persistence: `sec_armed`, `sec_entry_dl`, `sec_exit_dl`, `sec_dis_rem`
- **"Still DISARMED" reminder**: when presence detected in DISARMED state, sends notification every 30 min

### Fixed
- **Duplicate Telegram alert removed**: deleted direct `telegramBot.sendAlert()` in main.cpp -- SecurityMonitor already sends alerts via NotificationService
- **Serial spam**: `checkRSSIAnomaly`/`checkTamperState`/`checkRadarHealth` + `processRadarData` were called every loop cycle (~50,000x/s); all rate-limited to 1s
- **CPU tight-loop**: added `delay(10)` at end of `loop()` -- reduced CPU load and serial output from 500 KB/s to <10 B/s
- **GUI PROGMEM crash**: `request->send()` with 34KB PROGMEM string returned empty response (content-length: 0); fixed with `beginResponse_P()` -- streams from flash without RAM copy; min heap improved from 73KB to 104KB
- **EventLog flush**: cleaned up rate-limit logic

### Changed
- Zone and loitering alerts sent only in ARMED state (tamper always)
- LED strobe conditional on armed state
- Offline alarm memory conditional on armed state

## [v2.7.1-REVIEW] - 2026-02-01
### Fixed
- **processRadarData received mm instead of cm**: loitering (<2m), zone alerts, and event log had 10x wrong distance units; removed `* 10` from call in main.cpp
- **EventLog double conversion**: `_lastDistance / 10` in `triggerAlert` was redundant since data is already in cm
- Missing `#include <vector>` in SecurityMonitor.h for `std::vector<AlertZone>`
- Duplicate section number "3." in processRadarData corrected to 4. and 5.

## [v2.7.0-RC1] - 2026-02-01
### Added
- **Event log system**: persistent logging of security events (Presence, Tamper, WiFi, System) to SPIFFS; history viewable in web GUI
- **Zone management**: configurable alert zones (LOG, INFO, WARN, ALARM) with specific distance ranges and delays
- **Pet immunity**: filter for ignoring small movements (low energy) close to the sensor
- **Hold time configuration**: adjustable presence hold time via GUI and MQTT (Number entity)
- **RSSI monitoring**: configurable thresholds for weak signal and sudden signal drops (potential jamming detection)
- **Notification test**: button in GUI to test Telegram notifications with detailed status report
- **Responsive GUI**: improved mobile experience with touch-friendly inputs and collapsible sections
- MQTT: added `current_zone` sensor and `hold_time` number entity discovery

### Changed
- Enhanced SecurityMonitor logic for handling zones and notification priorities
- Web interface: added History tab, Zones tab improvements, WiFi Security settings

## [v2.6.0-AUDIT] - 2026-01-30
### Added
- **LD2412Service**: full implementation of LD2412 radar protocol (UART)
- **SecurityMonitor**: basic security features (anti-mask, loitering, tamper)
- **NotificationService**: multi-channel notification support (Telegram, Discord, webhook)
- **TelegramService**: interactive bot for control and alerting
- **MQTTService**: Home Assistant auto-discovery
- **Web GUI**: initial PROGMEM web interface with tabs and SSE telemetry
- Main: service orchestration, WiFi failover, FreeRTOS tasks

### Fixed
- Addressed various TODOs identified during the initial audit
