# Changelog

## [v3.12.0] - 2026-05-03
### Added
- **i18n**: CZ/EN language toggle in web UI — localStorage persistence, EN default, 105 translated strings
### Fixed
- **NVS persistence**: `/api/mqtt/config` now saves via `ConfigManager::save()` ensuring correct handle on reboot
- **Routing**: `/api/config/export` and `/api/config/import` were shadowed by `/api/config` (ESPAsyncWebServer prefix match); specific routes now registered first

## [v3.0.6] - 2026-02-06
### Added
- **Critical Memory Optimization**: Uvolněno ~10KB heapu pro stabilizaci AsyncWebServeru.
- **Task Stack Refactor**: Snížen stack size pro `conn_check` a `webhook_task` z 6KB na 4KB.
- **Log Buffers Reduction**: Ring buffery pro `systemLog` a `eventLog` zredukovány z 50 na 20 záznamů.
- **MQTT Topic Optimization**: Statické buffery pro MQTT témata zmenšeny z 80B na 48B.
- **Webhook Optimization**: Payload buffer pro webhooky zmenšen z 512B na 256B.
- **Workspace Cleanup**: Archivace redundantních souborů a historických záloh do `cleanup_archive/`.

## [v3.0.3] - 2026-02-05
### Fixed (Code Review)
- **WiFi Failover**: Implementována chybějící failover logika pro přepnutí na záložní síť.
- **LITE_BUILD**: Opravena kompilační chyba - server.begin() přesunuto do správného guardu.
- **Race Condition**: POST body handlery (zones, import) nyní používají per-request buffer místo static.
- **Semaphore Bypass**: setTamperDetected() už nemodifikuje stav bez mutex ochrany.
- **Ring Buffer Thread Safety**: Přidán portMUX pro ESP32 multi-core bezpečnost.
- **Blocking Delays**: delay() nahrazeny za vTaskDelay()/yield() v recovery funkcích.
- **SSE Reconnect**: GUI automaticky obnovuje spojení při výpadku.
- **Gate Sensitivity**: Nový endpoint `/api/radar/gate` + opravená GUI funkce.

### Added
- **Light Function/Threshold**: Konfigurace reakce OUT pinu na světlo (noční režim).
- **Query Resolution**: Čtení aktuálního rozlišení z radaru.
- **Presence Timeout**: Konfigurace unmanned duration v GUI.
- **Radar BT Management**: Ovládání Bluetooth modulu na radaru.
- **SSE Status Ikona**: Vizuální indikátor stavu realtime spojení v GUI.

### Changed
- Vyčištěn duplicitní kód v main.cpp.
- Certificate check už nerekonfiguruje NTP.
- Static heartbeat timer správně inicializován.

## [v3.0.2-STABLE] - 2026-02-05
### Fixed
- **Memory Optimization**: Odstraněn HTTP Auto-update, který způsoboval boot-loopy kvůli nedostatku RAM.
- **Stabilizace Bluetooth**: Bluetooth ponecháno jako nouzový režim, ale s uvolněnou pamětí po neúspěšných pokusech o update.

## [v3.0.1] - 2026-02-05
### Added
- **Bluetooth (NimBLE) Emergency Service**: Záchranný přístup přes BLE pro konfiguraci WiFi při výpadku sítě.
- **Safety Mode**: Bluetooth se aktivuje automaticky při ztrátě WiFi (>2 min) nebo ručně přes `/api/bluetooth/start`, aby se šetřila RAM.
- **Vylepšená stabilita MQTT**: Přidáno zpoždění 100ms mezi discovery zprávami pro ochranu síťového stacku.

## [v2.9.0] - 2026-02-05
### Changed
- **Konečná refaktorizace main.cpp**: Všechny zbývající HTTP routy (alarmy, logy, exporty) přesunuty do modulu `WebRoutes`.
- **Zjednodušení main.cpp**: Soubor zkrácen na ~850 řádků, čistší inicializace webových služeb.
- **Oprava perzistence**: `zonesJson` je nyní korektně předáváno do `WebRoutes` modulu přes Dependencies.

## [v2.8.9] - 2026-02-05
### Fixed
- **WebRoutes crash fix**: Lambda capture `[&deps]` způsoboval Guru Meditation Error (LoadProhibited) - reference na lokální proměnnou po ukončení setup(). Opraveno použitím statické kopie `Dependencies` struktury.

## [v2.8.8] - 2026-02-05
### Fixed
- **WebRoutes checkAuth**: Přidána chybějící deklarace `checkAuth()` do `WebRoutes.h`, opraveny všechny volání na `WebRoutes::checkAuth()` v main.cpp.
- **LD2412 Resolution modes**: Opravena podpora všech 3 HW režimů resolution:
  - Mode 0: 0.75m/gate (range 0-9m)
  - Mode 1: 0.50m/gate (range 0-6.5m)
  - Mode 2: 0.20m/gate (range 0-2.6m)
- Přidán `ResolutionMode` enum do knihovny LD2412.

### Changed
- **Refactoring dokončen**: WebRoutes a ConfigManager plně integrovány, main.cpp zredukován z ~1600 na ~1080 řádků.
- API endpoint `/api/config` nyní akceptuje resolution hodnoty 0.20, 0.50, 0.75.

### Added
- `include/constants.h` - centralizované časové konstanty a thresholdy.
- `include/WebRoutes.h` + `src/WebRoutes.cpp` - HTTP route handlery.
- `include/ConfigManager.h` + `src/ConfigManager.cpp` - správa konfigurace.
- `HANDOVER.md`, `MULTI_AGENT_REPORT.md` - dokumentace refaktoringu.

## [v2.8.7] - 2026-02-02
### Production Fixes
- **Serial za DEBUG flag**: Nový `include/debug.h` s makrem `DBG()`. ~80 volání `Serial.print*` nahrazeno za `DBG()` v services. Kritické logy (boot banner, WiFi IP, fatální chyby) ponechány vždy. `SERIAL_DEBUG=1` pro lab, `=0` pro production.
- **Deprecated `beginResponse_P`**: Nahrazeno za `beginResponse()` v hlavní route.
- **Blocking delay opravy**: Serial init 1s→500ms, portal fail 3s→1s, LITE reboot 10s→2s.
- **Heap guard**: MQTT publish a SSE telemetrie se přeskočí při `ESP.getFreeHeap() < 20000`.
- **GUI default heslo warning**: Banner text upraven na "Výchozí heslo admin/admin — změňte v sekci Síť & Cloud".

## [v2.8.6] - 2026-02-02
### Fixed — Telegram polling (revert na funkční zálohu)

**Root cause:** Všechny předchozí "opravy" (deferred `begin()`, blocking `sendMessage`, `reset()` po každém odeslání) rozbily fungující polling. Záloha v `full_lab_backup_20260202.tar.gz` ukazuje, že původní kód fungoval BEZ `_bot->begin()`, s non-blocking `sendMessage` a `_connected=true` ihned v `begin()`.

**Oprava:** Revert TelegramService.cpp na logiku ze zálohy + ponechány 2 nové opravy:
- `PRId64` pro konverzi group chat ID (záporná čísla na ESP32)
- Strip `@BotName` suffix z příkazů ve skupině

## [v2.8.5] - 2026-02-02
### Fixed — Telegram příkazy ve skupině (neúspěšné pokusy, nahrazeno v2.8.6)
- Pokusy o opravu pollingu (deferred `begin()`, blocking send, `reset()`) — vše rozbilo polling
- Přidány `/arm`, `/disarm`, `/arm_now` příkazy + `_secMon` reference
- Chat ID konverze přes `PRId64` pro záporná group ID
- Strip `@BotName` suffix z příkazů ve skupině

## [v2.8.2] - 2026-02-02
### Added
- **GUI změna hesla**: Formulář pro změnu přístupových údajů v tabu Síť & Cloud (sekce Přístupové údaje). Validace shody hesel, min. 4 znaky.

## [v2.8.1] - 2026-02-02
### Changed
- **GUI lokalizace**: Sjednocení všech textů do češtiny — alarm stavy (STŘEŽENO/NESTŘEŽENO/POPLACH), tlačítka, popisky, placeholdery. Technické zkratky (MQTT, RSSI, FPS) ponechány.

## [v2.8.0] - 2026-02-02
### Added
- **Armed/Disarmed mód** (TASK-9): Kompletní alarm systém s entry/exit delay (default 30s).
  - Stavy: DISARMED → ARMING (exit delay) → ARMED → PENDING (entry delay) → TRIGGERED
  - Telegram příkazy: `/arm`, `/disarm`, `/arm_now`, stav v `/status`
  - REST API: `POST /api/alarm/arm`, `POST /api/alarm/disarm`, `GET /api/alarm/status`, `POST /api/alarm/config`
  - MQTT `alarm_control_panel` discovery (HA integrace), subscribe `alarm/set`, publish `alarm/state`
  - GUI: ARM/DISARM tlačítko na hlavní kartě, entry/exit delay konfigurace v tab Bezpečnost
  - SSE telemetrie: `armed` a `alarm_state` pole
  - NVS persistence: `sec_armed`, `sec_entry_dl`, `sec_exit_dl`, `sec_dis_rem`
- **Připomínka "Stále DISARMED"** (TASK-10): Při detekci přítomnosti v DISARMED stavu se každých 30 min pošle upozornění.

### Fixed
- **Odstraněn duplicitní Telegram alert** (TASK-11): Smazán přímý `telegramBot.sendAlert()` v main.cpp — SecurityMonitor již posílá alerty přes NotificationService.
- **Serial spam** (pre-existing): `checkRSSIAnomaly`/`checkTamperState`/`checkRadarHealth` + `processRadarData` se volaly v každém loop cyklu (~50 000x/s). Vše rate-limitováno na 1s.
- **CPU tight-loop**: Přidán `delay(10)` na konec `loop()` — snížení CPU zátěže a serial output z 500 KB/s na <10 B/s.
- **GUI PROGMEM crash**: `request->send()` s 34KB PROGMEM stringem vracelo prázdný response (content-length: 0). Opraveno na `beginResponse_P()` — streamuje z flash bez RAM kopie. Min heap zlepšen z 73KB na 104KB.
- **EventLog flush**: Vyčištěna rate-limit logika.

### Changed
- Zone a loitering alerty se posílají pouze v ARMED stavu (tamper vždy).
- LED strobe podmíněn armed stavem.
- Offline alarm memory podmíněn armed stavem.

## [v2.7.1-REVIEW] - 2026-02-01
### Fixed
- **BUG: processRadarData dostával mm místo cm** — loitering (<2m), zónové alerty a event log měly 10x špatné jednotky vzdálenosti. Odstraněn `* 10` z volání v main.cpp.
- **BUG: EventLog double konverze** — `_lastDistance / 10` v triggerAlert byl navíc, protože data už jsou v cm.
- **WARN: Chybějící `#include <vector>`** v SecurityMonitor.h pro `std::vector<AlertZone>`.
- **MINOR: Duplicitní číslo sekce** "3." v processRadarData — opraveno na 4. a 5.

## [v2.7.0-RC1] - 2026-02-01
### Added
- **Event Log System**: Persistent logging of security events (Presence, Tamper, WiFi, System) to SPIFFS. History viewable in Web GUI.
- **Zone Management**: Configurable alert zones (LOG, INFO, WARN, ALARM) with specific distance ranges and delays.
- **Pet Immunity**: Filter for ignoring small movements (low energy) close to the sensor.
- **Hold Time Configuration**: Adjustable presence hold time via GUI and MQTT (Number entity).
- **RSSI Monitoring**: Configurable thresholds for weak signal and sudden signal drops (potential jamming detection).
- **Notification Test**: Button in GUI to test Telegram notifications with detailed status report.
- **Responsive GUI**: Improved mobile experience with touch-friendly inputs and collapsible sections.
- **MQTT Updates**: Added `current_zone` sensor and `hold_time` number entity discovery.

### Changed
- **Security Monitor**: Enhanced logic for handling zones and notification priorities.
- **Web Interface**: Added "History" tab, "Zones" tab improvements, and "WiFi Security" settings.
- **Firmware Version**: Bumped to v2.7.0-RC1.

## [v2.6.0-AUDIT] - 2026-01-30
### Added
- **LD2412Service**: Full implementation of LD2412 radar protocol (UART).
- **SecurityMonitor**: Basic security features (Anti-mask, Loitering, Tamper).
- **NotificationService**: Multi-channel notification support (Telegram, Discord, Webhook).
- **TelegramService**: Interactive bot for control and alerting.
- **MQTTService**: Home Assistant auto-discovery.
- **Web GUI**: Initial PROGMEM web interface with tabs and SSE telemetry.
- **Main**: Orchestration of services, WiFi failover, FreeRTOS tasks.

### Fixed
- Addressed various TODOs identified during the initial audit.
