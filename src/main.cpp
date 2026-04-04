#include <Arduino.h>
#include <WiFi.h>
#include <lwip/dns.h>
#ifndef LITE_BUILD
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPAsyncWiFiManager.h>
#include <DNSServer.h>
#endif
#include <ArduinoJson.h>
#ifndef LITE_BUILD
#include <ArduinoOTA.h>
#endif
#include <Preferences.h>
#include <esp_task_wdt.h>
#ifndef LITE_BUILD
#include <HTTPClient.h>
#endif

#include "services/LD2412Service.h"
#include "services/MQTTService.h"
#include "services/SecurityMonitor.h"
#include "services/NotificationService.h"
#include "services/TelegramService.h"
#include "services/LogService.h"
#include "services/EventLog.h"
#include "services/MQTTOfflineBuffer.h"
#ifndef NO_BLUETOOTH
#include "services/BluetoothService.h"
#endif
#include "debug.h"
#include "secrets.h"
#include "constants.h"
#include "ConfigManager.h"
#include "WebRoutes.h"
#include <esp_ota_ops.h>
#include <ESPmDNS.h>
#include <time.h>

// -------------------------------------------------------------------------
// Defines
// -------------------------------------------------------------------------
#ifndef FW_VERSION
#include <Update.h>

#define FW_VERSION "v3.11.0"

// --- BACKUP NETWORK CONFIGURATION (AP/Failover) ---
#endif
#define WDT_TIMEOUT_SECONDS 60

// NTP Config
const char* ntpServer = "pool.ntp.org";

// Radar UART pins - defined in platformio.ini per board type
#ifndef RADAR_RX_PIN
#error "RADAR_RX_PIN not defined! Use correct environment: esp32_board1 or esp32_board2"
#endif
#ifndef RADAR_TX_PIN
#error "RADAR_TX_PIN not defined! Use correct environment: esp32_board1 or esp32_board2"
#endif

#define LED_PIN 25

// Radar OUT pin (hardware detection output) - optional
#ifndef RADAR_OUT_PIN
#define RADAR_OUT_PIN -1  // -1 = not connected
#endif

// Siren/strobe GPIO output - optional
#ifndef SIREN_PIN
#define SIREN_PIN SIREN_PIN_DEFAULT  // -1 = disabled (from constants.h)
#endif

// -------------------------------------------------------------------------
// Objects
// -------------------------------------------------------------------------
LD2412Service radar(RADAR_RX_PIN, RADAR_TX_PIN);
#ifndef LITE_BUILD
AsyncWebServer server(80);
AsyncEventSource events("/events");
DNSServer dns;
#endif
Preferences preferences;
MQTTService mqttService;
SecurityMonitor securityMonitor;
NotificationService notificationService;
TelegramService telegramBot;
LogService systemLog(20); // Keep last 20 events
EventLog eventLog(100);   // Keep last 100 security events
MQTTOfflineBuffer mqttBuffer;
#ifndef NO_BLUETOOTH
BluetoothService btService;
#endif
TaskHandle_t radarTaskHandle = nullptr;
String g_prevRestartCause = "none"; // Set in setup(), read in loop() for MQTT publish

// -------------------------------------------------------------------------
// Supervision Heartbeat — peer monitoring
// -------------------------------------------------------------------------
struct SupervisionPeer {
    char id[32];
    unsigned long lastSeen;  // millis()
    bool alerted;            // tamper alert already sent for this peer
};
static constexpr uint8_t MAX_PEERS = 8;
static SupervisionPeer peers[MAX_PEERS];
static uint8_t peerCount = 0;
static unsigned long lastSupervisionPublish = 0;
static constexpr unsigned long SUPERVISION_INTERVAL_MS = 60000;   // Publish every 60s
static constexpr unsigned long SUPERVISION_TIMEOUT_MS  = 180000;  // Alert after 3x interval (3 min)
static const char* g_myDeviceId = nullptr; // Set in setup() after configManager init

// -------------------------------------------------------------------------
// Multi-sensor mesh — cross-node alarm verification
// -------------------------------------------------------------------------
static bool meshVerifyPending = false;        // We sent a verify request, awaiting confirms
static unsigned long meshVerifyRequestTime = 0;
static uint8_t meshConfirmCount = 0;
static constexpr unsigned long MESH_VERIFY_TIMEOUT_MS = 5000; // 5s window for peer confirms

void supervisionPeerSeen(const char* peerId) {
    if (g_myDeviceId && strcmp(peerId, g_myDeviceId) == 0) return;

    unsigned long now = millis();
    for (uint8_t i = 0; i < peerCount; i++) {
        if (strcmp(peers[i].id, peerId) == 0) {
            if (peers[i].alerted) {
                DBG("SUPV", "Peer '%s' back online", peerId);
                peers[i].alerted = false;
            }
            peers[i].lastSeen = now;
            return;
        }
    }
    if (peerCount < MAX_PEERS) {
        strncpy(peers[peerCount].id, peerId, sizeof(peers[peerCount].id) - 1);
        peers[peerCount].id[sizeof(peers[peerCount].id) - 1] = '\0';
        peers[peerCount].lastSeen = now;
        peers[peerCount].alerted = false;
        peerCount++;
        DBG("SUPV", "New peer discovered: '%s' (total: %d)", peerId, peerCount);
    }
}

void supervisionCheck() {
    unsigned long now = millis();
    for (uint8_t i = 0; i < peerCount; i++) {
        if (!peers[i].alerted && now - peers[i].lastSeen > SUPERVISION_TIMEOUT_MS) {
            peers[i].alerted = true;
            DBG("SUPV", "PEER OFFLINE: '%s' (no heartbeat for %lus)", peers[i].id, (now - peers[i].lastSeen) / 1000);
            String msg = "🔴 SUPERVISION: Node '" + String(peers[i].id) + "' offline!";
            String details = "No heartbeat for " + String((now - peers[i].lastSeen) / 1000) + "s. Possible tamper or failure.";
            notificationService.sendAlert(NotificationType::TAMPER_ALERT, msg, details);
            if (mqttService.connected()) {
                mqttService.publish(mqttService.getTopics().tamper, "peer_offline", false);
            }
        }
    }
}

// -------------------------------------------------------------------------
// Config
// -------------------------------------------------------------------------
ConfigManager configManager;

// Zones Persistence (TASK-011) - Kept here for now as logical unit
String zonesJson = "[]";

// Function prototypes for zones persistence (TASK-011)
void saveZonesToNVS();
void loadZonesFromNVS();

bool shouldSaveConfig = false;
volatile bool shouldReboot = false;
bool bootValidated = false; // For OTA Rollback
volatile bool wifiReconnectRequested = false; // Flag from connectivity task → loop()

// Thread-safe pending zones update (async web handler → main loop)
static String pendingZonesJson = "";
static volatile bool pendingZonesUpdate = false;
SemaphoreHandle_t zonesMutex = NULL;

unsigned long lastLedBlink = 0;
unsigned long lastTele = 0;
unsigned long lastWiFiCheck = 0;
unsigned long bootTime = 0;

// -------------------------------------------------------------------------
// Publish-on-Change Tracking (~80 bytes RAM)
// -------------------------------------------------------------------------
struct LastPublished {
    // Critical (immediate on change)
    char presence_state[16] = "";
    bool tamper = false, anti_masking = false, loitering = false;
    char alarm_state[16] = "";
    char motion_type[8] = "";

    // Primary sensor (2s interval + deadband)
    uint16_t distance_cm = 0;
    uint8_t energy_mov = 0, energy_stat = 0;
    char direction[16] = "";

    // Diagnostics (30s interval + deadband)
    int8_t rssi = 0;
    uint32_t uptime_s = 0;
    uint8_t health_score = 0;
    float frame_rate = 0.0f;
    uint32_t error_count = 0;
    char uart_state[24] = "";
    uint32_t free_heap_kb = 0, max_alloc_kb = 0;
    bool eng_mode = false;

    // Engineering gates (10s + deadband)
    uint8_t gate_mov[14] = {0}, gate_stat[14] = {0};
    uint8_t light_level = 0;

    // Tiered timestamps
    unsigned long lastDiagPublish = 0;
    unsigned long lastEngPublish = 0;
};
static LastPublished lastPub;

// Deadband comparison helpers (inline, zero overhead)
static inline bool changedU16(uint16_t c, uint16_t l, uint16_t d) { return (c > l+d) || (l > c+d); }
static inline bool changedU8 (uint8_t  c, uint8_t  l, uint8_t  d) { return (c > l+d) || (l > c+d); }
static inline bool changedI8 (int8_t   c, int8_t   l, int8_t   d) { int diff=(int)c-(int)l; return diff>d||diff<-d; }
static inline bool changedU32(uint32_t c, uint32_t l, uint32_t d) { return (c > l+d) || (l > c+d); }
static inline bool changedF  (float    c, float    l, float    d) { float diff=c-l; return diff>d||diff<-d; }

#include "web_interface.h"
#include "known_devices.h"

// -------------------------------------------------------------------------
// Authentication helper (moved to WebRoutes.cpp)
// -------------------------------------------------------------------------

// -------------------------------------------------------------------------
// HTML (Premium GUI - LD2412 Edition)
// -------------------------------------------------------------------------
// Moved to include/web_interface.h


// -------------------------------------------------------------------------
// Callbacks
// -------------------------------------------------------------------------
// -------------------------------------------------------------------------
// Safe Restart — centralized restart with NVS cause logging
// -------------------------------------------------------------------------
void safeRestart(const char* reason) {
    preferences.putString("restart_cause", reason);
    preferences.putULong("last_uptime", millis() / 1000);
    preferences.putULong("last_heap", ESP.getFreeHeap());
    preferences.putULong("last_maxalloc", ESP.getMaxAllocHeap());
    preferences.putULong("last_minheap", ESP.getMinFreeHeap());
    DBG("SYSTEM", ">>> RESTART: %s (uptime %lus, heap %u/%u/%u)",
        reason, millis() / 1000, ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
    delay(500);
    ESP.restart();
}

void saveConfigCallback() {
    shouldSaveConfig = true;
}

void setupWiFi() {
    // 1. LAB MODE: Try hardcoded credentials
    DBG("WiFi", "Trying hardcoded lab credentials...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID_DEFAULT, WIFI_PASS_DEFAULT);

    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 5000) { // Reduced to 5s for lab
        delay(250);
        DBG("WiFi", ".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected to %s, IP: %s\n", WIFI_SSID_DEFAULT, WiFi.localIP().toString().c_str());
        return;
    }

    // 2. PRIMARY: Try last saved credentials (SDK native)
    DBG("WiFi", "Trying saved Primary credentials...");
    WiFi.disconnect();
    WiFi.begin(); // Uses SDK saved config

    startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
        delay(250);
        DBG("WiFi", ".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected to Saved Primary, IP: %s\n", WiFi.localIP().toString().c_str());
        return;
    }

    // 3. BACKUP: Try backup credentials from Preferences
    if (strlen(configManager.getConfig().backup_ssid) > 0) {
        DBG("WiFi", "Trying Backup credentials: %s...", configManager.getConfig().backup_ssid);
        WiFi.disconnect();
        WiFi.begin(configManager.getConfig().backup_ssid, configManager.getConfig().backup_pass);

        startAttempt = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) { // Longer timeout for backup
            delay(250);
            DBG("WiFi", ".");
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\n[WiFi] Connected to Backup (%s), IP: %s\n", configManager.getConfig().backup_ssid, WiFi.localIP().toString().c_str());
            return;
        }
    }

    DBG("WiFi", "All methods failed, starting AP Portal...");

#ifndef LITE_BUILD
    // 4. FALLBACK: Start AP Portal
    AsyncWiFiManager wm(&server, &dns);
    
    // Config Parameters
    AsyncWiFiManagerParameter custom_mqtt_server("server", "MQTT Server", configManager.getConfig().mqtt_server, 60);
    AsyncWiFiManagerParameter custom_mqtt_port("port", "MQTT Port", configManager.getConfig().mqtt_port, 6);
    AsyncWiFiManagerParameter custom_mqtt_user("user", "MQTT User", configManager.getConfig().mqtt_user, 40);
    AsyncWiFiManagerParameter custom_mqtt_pass("pass", "MQTT Password", configManager.getConfig().mqtt_pass, 40);
    
    // Backup WiFi Parameters
    AsyncWiFiManagerParameter custom_backup_ssid("bk_ssid", "Backup SSID", configManager.getConfig().backup_ssid, 32);
    AsyncWiFiManagerParameter custom_backup_pass("bk_pass", "Backup Password", configManager.getConfig().backup_pass, 64);

    wm.addParameter(&custom_mqtt_server);
    wm.addParameter(&custom_mqtt_port);
    wm.addParameter(&custom_mqtt_user);
    wm.addParameter(&custom_mqtt_pass);
    wm.addParameter(&custom_backup_ssid);
    wm.addParameter(&custom_backup_pass);
    
    wm.setSaveConfigCallback(saveConfigCallback);

    String ssid = "LD2412_Setup_" + String((uint32_t)ESP.getEfuseMac(), HEX);
    
    // We use startConfigPortal instead of autoConnect to force AP mode since we handled connection attempts manually
    if (!wm.startConfigPortal(ssid.c_str(), "ld2412setup")) {
        Serial.println("Failed to connect and hit timeout");
        safeRestart("wifi_portal_timeout");
    }

    // Retrieve Values and save to prefs via manual putString (or update ConfigManager)
    // For now we update the struct and rely on ConfigManager to handle persistence if we added save()
    // But ConfigManager::save() is empty, so we should use preferences directly or update struct and save?
    // Let's stick to updating the struct for runtime and verify if we need explicit save logic.
    // Actually the origin code used global vars and didn't save them here? 
    // Wait, wm.setSaveConfigCallback sets shouldSaveConfig=true. 
    // And loop() handles saving? Yes.
    
    strncpy(configManager.getConfig().mqtt_server, custom_mqtt_server.getValue(), 59);
    configManager.getConfig().mqtt_server[59] = '\0';
    strncpy(configManager.getConfig().mqtt_port, custom_mqtt_port.getValue(), 5);
    configManager.getConfig().mqtt_port[5] = '\0';
    strncpy(configManager.getConfig().mqtt_user, custom_mqtt_user.getValue(), 39);
    configManager.getConfig().mqtt_user[39] = '\0';
    strncpy(configManager.getConfig().mqtt_pass, custom_mqtt_pass.getValue(), 39);
    configManager.getConfig().mqtt_pass[39] = '\0';
    strncpy(configManager.getConfig().backup_ssid, custom_backup_ssid.getValue(), sizeof(configManager.getConfig().backup_ssid) - 1);
    configManager.getConfig().backup_ssid[sizeof(configManager.getConfig().backup_ssid) - 1] = '\0';
    strncpy(configManager.getConfig().backup_pass, custom_backup_pass.getValue(), sizeof(configManager.getConfig().backup_pass) - 1);
    configManager.getConfig().backup_pass[sizeof(configManager.getConfig().backup_pass) - 1] = '\0';
    
    // Save is handled in main loop via shouldSaveConfig flag
#else
    Serial.println("[WiFi] LITE build: no AP portal. Rebooting...");
    delay(2000);
    ESP.restart();
#endif
}

// -------------------------------------------------------------------------
// Zones Persistence (TASK-011)
// -------------------------------------------------------------------------
void updateZonesFromJSON() {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, zonesJson);
    if (error) {
        DBG("CONFIG", "Failed to parse zones JSON");
        return;
    }

    std::vector<AlertZone> zones;
    JsonArray arr = doc.as<JsonArray>();
    for (JsonObject obj : arr) {
        AlertZone z;
        String name = obj["name"] | "Zone";
        strncpy(z.name, name.c_str(), sizeof(z.name)-1);
        z.name[sizeof(z.name)-1] = '\0';
        
        z.min_cm = obj["min"] | 0;
        z.max_cm = obj["max"] | 0;
        z.alert_level = obj["level"] | 0; // Default 0 (LOG)
        z.delay_ms = obj["delay"] | 0;    // Default 0ms
        z.enabled = obj["enabled"] | true; // Default true if missing
        z.alarm_behavior = obj["alarm_behavior"] | 0; // 0=entry_delay, 1=immediate, 2=ignore

        String prevZone = obj["prev_zone"] | "";
        strncpy(z.valid_prev_zone, prevZone.c_str(), sizeof(z.valid_prev_zone)-1);
        z.valid_prev_zone[sizeof(z.valid_prev_zone)-1] = '\0';

        zones.push_back(z);
    }
    securityMonitor.setZones(zones);
    DBG("CONFIG", "Updated %d zones", zones.size());
}

void saveZonesToNVS() {
    if (zonesJson.length() < 1000) { // Sanity check
        preferences.putString("zones_json", zonesJson);
        DBG("CONFIG", "Zones saved to NVS");
        updateZonesFromJSON();
    }
}

void loadZonesFromNVS() {
    if (preferences.isKey("zones_json")) {
        zonesJson = preferences.getString("zones_json", "[]");
        DBG("CONFIG", "Zones loaded from NVS: %d bytes", zonesJson.length());
        updateZonesFromJSON();
    }
}

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------
void radarTask(void* param) {
    const TickType_t delayTicks = pdMS_TO_TICKS(2);
    for (;;) {
        radar.update();
        vTaskDelay(delayTicks);
    }
}

// Connectivity watchdog — monitors WiFi + MQTT health without external HTTP.
// If WiFi is connected but MQTT has been disconnected for >5 min, attempt
// WiFi.reconnect() (NOT disconnect) to refresh the TCP stack.
void connectivityTask(void* param) {
    const TickType_t delayTicks = pdMS_TO_TICKS(INTERVAL_CONNECTIVITY_MS);
    int mqttFailCount = 0;
    const int MQTT_FAIL_THRESHOLD = 5; // 5 × 60s = 5 min of MQTT down

    for (;;) {
        vTaskDelay(delayTicks);

        if (WiFi.status() != WL_CONNECTED) {
            mqttFailCount = 0; // WiFi is down — failover logic in loop() handles this
            continue;
        }

        // WiFi is connected — check if MQTT is working
        if (mqttService.connected()) {
            mqttFailCount = 0;
        } else {
            mqttFailCount++;
            DBG("CONN", "WiFi OK but MQTT down (%d/%d)", mqttFailCount, MQTT_FAIL_THRESHOLD);
            if (mqttFailCount >= MQTT_FAIL_THRESHOLD) {
                DBG("CONN", "MQTT down too long — requesting WiFi reconnect from loop");
                wifiReconnectRequested = true; // Safe: processed by loop() on core 1
                mqttFailCount = 0;
            }
        }
    }
}

void setup() {
    Serial.begin(115200); // Standard speed
    
    zonesMutex = xSemaphoreCreateMutex();
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW); // Start with LED OFF

    // --- Factory Reset: hold GPIO 0 (BOOT button) for 5 seconds at boot ---
    // NOTE: Some USB-serial bridges (CP210x) hold GPIO 0 LOW via DTR after reset.
    // Wait 2s for USB-serial lines to stabilize before checking button state.
    pinMode(0, INPUT_PULLUP);
    delay(2000);
    if (digitalRead(0) == LOW) {
        unsigned long pressStart = millis();
        Serial.println("[SYSTEM] GPIO 0 pressed — hold 5s for factory reset...");
        digitalWrite(LED_PIN, HIGH); // LED ON = visual feedback
        while (digitalRead(0) == LOW && (millis() - pressStart) < 5000) {
            delay(100);
        }
        if (millis() - pressStart >= 5000) {
            Serial.println("[SYSTEM] FACTORY RESET! Clearing NVS...");
            Preferences resetPrefs;
            resetPrefs.begin("radar_config", false);
            resetPrefs.clear();
            resetPrefs.end();
            WiFi.disconnect(true, true); // Clear WiFiManager saved networks
            Serial.println("[SYSTEM] NVS cleared. Restarting...");
            for (int i = 0; i < 6; i++) { // Blink LED 3x
                digitalWrite(LED_PIN, i % 2); delay(200);
            }
            ESP.restart();
        }
        digitalWrite(LED_PIN, LOW);
        Serial.println("[SYSTEM] GPIO 0 released — normal boot.");
    }

    // Initialize radar OUT pin if connected
    #if RADAR_OUT_PIN >= 0
    pinMode(RADAR_OUT_PIN, INPUT);
    DBG("INIT", "Radar OUT pin: GPIO %d", RADAR_OUT_PIN);
    #endif

    delay(500);
    Serial.println("\n\n\n");
    Serial.println("=============================================");
    Serial.println("   LD2412 SECURITY NODE - BOOT SEQUENCE");
    Serial.println("=============================================");
    Serial.print(">> FW Version: "); Serial.println(FW_VERSION);
    // Init ConfigManager
    configManager.begin();
    
    // Explicit boot diagnostics
    Serial.println("---------------------------------------------");
    Serial.printf("[SYSTEM] MAC: %s\n", WiFi.macAddress().c_str());
    Serial.printf("[SYSTEM] MQTT Server: %s\n", configManager.getConfig().mqtt_server);
    Serial.printf("[SYSTEM] MQTT Client ID: %s\n", configManager.getConfig().mqtt_id);
    Serial.println("---------------------------------------------");

    // Init global preferences (same namespace as ConfigManager)
    preferences.begin("radar_config", false);
    
    // Start Bluetooth Config Service ONLY if WiFi fails or manually requested (TASK-BT-SAFE)
    #ifndef NO_BLUETOOTH
    bool startBT = false;
    if (startBT) {
        btService.begin(configManager.getConfig().mqtt_id, &configManager);
        btService.setTimeout(600); // 10 minutes active
    }
    #endif
    
    Serial.print(">> MQTT Broker Target: "); Serial.println(configManager.getConfig().mqtt_server);
    Serial.println("=============================================\n");
    
    // --- Auto-Config by MAC (Multi-Device Support) ---
    String mac = WiFi.macAddress();
    mac.toLowerCase();
    DBG("SETUP", "MAC: %s", mac.c_str());
    
    bool knownDevice = false;
    for (int i = 0; i < KNOWN_DEVICE_COUNT; i++) {
        if (mac == KNOWN_DEVICES[i].mac) {
            DBG("SETUP", "Match found! Auto-configuring as: %s", KNOWN_DEVICES[i].id);
            
            // Force Identity from table
            strncpy(configManager.getConfig().mqtt_id, KNOWN_DEVICES[i].id, 39);
            configManager.getConfig().mqtt_id[39] = '\0';
            
            // Save to preferences only if changed (reduce NVS wear)
            if (preferences.getString("mqtt_id", "") != String(KNOWN_DEVICES[i].id)) {
                preferences.putString("mqtt_id", KNOWN_DEVICES[i].id);
            }
            
            // Set Hostname: NVS has priority (user may have changed via GUI), KNOWN_DEVICES is just default
            String nvsHostname = preferences.getString("hostname", "");
            if (nvsHostname.isEmpty()) {
                // First boot — use default from table
                nvsHostname = KNOWN_DEVICES[i].hostname;
                preferences.putString("hostname", nvsHostname);
            }
            strncpy(configManager.getConfig().hostname, nvsHostname.c_str(), 32);
            configManager.getConfig().hostname[32] = '\0';
            WiFi.setHostname(nvsHostname.c_str());
            
            knownDevice = true;
            break;
        }
    }
    if (!knownDevice) {
        DBG("SETUP", "Unknown device. Using stored/default config.");
    }
    // -------------------------------------------------

    // Load zones from NVS (TASK-011)
    loadZonesFromNVS();
    
    // Load Pet Immunity (TASK-XXX)
    uint8_t petImmunity = preferences.getUInt("sec_pet", 0);
    if(petImmunity > 0) {
        radar.setMinMoveEnergy(petImmunity);
        securityMonitor.setPetImmunity(petImmunity);
        DBG("CONFIG", "Pet Immunity loaded: %d", petImmunity);
    }

    // Load Hold Time (TASK-FIX)
    unsigned long holdTime = preferences.getULong("hold_time", 500);
    radar.setHoldTime(holdTime);
    DBG("CONFIG", "Hold Time loaded: %lu ms", holdTime);

    // --- Reset Reason Logging (TASK-029) ---
    esp_reset_reason_t reason = esp_reset_reason();
    String reasonStr;
    switch (reason) {
        case ESP_RST_POWERON: reasonStr = "Power-on"; break;
        case ESP_RST_EXT:     reasonStr = "External pin"; break;
        case ESP_RST_SW:      reasonStr = "Software reset"; break;
        case ESP_RST_PANIC:   reasonStr = "Exception/Panic"; break;
        case ESP_RST_INT_WDT: reasonStr = "Interrupt WDT"; break;
        case ESP_RST_TASK_WDT: reasonStr = "Task WDT"; break;
        case ESP_RST_WDT:      reasonStr = "Other WDT"; break;
        case ESP_RST_DEEPSLEEP: reasonStr = "Deep sleep"; break;
        case ESP_RST_BROWNOUT: reasonStr = "Brownout"; break;
        case ESP_RST_SDIO:     reasonStr = "SDIO reset"; break;
        default:               reasonStr = "Unknown"; break;
    }
    
    String history = preferences.getString("reset_history", "[]");
    JsonDocument historyDoc;
    deserializeJson(historyDoc, history);
    JsonArray arr = historyDoc.as<JsonArray>();
    
    // Read safeRestart cause from previous run (if any) — save to global for loop() MQTT publish
    String prevRestartCause = preferences.getString("restart_cause", "none");
    g_prevRestartCause = prevRestartCause;

    JsonObject entry = arr.add<JsonObject>();
    entry["reason"] = reasonStr;
    entry["cause"] = prevRestartCause;
    entry["uptime"] = preferences.getULong("last_uptime", 0);
    entry["ts"] = millis(); // Relative to this boot, but helps order

    // Keep last 10 entries
    while (arr.size() > 10) arr.remove(0);

    String newHistory;
    serializeJson(historyDoc, newHistory);
    preferences.putString("reset_history", newHistory);
    // Clear restart_cause so next power-on shows "none"
    preferences.putString("restart_cause", "none");
    DBG("SYSTEM", "Reset reason: %s, cause: %s", reasonStr.c_str(), prevRestartCause.c_str());
    systemLog.warn("System restart: " + reasonStr + " (" + prevRestartCause + ")");
    // ---------------------------------------

    uint8_t minGate = preferences.getUInt("radar_min", 0);
    uint8_t maxGate = preferences.getUInt("radar_max", 13);
    
    if (!radar.begin(Serial2, minGate, maxGate)) {
        Serial.println("[RADAR] Failed to init LD2412");
        systemLog.error("Radar init failed");
    } else {
        systemLog.info("Radar initialized");

        // Apply stored resolution to radar hardware (ESPHome-compatible cmd 0x01)
        float storedRes = configManager.getConfig().radar_resolution;
        if (storedRes != 0.75f) {  // Only send if non-default (avoid unnecessary restart)
            if (radar.setResolution(storedRes)) {
                DBG("RADAR", "Resolution set to %.2fm", storedRes);
            } else {
                DBG("RADAR", "Resolution command failed (%.2fm)", storedRes);
            }
        } else {
            DBG("RADAR", "Resolution: 0.75m (default, skipping command)");
        }
    }

    // Run radar update in a dedicated task to stabilize UART parsing
    xTaskCreatePinnedToCore(radarTask, "radar_task", 8192, nullptr, 2, &radarTaskHandle, 1);

    setupWiFi();
    
    DBG("NET", "IP: %s  GW: %s  SN: %s  DNS: %s",
        WiFi.localIP().toString().c_str(),
        WiFi.gatewayIP().toString().c_str(),
        WiFi.subnetMask().toString().c_str(),
        WiFi.dnsIP().toString().c_str());

    // Init mDNS (device discoverable by hostname.local)
    if (MDNS.begin(configManager.getConfig().hostname)) {
        MDNS.addService("http", "tcp", 80);
        DBG("NET", "mDNS started: %s.local", configManager.getConfig().hostname);
    }

    // DNS fallback — Google 8.8.8.8 as secondary (fix for networks with broken DHCP DNS)
    {
        ip_addr_t dns_fallback;
        IP_ADDR4(&dns_fallback, 8, 8, 8, 8);
        dns_setserver(1, &dns_fallback);
        DBG("NET", "DNS fallback set: 8.8.8.8");
    }

    // Init NTP
    configTime(configManager.getConfig().tz_offset, configManager.getConfig().dst_offset, ntpServer);
    DBG("SYSTEM", "NTP Time sync started");

    // Init SSE
#ifndef LITE_BUILD
    events.onConnect([](AsyncEventSourceClient *client){
        if(client->lastId()){
            DBG("SSE", "Client reconnected! Last message ID: %u", client->lastId());
        }
        client->send("hello!", NULL, millis(), 1000);
    });
    server.addHandler(&events);
#endif

    // WDT after WiFi - prevents timeout during WiFiManager portal
    esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
    esp_task_wdt_add(NULL);

    // --- Web Server Routes ---
#ifndef LITE_BUILD
    // Use WebRoutes module
    WebRoutes::Dependencies deps = {
        .server = &server,
        .events = &events,
        .preferences = &preferences,
        .radar = &radar,
        .mqttService = &mqttService,
        .securityMonitor = &securityMonitor,
        .telegramBot = &telegramBot,
        .notificationService = &notificationService,
        .systemLog = &systemLog,
        .eventLog = &eventLog,
        #ifndef NO_BLUETOOTH
        .bluetooth = &btService,
        #else
        .bluetooth = nullptr,
        #endif
        
        .config = &configManager.getConfig(),
        .configManager = &configManager,

        .zonesJson = &zonesJson,
        .fwVersion = FW_VERSION,
        .shouldReboot = &shouldReboot,
        
        .pendingZonesJson = &pendingZonesJson,
        .pendingZonesUpdate = &pendingZonesUpdate,
        .zonesMutex = &zonesMutex
    };
    
    WebRoutes::setup(deps);

    server.begin();
#endif

    if (shouldSaveConfig) {
        preferences.putString("mqtt_server", configManager.getConfig().mqtt_server);
        preferences.putString("mqtt_port", configManager.getConfig().mqtt_port);
        preferences.putString("mqtt_user", configManager.getConfig().mqtt_user);
        preferences.putString("mqtt_pass", configManager.getConfig().mqtt_pass);
        preferences.putString("bk_ssid", configManager.getConfig().backup_ssid);
        preferences.putString("bk_pass", configManager.getConfig().backup_pass);
        shouldSaveConfig = false;
    }

    // Init services
    g_myDeviceId = configManager.getConfig().mqtt_id; // For supervision heartbeat
    if (configManager.getConfig().mqtt_enabled) {
        mqttService.begin(&preferences, configManager.getConfig().mqtt_id, FW_VERSION);
        mqttService.setCommandCallback([](const char* topic, const char* payload) {
            const MQTTTopics& t = mqttService.getTopics();
            if (strcmp(topic, t.cmd_max_range) == 0) {
                int val = atoi(payload);
                if (val >= 1 && val <= 14) radar.setParamConfig(1, (uint8_t)val, 5);
            } else if (strcmp(topic, t.cmd_hold_time) == 0) {
                unsigned long val = strtoul(payload, nullptr, 10);
                if (val <= 65535) {
                    radar.setHoldTime(val);
                    preferences.putULong("hold_time", val);
                    mqttService.publish(t.state_hold_time, String(val).c_str(), true);
                }
            } else if (strcmp(topic, t.cmd_sensitivity) == 0) {
                // Format: "mov,stat" (all gates) or "gate,mov,stat" (single gate)
                int a, b, c;
                int n = sscanf(payload, "%d,%d,%d", &a, &b, &c);
                if (n == 2 && a >= 0 && a <= 100 && b >= 0 && b <= 100) {
                    // All gates: "mov,stat"
                    radar.setMotionSensitivity((uint8_t)a);
                    radar.setStaticSensitivity((uint8_t)b);
                } else if (n == 3 && a >= 0 && a <= 13 && b >= 0 && b <= 100 && c >= 0 && c <= 100) {
                    // Single gate: "gate,mov,stat"
                    const uint8_t* currentMov = radar.getMotionSensitivityArray();
                    const uint8_t* currentStat = radar.getStaticSensitivityArray();
                    uint8_t movArr[14], statArr[14];
                    
                    memcpy(movArr, currentMov, 14);
                    memcpy(statArr, currentStat, 14);
                    
                    movArr[a] = b; statArr[a] = c;
                    radar.setMotionSensitivity(movArr);
                    radar.setStaticSensitivity(statArr);
                }
            } else if (strcmp(topic, t.cmd_pet_immunity) == 0) {
                int val = atoi(payload);
                if (val >= 0 && val <= 100) {
                    radar.setMinMoveEnergy((uint8_t)val);
                    securityMonitor.setPetImmunity((uint8_t)val);
                    preferences.putUInt("sec_pet", (uint8_t)val);
                }
            } else if (strcmp(topic, t.cmd_dyn_bg) == 0) {
                radar.startCalibration();
            } else if (strcmp(topic, t.alarm_set) == 0) {
                String cmd = String(payload);
                if (cmd == "ARM_AWAY") securityMonitor.setArmed(true, false);
                else if (cmd == "DISARM") securityMonitor.setArmed(false);
            } else if (strstr(topic, "/supervision/alive") != nullptr) {
                // Extract peer ID from topic: security/<id>/supervision/alive
                const char* start = topic + 9; // skip "security/"
                const char* end = strstr(start, "/supervision");
                if (end && end - start < 32) {
                    char peerId[32];
                    size_t len = end - start;
                    memcpy(peerId, start, len);
                    peerId[len] = '\0';
                    supervisionPeerSeen(peerId);
                }
            } else if (strstr(topic, "/mesh/verify_request") != nullptr) {
                // Another node is asking for alarm verification
                // If we also see presence, confirm it
                auto d = radar.getData();
                if (d.distance_cm > 0 && (d.moving_energy > 0 || d.static_energy > 0)) {
                    // We see something too — confirm
                    char confirmTopic[96];
                    snprintf(confirmTopic, sizeof(confirmTopic), "security/%s/mesh/verify_confirm", g_myDeviceId);
                    char confirmPayload[64];
                    snprintf(confirmPayload, sizeof(confirmPayload), "{\"dist\":%d,\"mov\":%d,\"stat\":%d}",
                        d.distance_cm, d.moving_energy, d.static_energy);
                    mqttService.publish(confirmTopic, confirmPayload, false);
                    DBG("MESH", "Confirmed verify request (dist=%d)", d.distance_cm);
                }
            } else if (strstr(topic, "/mesh/verify_confirm") != nullptr) {
                // A peer confirmed our alarm — count it
                if (meshVerifyPending) {
                    meshConfirmCount++;
                    DBG("MESH", "Received verify confirm #%d", meshConfirmCount);
                }
            }
            // Removed: rain_mode, resolution, out_pin (not in native library)
        });
    } else {
        DBG("SYSTEM", "MQTT Disabled (Stand-alone Mode)");
    }
    
    notificationService.begin(&preferences, configManager.getConfig().mqtt_id);
    telegramBot.begin(&preferences);
    telegramBot.setRadarService(&radar);
    notificationService.setTelegramService(&telegramBot);

    eventLog.begin();
    mqttBuffer.begin();
    securityMonitor.begin(&notificationService, &mqttService, &telegramBot, &eventLog, &preferences, configManager.getConfig().mqtt_id);
    telegramBot.setSecurityMonitor(&securityMonitor);
    telegramBot.setRebootFlag(&shouldReboot);

    // Load security configuration from NVS
    if (preferences.isKey("sec_antimask"))
        securityMonitor.setAntiMaskTime(preferences.getULong("sec_antimask", DEFAULT_ANTI_MASK_MS));
    
    // Default Anti-Masking to FALSE (Disabled for Silent Mode)
    securityMonitor.setAntiMaskEnabled(preferences.getBool("sec_am_en", false));

    if (preferences.isKey("sec_loiter"))
        securityMonitor.setLoiterTime(preferences.getULong("sec_loiter", 15000));
    if (preferences.isKey("sec_loit_en"))
        securityMonitor.setLoiterAlertEnabled(preferences.getBool("sec_loit_en", true));
    if (preferences.isKey("sec_hb"))
        securityMonitor.setHeartbeatInterval(preferences.getULong("sec_hb", 14400000)); // 4h default

    if (preferences.isKey("sec_rssi_thr"))
        securityMonitor.setRSSIThreshold(preferences.getInt("sec_rssi_thr", -80));
    if (preferences.isKey("sec_rssi_drop"))
        securityMonitor.setRSSIDropThreshold(preferences.getInt("sec_rssi_drop", 20));

    // Armed/Disarmed state & delays
    securityMonitor.setEntryDelay(preferences.getULong("sec_entry_dl", DEFAULT_ENTRY_DELAY_MS));
    securityMonitor.setExitDelay(preferences.getULong("sec_exit_dl", DEFAULT_EXIT_DELAY_MS));
    securityMonitor.setDisarmReminderEnabled(preferences.getBool("sec_dis_rem", false));
    securityMonitor.setTriggerTimeout(preferences.getULong("sec_trig_to", DEFAULT_TRIGGER_TIMEOUT_MS));
    securityMonitor.setAutoRearm(preferences.getBool("sec_auto_rearm", true));
    securityMonitor.setAlarmEnergyThreshold(preferences.getUChar("sec_alarm_en", DEFAULT_ALARM_ENERGY_THRESHOLD));
    securityMonitor.setAlarmDebounceFrames(preferences.getUChar("sec_debounce", 3));
    securityMonitor.setSirenPin(SIREN_PIN);
    if (preferences.getBool("sec_armed", false)) {
        // After DMS/watchdog restart, skip exit delay — was already armed
        bool wasAutoRestart = (g_prevRestartCause == "dms_no_mqtt_publish" ||
                               g_prevRestartCause == "wifi_failover_exhausted");
        securityMonitor.setArmed(true, wasAutoRestart); // immediate if auto-restart
    }

    #ifndef LITE_BUILD
    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
            type = "sketch";
        else // U_SPIFFS
            type = "filesystem";
        Serial.println("Start updating " + type);
        if (radarTaskHandle) vTaskSuspend(radarTaskHandle);
        radar.stop();
        // Telegram notification - best effort, don't wait for response
        // Network may be busy during OTA
        if (telegramBot.isEnabled()) {
            // Short attempt to send, but not critical
            Serial.println("[OTA] Attempting Telegram notification...");
            telegramBot.sendMessage("⚠️ OTA Update Started...");
        }
    });

    ArduinoOTA.setPassword(configManager.getConfig().auth_pass);
    ArduinoOTA.begin();
    #endif
    bootTime = millis();
    Serial.printf("[SYSTEM] LD2412 Security Node %s ready\n", FW_VERSION);

    // Start background connectivity watchdog (non-blocking to the main loop)
    xTaskCreatePinnedToCore(connectivityTask, "conn_check", 3072, nullptr, 1, nullptr, 0);

    if (telegramBot.isEnabled()) {
        telegramBot.sendMessage("🟢 *LD2412 Security Node Online*\n🏷️ Device: " + String(configManager.getConfig().mqtt_id) + "\nFW: " + String(FW_VERSION) + "\nIP: " + WiFi.localIP().toString());
    }
}

// -------------------------------------------------------------------------
// Loop
// -------------------------------------------------------------------------
void loop() {
    unsigned long now = millis();
    esp_task_wdt_reset();
    #ifndef NO_BLUETOOTH
    btService.update();
    #endif
    #ifndef LITE_BUILD
    ArduinoOTA.handle();
    #endif

    // Process WiFi reconnect request from connectivity task (thread-safe)
    if (wifiReconnectRequested && WiFi.status() == WL_CONNECTED) {
        wifiReconnectRequested = false;
        DBG("WiFi", "Reconnecting (requested by conn_check)");
        WiFi.reconnect();
    }

    // WiFi State Variables
    static unsigned long disconnectStart = 0;
    static bool isBackupActive = false;
    static uint8_t failoverAttempts = 0;
    static unsigned long currentFailoverInterval = TIMEOUT_WIFI_FAILOVER_MS;

    if (shouldReboot) {
        safeRestart("manual_reboot");
    }

    // Process pending zones update from async web handler (thread-safe)
    if (pendingZonesUpdate) {
        if (zonesMutex != NULL && xSemaphoreTake(zonesMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            zonesJson = pendingZonesJson;
            pendingZonesUpdate = false; // Clear inside mutex to prevent lost updates
            xSemaphoreGive(zonesMutex);
            saveZonesToNVS();
        }
    }

    if (configManager.getConfig().mqtt_enabled) mqttService.update();

    // Force full MQTT state replay after reconnect (broker restart, retained loss)
    if (mqttService.consumeReconnect()) {
        memset(lastPub.presence_state, 0, sizeof(lastPub.presence_state));
        memset(lastPub.alarm_state, 0, sizeof(lastPub.alarm_state));
        memset(lastPub.direction, 0, sizeof(lastPub.direction));
        memset(lastPub.motion_type, 0, sizeof(lastPub.motion_type));
        lastPub.tamper = !lastPub.tamper; // flip to force resend
        lastPub.anti_masking = !lastPub.anti_masking;
        lastPub.loitering = !lastPub.loitering;
        lastPub.rssi = 0;
        lastPub.health_score = 255;
        lastPub.eng_mode = !lastPub.eng_mode;
        DBG("MQTT", "State replay triggered after reconnect");
    }

    // Drain offline alarm buffer — replay persisted events after reconnect or across reboots
    if (mqttService.connected() && mqttBuffer.hasMessages()) {
        char bufTopic[MQTT_OFB_TOPIC_LEN];
        char bufPayload[MQTT_OFB_PAYLOAD_LEN];
        while (mqttBuffer.hasMessages()) {
            if (!mqttBuffer.peek(bufTopic, sizeof(bufTopic), bufPayload, sizeof(bufPayload))) break;
            if (mqttService.publish(bufTopic, bufPayload, false)) {
                mqttBuffer.consume();
            } else {
                break;
            }
        }
        if (!mqttBuffer.hasMessages()) {
            DBG("MQTTOfB", "Offline buffer drained");
        }
    }

    // Publish restart cause once after first MQTT connection
    static bool restartCausePublished = false;
    if (!restartCausePublished && mqttService.connected()) {
        restartCausePublished = true;
        const MQTTTopics& t = mqttService.getTopics();
        String cause = g_prevRestartCause;
        // Build combined message: "Software reset (dms_no_mqtt_publish)"
        esp_reset_reason_t r = esp_reset_reason();
        const char* rStr = "unknown";
        switch (r) {
            case ESP_RST_POWERON: rStr = "power_on"; break;
            case ESP_RST_SW:      rStr = "sw_reset"; break;
            case ESP_RST_PANIC:   rStr = "panic"; break;
            case ESP_RST_INT_WDT: rStr = "int_wdt"; break;
            case ESP_RST_TASK_WDT: rStr = "task_wdt"; break;
            case ESP_RST_WDT:      rStr = "wdt"; break;
            case ESP_RST_BROWNOUT: rStr = "brownout"; break;
            default: break;
        }
        String msg = String(rStr) + ":" + cause;
        // Append heap snapshot from previous run (if safeRestart was used)
        uint32_t prevHeap = preferences.getULong("last_heap", 0);
        uint32_t prevMaxAlloc = preferences.getULong("last_maxalloc", 0);
        uint32_t prevMinHeap = preferences.getULong("last_minheap", 0);
        if (prevHeap > 0) {
            msg += "|heap:" + String(prevHeap) + "/" + String(prevMaxAlloc) + "/" + String(prevMinHeap);
        }
        mqttService.publish(t.restart_cause, msg.c_str(), true);
        DBG("SYSTEM", "Published restart cause: %s", msg.c_str());
    }

    // One-shot gate config verification 40s after boot (ESPHome #13366: V1.26 may revert UART config)
    {
        static bool gateVerified = false;
        if (!gateVerified && now - bootTime >= TIMEOUT_GATE_VERIFY_MS) {
            gateVerified = true;
            uint8_t expectedMin = preferences.getUInt("radar_min", 0);
            uint8_t expectedMax = preferences.getUInt("radar_max", 13);
            if (!radar.verifyGateConfig(expectedMin, expectedMax)) {
                eventLog.addEvent(EVT_SYSTEM, 0, 0, "Gate config reverted by FW");
            }
        }
    }

    securityMonitor.update();
    telegramBot.update();
    eventLog.flush();

    // Scheduled arm/disarm (check every 30s)
    {
        static unsigned long lastSchedCheck = 0;
        if (now - lastSchedCheck > 30000) {
            lastSchedCheck = now;
            time_t epoch = time(nullptr);
            if (epoch > 1700000000) {
                struct tm timeinfo;
                localtime_r(&epoch, &timeinfo);
                int curMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
                const char* armTime = configManager.getConfig().sched_arm_time;
                const char* disarmTime = configManager.getConfig().sched_disarm_time;
                int armH, armM, disH, disM;
                if (strlen(armTime) >= 4 && sscanf(armTime, "%d:%d", &armH, &armM) == 2) {
                    int armMinutes = armH * 60 + armM;
                    if (curMinutes == armMinutes && !securityMonitor.isArmed()) {
                        securityMonitor.setArmed(true);
                        DBG("SCHED", "Auto-armed at %s", armTime);
                        systemLog.info("Scheduled arm at " + String(armTime));
                        if (telegramBot.isEnabled()) {
                            telegramBot.sendMessage("🔒 Scheduled arm (" + String(armTime) + ")");
                        }
                    }
                }
                if (strlen(disarmTime) >= 4 && sscanf(disarmTime, "%d:%d", &disH, &disM) == 2) {
                    int disMinutes = disH * 60 + disM;
                    if (curMinutes == disMinutes && securityMonitor.isArmed()) {
                        securityMonitor.setArmed(false);
                        DBG("SCHED", "Auto-disarmed at %s", disarmTime);
                        systemLog.info("Scheduled disarm at " + String(disarmTime));
                        if (telegramBot.isEnabled()) {
                            telegramBot.sendMessage("🔓 Scheduled disarm (" + String(disarmTime) + ")");
                        }
                    }
                }
            }
        }
    }

    // Auto-arm after N minutes of no presence
    {
        static unsigned long lastPresenceTime = now;
        static bool wasArmed = false;
        RadarData peekData = radar.getData();

        if (peekData.state != PresenceState::IDLE) {
            lastPresenceTime = now;
        }

        bool armed = securityMonitor.isArmed();
        if (wasArmed && !armed) {
            lastPresenceTime = now;
            DBG("AUTO-ARM", "Manual disarm — timer reset");
        }
        wasArmed = armed;

        uint16_t autoArmMin = configManager.getConfig().auto_arm_minutes;
        if (autoArmMin > 0 && !armed) {
            unsigned long elapsed = (now - lastPresenceTime) / 60000;
            if (elapsed >= autoArmMin) {
                securityMonitor.setArmed(true);
                wasArmed = true;
                lastPresenceTime = now;
                DBG("AUTO-ARM", "No presence for %u min — armed", autoArmMin);
                systemLog.info("Auto-arm: no presence " + String(autoArmMin) + "min");
                eventLog.addEvent(EVT_SECURITY, 0, 0, "Auto-arm (no presence)");
                if (telegramBot.isEnabled()) {
                    telegramBot.sendMessage("🔒 Auto-arm: no movement for " + String(autoArmMin) + " min");
                }
            }
        }
    }

    // Internet Connectivity Watchdog (TASK-032)
    // Connectivity watchdog moved to background task to avoid blocking the loop.

    // WiFi Failover Logic (TASK-031)
    if (WiFi.status() != WL_CONNECTED) {
        if (disconnectStart == 0) disconnectStart = now;

        // Exponential backoff: 60s, 120s, 240s, ... max 600s
        if (now - disconnectStart > currentFailoverInterval) {
            if (failoverAttempts < WIFI_FAILOVER_MAX_ATTEMPTS) {
                failoverAttempts++;
                DBG("WiFi", "Failover attempt %d/%d (interval %lus)", failoverAttempts, WIFI_FAILOVER_MAX_ATTEMPTS, currentFailoverInterval / 1000);

                if (!isBackupActive && strlen(configManager.getConfig().backup_ssid) > 0) {
                    // Switch to backup network
                    DBG("WiFi", "Switching to backup: %s", configManager.getConfig().backup_ssid);
                    WiFi.disconnect();
                    delay(100);
                    WiFi.begin(configManager.getConfig().backup_ssid, configManager.getConfig().backup_pass);
                    isBackupActive = true;
                } else if (isBackupActive) {
                    // Try to return to primary network — use SDK saved credentials (not hardcoded)
                    DBG("WiFi", "Trying primary network again (SDK saved)...");
                    WiFi.disconnect();
                    delay(100);
                    WiFi.begin(); // Uses SDK-saved credentials from setupWiFi()
                    isBackupActive = false;
                } else {
                    // No backup network configured, retry SDK saved primary
                    DBG("WiFi", "No backup configured, retrying saved primary...");
                    WiFi.disconnect();
                    delay(100);
                    WiFi.begin(); // Uses SDK-saved credentials
                }

                disconnectStart = now; // Reset timer for next attempt
                // Exponential backoff: double the interval, cap at max
                currentFailoverInterval = min(currentFailoverInterval * 2, WIFI_FAILOVER_MAX_INTERVAL_MS);
            } else {
                // Max attempts reached, restart
                systemLog.error("WiFi failover exhausted - rebooting");
                safeRestart("wifi_failover_exhausted");
            }
        }

        #ifndef NO_BLUETOOTH
        // Emergency Bluetooth: If disconnected for > 2 minutes, start BLE for recovery (TASK-BT-SAFE)
        if (now - disconnectStart > 120000 && !btService.isRunning()) {
            DBG("SYSTEM", "WiFi disconnected > 2min. Starting Emergency Bluetooth...");
            btService.begin(configManager.getConfig().mqtt_id, &configManager);
            btService.setTimeout(600); // 10 min
        }
        #endif
    } else {
        // Reset counters if connected
        disconnectStart = 0;
        if (failoverAttempts > 0) {
            failoverAttempts = 0;
            isBackupActive = false;
            currentFailoverInterval = TIMEOUT_WIFI_FAILOVER_MS; // Reset backoff
        }
    }

    RadarData data = radar.getData();

    // LED heartbeat (Stealth Mode)
    // Blink only for first 2 minutes OR if there is a security alert
    // Tamper always alerts; loitering/blind only when armed
    bool isArmedActive = securityMonitor.isArmed();
    bool securityAlert = data.tamper_alert
        || (isArmedActive && securityMonitor.isBlind() && securityMonitor.isAntiMaskEnabled())
        || (isArmedActive && securityMonitor.isLoitering() && securityMonitor.isLoiterAlertEnabled())
        || (securityMonitor.getAlarmState() == AlarmState::TRIGGERED);
    
    // Offline Alarm Memory (TASK-033)
    // FIX #8: Track actual TRIGGERED separately from other security alerts
    static bool offlineAlarmOccurred = false;
    static bool offlineTamperOccurred = false;
    static unsigned long offlineAlarmTime = 0;

    if (securityAlert) {
        // Fast strobe on alert - ONLY if LED enabled
        if (configManager.getConfig().led_enabled && now - lastLedBlink > 100) {
            lastLedBlink = now;
            digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        }

        // Catch events if offline (for HA sync later)
        if (!mqttService.connected()) {
            if (securityMonitor.getAlarmState() == AlarmState::TRIGGERED && !offlineAlarmOccurred) {
                offlineAlarmOccurred = true;
                offlineAlarmTime = now;
                systemLog.error("OFFLINE ALARM DETECTED! Waiting for sync...");
                DBG("SYSTEM", "OFFLINE ALARM TRIGGERED");
            }
            if (!offlineTamperOccurred && (data.tamper_alert || securityMonitor.isBlind() || securityMonitor.isLoitering())) {
                offlineTamperOccurred = true;
            }
        }
    } else if (configManager.getConfig().led_enabled && (now - bootTime) < (unsigned long)configManager.getConfig().startup_led_sec * 1000) {
        // Startup heartbeat (1s)
        if (now - lastLedBlink > 1000) {
            lastLedBlink = now;
            digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        }
    } else {
        // Stealth Mode: LED OFF
        digitalWrite(LED_PIN, LOW); // Assuming Active High LED. If Low, use HIGH.
    }

    // Sync Offline events once connected
    if (mqttService.connected() && (offlineAlarmOccurred || offlineTamperOccurred)) {
        if (offlineAlarmOccurred) {
            unsigned long diff = (now - offlineAlarmTime) / 1000;
            String msg = "⚠️ SYNC: ALARM occurred during network outage! (" + String(diff) + "s ago)";
            DBG("SYNC", "Reporting offline alarm to HA/Telegram");
            notificationService.sendTelegram(msg);
            mqttService.publish("home/security/log", msg.c_str());
        }
        if (offlineTamperOccurred && !offlineAlarmOccurred) {
            // FIX #8: Separate message for non-alarm security events
            String msg = "⚠️ SYNC: Tamper/blind/loitering detected during network outage (not alarm).";
            DBG("SYNC", "Reporting offline tamper event to HA/Telegram");
            notificationService.sendTelegram(msg);
            mqttService.publish("home/security/log", msg.c_str());
        }
        offlineAlarmOccurred = false;
        offlineTamperOccurred = false;
    }

    // Static learn completed — send result via Telegram
    if (radar.consumeLearnDone()) {
        JsonDocument learnDoc;
        radar.getLearnResultJson(learnDoc);
        String learnMsg = "📡 *Static Learn complete*\n";
        learnMsg += "Samples: " + String((int)learnDoc["static_samples"]) + " / " + String((int)learnDoc["total_samples"]) + "\n";
        learnMsg += "Static frequency: " + String((int)learnDoc["static_freq_pct"]) + "%\n";
        if ((bool)learnDoc["suggest_ready"]) {
            learnMsg += "✅ Suggested zone: *" + String((int)learnDoc["suggest_min_cm"]) + "–" + String((int)learnDoc["suggest_max_cm"]) + " cm*";
            learnMsg += " (gate " + String((int)learnDoc["top_gate"]) + ", confidence " + String((int)learnDoc["confidence"]) + "%)";
        } else {
            learnMsg += "⚠️ No significant static presence found.";
        }
        notificationService.sendTelegram(learnMsg);
    }

    // Security alarm trigger — 50ms tick (20 Hz) to catch short detections
    // FIX #9: Skip evaluation when radar data is unavailable (mutex timeout)
    static unsigned long lastAlarmCheck = 0;
    if (now - lastAlarmCheck >= 50 && data.valid) {
        lastAlarmCheck = now;
        securityMonitor.processRadarData(data.distance_cm, data.moving_energy, data.static_energy);
        radar.setTamperDetected(securityMonitor.isBlind() && securityMonitor.isAntiMaskEnabled());
    }

    // Slow diagnostics — 1s tick
    static unsigned long lastSecCheck = 0;
    if (now - lastSecCheck >= 1000) {
        lastSecCheck = now;
        securityMonitor.checkTamperState(data.tamper_alert);
        securityMonitor.checkRadarHealth(radar.isRadarConnected());
        securityMonitor.checkRSSIAnomaly(WiFi.RSSI());
    }

    // OTA Rollback Validation — mark as valid after 60s of stable operation
    if (!bootValidated && (now - bootTime) > TIMEOUT_OTA_VALIDATION_MS) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t ota_state;
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
            if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
                esp_ota_mark_app_valid_cancel_rollback();
                DBG("OTA", "App verified & rollback cancelled!");
                systemLog.info("OTA Update verified OK");
            }
        }
        bootValidated = true;
    }

    // Update last_uptime in preferences every 10 min (reduced NVS wear)
    static unsigned long lastUptimeSave = 0;
    if (now - lastUptimeSave > INTERVAL_UPTIME_SAVE_MS) {
        lastUptimeSave = now;
        preferences.putULong("last_uptime", now / 1000);
    }

    // Dead Man's Switch (TASK-028) — limited restarts to prevent boot loop
    // After 3 failed restarts, stay in degraded mode (web UI still works)
    static uint8_t dmsRestarts = preferences.getUInt("dms_count", 0); // Cache from NVS at first call
    static bool dmsDegraded = false;
    static bool dmsReconnectPending = false;
    static unsigned long dmsReconnectTime = 0;
    if (!dmsDegraded && configManager.getConfig().mqtt_enabled && strlen(configManager.getConfig().mqtt_server) > 0 && (now - bootTime) > TIMEOUT_DMS_STARTUP_MS &&
        (unsigned long)(now - mqttService.getLastPublishTime()) > TIMEOUT_DMS_NO_PUBLISH_MS) {

        // Skip DMS if WiFi signal is too weak — restart won't help
        if (WiFi.RSSI() < -85) {
            static unsigned long lastWeakWifiLog = 0;
            if (now - lastWeakWifiLog > 300000) {
                lastWeakWifiLog = now;
                DBG("SYSTEM", "DMS: skipping — weak WiFi (RSSI %d)", WiFi.RSSI());
            }
        } else if (!dmsReconnectPending) {
            // Phase 1: Try MQTT reconnect first before restarting ESP
            unsigned long publishAge = (now - mqttService.getLastPublishTime()) / 1000;
            DBG("SYSTEM", "DMS: No publish for %lus, connected=%d, RSSI=%d — forcing MQTT reconnect",
                publishAge, mqttService.connected(), WiFi.RSSI());
            systemLog.error("DMS: publish stale " + String(publishAge) + "s — reconnecting MQTT");
            mqttService.forceReconnect();
            dmsReconnectPending = true;
            dmsReconnectTime = now;
        } else if ((now - dmsReconnectTime) > 60000) {
            // Phase 2: 60s after reconnect — check if publish recovered
            if ((unsigned long)(now - mqttService.getLastPublishTime()) > TIMEOUT_DMS_NO_PUBLISH_MS) {
                // Reconnect didn't help — escalate to restart
                if (dmsRestarts < DMS_MAX_RESTARTS) {
                    dmsRestarts++;
                    preferences.putUInt("dms_count", dmsRestarts);
                    DBG("SYSTEM", "DMS: Reconnect failed. Restart %d/%d", dmsRestarts, DMS_MAX_RESTARTS);
                    systemLog.error("DMS restart #" + String(dmsRestarts) + " (reconnect failed)");
                    safeRestart("dms_no_mqtt_publish");
                } else {
                    dmsDegraded = true;
                    DBG("SYSTEM", "DMS: Max restarts reached. Degraded mode (local only).");
                    systemLog.error("DMS: Degraded mode — MQTT offline, local operation only");
                }
            } else {
                // Reconnect worked — publish recovered
                DBG("SYSTEM", "DMS: MQTT reconnect resolved publish issue");
                dmsReconnectPending = false;
            }
        }
    } else if (dmsReconnectPending && (unsigned long)(now - mqttService.getLastPublishTime()) < TIMEOUT_DMS_NO_PUBLISH_MS) {
        // Publish recovered during reconnect grace period
        dmsReconnectPending = false;
    }
    // Reset DMS counter only after successful MQTT publish (not just connected)
    if (dmsRestarts > 0 && mqttService.getLastPublishTime() > 0 &&
        (unsigned long)(now - mqttService.getLastPublishTime()) < TIMEOUT_DMS_NO_PUBLISH_MS) {
        preferences.putUInt("dms_count", 0);
        dmsRestarts = 0;
        if (dmsDegraded) {
            dmsDegraded = false;
            DBG("SYSTEM", "MQTT publish restored — exiting degraded mode");
            systemLog.info("MQTT restored, DMS counter reset");
        }
    }

    // SSE Realtime Telemetry (250ms)
#ifndef LITE_BUILD
    static unsigned long lastSSE = 0;
    if (now - lastSSE > INTERVAL_SSE_UPDATE_MS && ESP.getFreeHeap() >= HEAP_MIN_FOR_PUBLISH) {
        lastSSE = now;
        JsonDocument doc;
        radar.getTelemetryJson(doc);
        doc["uptime"] = now / 1000;
        doc["rssi"] = WiFi.RSSI();
        doc["armed"] = securityMonitor.isArmed();
        doc["alarm_state"] = securityMonitor.getAlarmStateStr();
        #if RADAR_OUT_PIN >= 0
        doc["out_pin"] = digitalRead(RADAR_OUT_PIN);
        #endif

        // Buffer must fit full telemetry JSON (~700-900 B incl. eng_mode arrays).
        // v3.10.0 used 256 B which silently dropped every event since serializeJson
        // returns sizeof(buf) on overflow → `sseLen < sizeof(sseBuf)` was false.
        char sseBuf[1024];
        size_t sseLen = serializeJson(doc, sseBuf, sizeof(sseBuf));
        if (sseLen > 0 && sseLen < sizeof(sseBuf)) {
            events.send(sseBuf, "telemetry", millis());
        }
    }
#endif

    // =====================================================================
    // MQTT Publish-on-Change with Deadband (3-tier system)
    // Tier 1: Critical state changes — immediate, every loop
    // Tier 2: Primary sensor data  — 2s active / 1h idle + deadband
    // Tier 3: Diagnostics          — 30s unconditional + deadband
    // Tier ENG: Engineering gates   — 10s + deadband
    // =====================================================================

    // Heap guard — skip all MQTT if memory is critically low
    if (ESP.getFreeHeap() < HEAP_MIN_FOR_PUBLISH) {
        static unsigned long lastHeapWarn = 0;
        if (now - lastHeapWarn > 10000) { lastHeapWarn = now; Serial.println("[WARN] Low heap — skipping MQTT publish"); }
    }
    else if (configManager.getConfig().mqtt_enabled) {
        const MQTTTopics& topics = mqttService.getTopics();

        // --- TIER 1: Critical state changes (immediate, every loop) ---
        {
            const char* stateStr = "idle";
            if (securityMonitor.isStaticFiltered()) {
                stateStr = "idle"; // known reflector — nepropagovat do HA
            } else if (data.state == PresenceState::PRESENCE_DETECTED) stateStr = "detected";
            else if (data.state == PresenceState::HOLD_TIMEOUT) stateStr = "detected";
            else if (data.state == PresenceState::TAMPER) stateStr = "detected";

            // FIX #10: Only update lastPub cache after successful publish
            if (strcmp(stateStr, lastPub.presence_state) != 0) {
                if (mqttService.publish(topics.presence_state, stateStr, true)) {
                    strncpy(lastPub.presence_state, stateStr, sizeof(lastPub.presence_state) - 1);
                    lastPub.presence_state[sizeof(lastPub.presence_state) - 1] = '\0';
                }
            }
            if (data.tamper_alert != lastPub.tamper) {
                if (mqttService.publish(topics.tamper, data.tamper_alert ? "true" : "false", true))
                    lastPub.tamper = data.tamper_alert;
            }
            bool blind = securityMonitor.isBlind();
            if (blind != lastPub.anti_masking) {
                if (mqttService.publish(topics.alert_anti_masking, blind ? "true" : "false", true))
                    lastPub.anti_masking = blind;
            }
            bool loiter = securityMonitor.isLoitering();
            if (loiter != lastPub.loitering) {
                if (mqttService.publish(topics.alert_loitering, loiter ? "true" : "false", true))
                    lastPub.loitering = loiter;
            }
            const char* alarmStr = securityMonitor.getAlarmStateStr();
            if (strcmp(alarmStr, lastPub.alarm_state) != 0) {
                if (mqttService.publish(topics.alarm_state, alarmStr, true)) {
                    strncpy(lastPub.alarm_state, alarmStr, sizeof(lastPub.alarm_state) - 1);
                    lastPub.alarm_state[sizeof(lastPub.alarm_state) - 1] = '\0';
                }
            }

            // motion_type: "moving" | "static" | "both" | "none"
            const char* mtStr = "none";
            if (!securityMonitor.isStaticFiltered() && data.state != PresenceState::IDLE) {
                if (data.moving_energy > 0 && data.static_energy > 0) mtStr = "both";
                else if (data.moving_energy > 0) mtStr = "moving";
                else if (data.static_energy > 0) mtStr = "static";
            }
            if (strcmp(mtStr, lastPub.motion_type) != 0) {
                if (mqttService.publish(topics.motion_type, mtStr, true)) {
                    strncpy(lastPub.motion_type, mtStr, sizeof(lastPub.motion_type) - 1);
                    lastPub.motion_type[sizeof(lastPub.motion_type) - 1] = '\0';
                }
            }

            // alarm/event: atomic JSON on PENDING/TRIGGERED
            // FIX #5: Peek first, only consume after successful publish
            {
                AlarmTriggerEvent evt;
                while (securityMonitor.peekAlarmEvent(evt)) {
                    JsonDocument evtDoc;
                    evtDoc["reason"]      = evt.reason;
                    evtDoc["zone"]        = evt.zone;
                    evtDoc["distance_cm"] = evt.distance_cm;
                    evtDoc["energy_mov"]  = evt.energy_mov;
                    evtDoc["energy_stat"] = evt.energy_stat;
                    evtDoc["motion_type"] = evt.motion_type;
                    evtDoc["uptime_s"]    = evt.uptime_s;
                    if (evt.iso_time[0]) evtDoc["time"] = evt.iso_time;

                    // Mesh: include verification status
                    if (peerCount > 0) {
                        evtDoc["mesh_peers"]     = peerCount;
                        evtDoc["mesh_confirmed"] = meshConfirmCount;
                        evtDoc["mesh_verified"]  = (meshConfirmCount > 0);
                    }

                    String evtJson;
                    serializeJson(evtDoc, evtJson);
                    if (mqttService.publish(topics.alarm_event, evtJson.c_str(), false)) {
                        securityMonitor.consumeAlarmEvent();
                    } else {
                        mqttBuffer.push(topics.alarm_event, evtJson.c_str());
                        securityMonitor.consumeAlarmEvent();
                        break;
                    }

                    // Mesh: send verify request to peers on entry_delay or immediate events
                    if (peerCount > 0 && (strcmp(evt.reason, "entry_delay") == 0 || strcmp(evt.reason, "immediate") == 0)) {
                        meshVerifyPending = true;
                        meshVerifyRequestTime = now;
                        meshConfirmCount = 0;
                        mqttService.publish(topics.mesh_verify_request, evtJson.c_str(), false);
                        DBG("MESH", "Verify request sent to %d peers", peerCount);
                    }
                }
            }

            // Mesh: check verify timeout — log result
            if (meshVerifyPending && now - meshVerifyRequestTime > MESH_VERIFY_TIMEOUT_MS) {
                meshVerifyPending = false;
                if (meshConfirmCount > 0) {
                    DBG("MESH", "Alarm VERIFIED by %d peer(s)", meshConfirmCount);
                } else {
                    DBG("MESH", "Alarm UNVERIFIED (no peers confirmed within %lus)", MESH_VERIFY_TIMEOUT_MS / 1000);
                }
            }
        }

        // --- TIER 2: Primary sensor data (2s active / 1h idle + deadband) ---
        {
            unsigned long teleInterval = INTERVAL_TELEMETRY_IDLE_MS;
            if (data.state != PresenceState::IDLE || data.tamper_alert || securityMonitor.isBlind() || securityMonitor.isLoitering()) {
                teleInterval = INTERVAL_TELEMETRY_ACTIVE_MS;
            }

            if (now - lastTele > teleInterval) {
                lastTele = now;

                char numBuf[16];
                if (changedU16(data.distance_cm, lastPub.distance_cm, DEADBAND_DISTANCE_CM)) {
                    snprintf(numBuf, sizeof(numBuf), "%u", data.distance_cm);
                    mqttService.publish(topics.distance, numBuf);
                    lastPub.distance_cm = data.distance_cm;
                }
                if (changedU8(data.moving_energy, lastPub.energy_mov, DEADBAND_ENERGY)) {
                    snprintf(numBuf, sizeof(numBuf), "%u", data.moving_energy);
                    mqttService.publish(topics.energy_mov, numBuf);
                    lastPub.energy_mov = data.moving_energy;
                }
                if (changedU8(data.static_energy, lastPub.energy_stat, DEADBAND_ENERGY)) {
                    snprintf(numBuf, sizeof(numBuf), "%u", data.static_energy);
                    mqttService.publish(topics.energy_stat, numBuf);
                    lastPub.energy_stat = data.static_energy;
                }
                String dirNow = securityMonitor.getDirection();
                if (strcmp(dirNow.c_str(), lastPub.direction) != 0) {
                    mqttService.publish(topics.motion_direction, dirNow.c_str());
                    strncpy(lastPub.direction, dirNow.c_str(), sizeof(lastPub.direction) - 1);
                    lastPub.direction[sizeof(lastPub.direction) - 1] = '\0';
                }
            }
        }

        // --- TIER 3: Diagnostics (30s unconditional + deadband) ---
        if (now - lastPub.lastDiagPublish > INTERVAL_TELEMETRY_DIAG_MS) {
            lastPub.lastDiagPublish = now;

            char numBuf[16];
            int8_t curRssi = (int8_t)WiFi.RSSI();
            if (changedI8(curRssi, lastPub.rssi, DEADBAND_RSSI)) {
                snprintf(numBuf, sizeof(numBuf), "%d", curRssi);
                mqttService.publish(topics.rssi, numBuf);
                lastPub.rssi = curRssi;
            }

            // Uptime always published (DMS keepalive)
            uint32_t curUptime = now / 1000;
            snprintf(numBuf, sizeof(numBuf), "%u", curUptime);
            mqttService.publish(topics.uptime, numBuf);
            lastPub.uptime_s = curUptime;

            uint8_t curHealth = radar.getHealthScore();
            if (changedU8(curHealth, lastPub.health_score, DEADBAND_HEALTH_SCORE)) {
                snprintf(numBuf, sizeof(numBuf), "%u", curHealth);
                mqttService.publish(topics.health_score, numBuf);
                lastPub.health_score = curHealth;
            }

            float curFR = radar.getFrameRate();
            if (changedF(curFR, lastPub.frame_rate, DEADBAND_FRAME_RATE)) {
                snprintf(numBuf, sizeof(numBuf), "%.1f", curFR);
                mqttService.publish(topics.frame_rate, numBuf);
                lastPub.frame_rate = curFR;
            }

            uint32_t curErrors = radar.getErrorCount();
            if (curErrors != lastPub.error_count) {
                snprintf(numBuf, sizeof(numBuf), "%u", curErrors);
                mqttService.publish(topics.error_count, numBuf);
                lastPub.error_count = curErrors;
            }

            const char* curUart = radar.getUARTStateString();
            if (strcmp(curUart, lastPub.uart_state) != 0) {
                mqttService.publish(topics.uart_state, curUart);
                strncpy(lastPub.uart_state, curUart, sizeof(lastPub.uart_state) - 1);
                lastPub.uart_state[sizeof(lastPub.uart_state) - 1] = '\0';
            }

            uint32_t curHeap = ESP.getFreeHeap() / 1024;
            if (changedU32(curHeap, lastPub.free_heap_kb, DEADBAND_FREE_HEAP_KB)) {
                snprintf(numBuf, sizeof(numBuf), "%u", curHeap);
                mqttService.publish(topics.free_heap, numBuf);
                lastPub.free_heap_kb = curHeap;
            }

            uint32_t curMaxAlloc = ESP.getMaxAllocHeap() / 1024;
            if (changedU32(curMaxAlloc, lastPub.max_alloc_kb, DEADBAND_FREE_HEAP_KB)) {
                snprintf(numBuf, sizeof(numBuf), "%u", curMaxAlloc);
                mqttService.publish(topics.max_alloc_heap, numBuf);
                lastPub.max_alloc_kb = curMaxAlloc;
            }

            bool curEngMode = radar.isEngineeringMode();
            if (curEngMode != lastPub.eng_mode) {
                mqttService.publish(topics.eng_mode, curEngMode ? "true" : "false", true);
                lastPub.eng_mode = curEngMode;
            }
        }

        // --- TIER ENG: Engineering gate data (10s + deadband) ---
        if (radar.isEngineeringMode() && (now - lastPub.lastEngPublish > INTERVAL_TELEMETRY_ENG_MS)) {
            lastPub.lastEngPublish = now;

            uint8_t curLight = radar.getLightLevel();
            if (changedU8(curLight, lastPub.light_level, DEADBAND_GATE_ENERGY)) {
                mqttService.publish(topics.light, String(curLight).c_str());
                lastPub.light_level = curLight;
            }

            uint8_t movCopy[14], statCopy[14];
            radar.getGateEnergiesSafe(movCopy, statCopy);

            for (int i = 0; i < 14; i++) {
                if (changedU8(movCopy[i], lastPub.gate_mov[i], DEADBAND_GATE_ENERGY)) {
                    char tMov[96];
                    snprintf(tMov, sizeof(tMov), "%s%d/moving", topics.eng_gate_base, i);
                    mqttService.publish(tMov, String(movCopy[i]).c_str());
                    lastPub.gate_mov[i] = movCopy[i];
                }
                if (changedU8(statCopy[i], lastPub.gate_stat[i], DEADBAND_GATE_ENERGY)) {
                    char tStat[96];
                    snprintf(tStat, sizeof(tStat), "%s%d/static", topics.eng_gate_base, i);
                    mqttService.publish(tStat, String(statCopy[i]).c_str());
                    lastPub.gate_stat[i] = statCopy[i];
                }
            }
        }

        // --- Supervision heartbeat: publish alive + check peers ---
        if (now - lastSupervisionPublish > SUPERVISION_INTERVAL_MS) {
            lastSupervisionPublish = now;
            mqttService.publish(topics.supervision_alive, "1", false);
            supervisionCheck();
        }
    } // end mqtt_enabled

    // --- STABILITY LOGGER (5s interval) ---
    static unsigned long lastStabilityLog = 0;
    if (now - lastStabilityLog > 5000) {
        lastStabilityLog = now;
        Serial.printf("STAB|%lu|%d|%d|%d|%ld|%u|%u|%u|%s|MQTT:%s\n",
            now / 1000,
            data.distance_cm,
            data.moving_energy,
            data.static_energy,
            WiFi.RSSI(),
            ESP.getFreeHeap(),
            ESP.getMinFreeHeap(),
            ESP.getMaxAllocHeap(),
            securityMonitor.getAlarmStateStr(),
            mqttService.connected() ? "CONNECTED" : "DISCONNECTED"
        );
    }

    delay(10); // Prevent tight loop, save CPU
}
