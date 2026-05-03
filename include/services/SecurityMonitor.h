#ifndef SECURITY_MONITOR_H
#define SECURITY_MONITOR_H

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <vector>
#include <freertos/semphr.h>
#include "NotificationService.h"
#include "EventLog.h"

// Forward declarations
class MQTTService;
class TelegramService;

enum class AlarmState {
    DISARMED,
    ARMING,    // Exit delay active
    ARMED,
    PENDING,   // Entry delay active
    TRIGGERED
};

// Alarm trigger event — published atomically to MQTT alarm/event
struct AlarmTriggerEvent {
    char reason[24];     // "entry_delay" | "immediate" | "entry_delay_expired" | "disarmed"
    char zone[16];
    uint16_t distance_cm;
    uint8_t energy_mov;
    uint8_t energy_stat;
    char motion_type[8]; // "moving" | "static" | "both" | "none"
    uint32_t uptime_s;
    char iso_time[20];   // "2026-03-21T22:14:05" or "" if NTP not synced
};

// Security event types
struct SecurityEvent {
    bool tamper_detected = false;
    bool wifi_jamming_detected = false;
    bool radar_disconnected = false;
    bool low_rssi = false;
    unsigned long last_event_time = 0;
};

struct AlertZone {
    uint16_t min_cm;
    uint16_t max_cm;
    uint8_t alert_level;     // 0=LOG, 1=INFO, 2=WARNING, 3=ALARM
    uint8_t alarm_behavior;  // 0=entry_delay (default), 1=immediate, 2=ignore, 3=ignore_static_only
    uint16_t delay_ms;
    bool enabled;
    char name[16];
    char valid_prev_zone[16]; // Entry path: required previous zone name ("" = any path allowed)
};

enum class ZoneState {
    NONE,
    ENTERED,
    OCCUPIED,
    EXITED
};

class SecurityMonitor {
public:
    SecurityMonitor();

    void begin(NotificationService* notifService, MQTTService* mqttService = nullptr, TelegramService* telegramService = nullptr, EventLog* eventLog = nullptr, Preferences* prefs = nullptr, const char* deviceLabel = nullptr);
    void update();

    // Zone Management
    void setZones(const std::vector<AlertZone>& zones) { _zones = zones; }
    String getCurrentZoneName() const { return _currentZoneName; }

    // Monitoring functions
    void checkRSSIAnomaly(long currentRSSI);
    void checkTamperState(bool isTamper);
    void checkRadarHealth(bool isConnected);
    void checkSystemHealth();
    
    // Security Pack v2.0
    void processRadarData(uint16_t distance, uint8_t move_energy, uint8_t static_energy);
    String getDirection() const { return _lastDirection; }
    bool isBlind() const { return _isBlind; }
    bool isLoitering() const { return _isLoitering; }
    bool isStaticFiltered() const { return _isStaticFiltered; }

    // Configuration
    void setRSSIThreshold(int threshold) { _rssiThreshold = threshold; }
    void setRSSIDropThreshold(int drop) { _rssiDropThreshold = drop; }
    void setAntiMaskTime(unsigned long ms) { _antiMaskThreshold = ms; }
    void setLoiterTime(unsigned long ms) { _loiterThreshold = ms; }
    void setPetImmunity(uint8_t energy) { _petImmunityThreshold = energy; }

    // Armed/Disarmed
    void setArmed(bool armed, bool immediate = false, bool homeMode = false);
    bool isArmed() const { return _alarmState == AlarmState::ARMED || _alarmState == AlarmState::ARMING || _alarmState == AlarmState::PENDING || _alarmState == AlarmState::TRIGGERED; }
    bool isHomeMode() const { return _homeMode; }
    AlarmState getAlarmState() const { return _alarmState; }
    const char* getAlarmStateStr() const;
    void setEntryDelay(unsigned long ms) { _entryDelay = ms; }
    void setExitDelay(unsigned long ms) { _exitDelay = ms; }
    unsigned long getEntryDelay() const { return _entryDelay; }
    unsigned long getExitDelay() const { return _exitDelay; }
    void setDisarmReminderEnabled(bool en) { _disarmReminderEnabled = en; }
    bool isDisarmReminderEnabled() const { return _disarmReminderEnabled; }
    void setDisarmReminderInterval(unsigned long ms) { _disarmReminderInterval = ms; }
    unsigned long getDisarmReminderInterval() const { return _disarmReminderInterval; }

    // Trigger timeout & auto-rearm
    void setTriggerTimeout(unsigned long ms) { _triggerTimeout = ms; }
    unsigned long getTriggerTimeout() const { return _triggerTimeout; }
    void setAutoRearm(bool en) { _autoRearm = en; }
    bool isAutoRearm() const { return _autoRearm; }

    // Alarm energy threshold (min energy to trigger ARMED→PENDING)
    void setAlarmEnergyThreshold(uint8_t e) { _alarmEnergyThreshold = e; }
    uint8_t getAlarmEnergyThreshold() const { return _alarmEnergyThreshold; }
    void setAlarmDebounceFrames(uint8_t n) { _alarmDebounceFrames = (n < 1) ? 1 : n; }
    uint8_t getAlarmDebounceFrames() const { return _alarmDebounceFrames; }

    // Siren GPIO
    void setSirenPin(int8_t pin);

    // Enable/Disable toggles (pro prázdné lokality - sklady, chaty, serverovny)
    void setAntiMaskEnabled(bool en) { _antiMaskEnabled = en; }
    void setLoiterAlertEnabled(bool en) { _loiterAlertEnabled = en; }
    void setHeartbeatInterval(unsigned long ms) { _heartbeatInterval = ms; }

    // Alarm event (atomický JSON pro MQTT — jednorázový)
    // FIX #5 + #17: All queue ops mutex-protected
    bool hasAlarmEvent() {
        if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
        bool has = _pendingEventCount > 0;
        if (_mutex) xSemaphoreGive(_mutex);
        return has;
    }
    // Peek without consuming — call consumeAlarmEvent() after successful publish
    bool peekAlarmEvent(AlarmTriggerEvent& out) {
        if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
        if (_pendingEventCount == 0) { if (_mutex) xSemaphoreGive(_mutex); return false; }
        out = _pendingEvents[_pendingEventHead];
        if (_mutex) xSemaphoreGive(_mutex);
        return true;
    }
    void consumeAlarmEvent() {
        if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
        if (_pendingEventCount > 0) {
            _pendingEventHead = (_pendingEventHead + 1) % ALARM_QUEUE_SIZE;
            _pendingEventCount--;
        }
        if (_mutex) xSemaphoreGive(_mutex);
    }

    // Last alarm event (pro API — persistentní, nepřepisuje se dokud nenastane nový trigger)
    bool hasLastAlarmEvent() const { return _lastAlarmEventValid; }
    const AlarmTriggerEvent& getLastAlarmEvent() const { return _lastAlarmEvent; }

    // Getters
    const SecurityEvent& getLastEvent() const { return _lastEvent; }
    bool isSystemHealthy() const { return _systemHealthy; }
    unsigned long getAntiMaskTime() const { return _antiMaskThreshold; }
    unsigned long getLoiterTime() const { return _loiterThreshold; }
    bool isAntiMaskEnabled() const { return _antiMaskEnabled; }
    bool isLoiterAlertEnabled() const { return _loiterAlertEnabled; }
    unsigned long getHeartbeatInterval() const { return _heartbeatInterval; }
    int getRSSIThreshold() const { return _rssiThreshold; }
    int getRSSIDropThreshold() const { return _rssiDropThreshold; }

private:
    void triggerAlert(NotificationType type, const String& message, const String& details = "", int16_t explicitDist = -1);
    void activateSiren();
    void deactivateSiren();

    SemaphoreHandle_t _mutex = nullptr;
    NotificationService* _notifService = nullptr;
    MQTTService* _mqttService;
    TelegramService* _telegramService = nullptr;
    EventLog* _eventLog = nullptr;
    Preferences* _prefs = nullptr;
    char _deviceLabel[40] = "";

    // Armed/Disarmed state
    AlarmState _alarmState = AlarmState::DISARMED;
    bool _homeMode = false;            // ARMED → reported as armed_home when true, armed_away otherwise
    unsigned long _entryDelay = 30000;
    unsigned long _exitDelay = 30000;
    unsigned long _exitDelayStart = 0;
    unsigned long _entryDelayStart = 0;
    unsigned long _triggerStartTime = 0;
    unsigned long _triggerTimeout = 900000;  // 15 min default
    bool _autoRearm = true;
    uint8_t _alarmEnergyThreshold = 15;  // Min energy to trigger alarm
    uint8_t _alarmDebounceFrames = 3;    // FIX #4: consecutive frames required before ARMED→PENDING/TRIGGERED
    uint8_t _armedDebounceCount = 0;     // Current consecutive qualifying frame count

    // Disarm reminder
    bool _disarmReminderEnabled = false; // Disabled - fusion handles reminders
    unsigned long _disarmReminderInterval = 1800000; // 30 min
    unsigned long _lastDisarmReminder = 0;
    unsigned long _lastPresenceWhileDisarmed = 0;
    unsigned long _presenceWhileDisarmedStart = 0;  // Sustained detection timer
    SecurityEvent _lastEvent;

    // Configurable Thresholds
    unsigned long _antiMaskThreshold = 300000; // Default 5 min
    unsigned long _loiterThreshold = 15000;    // Default 15 sec
    uint8_t _petImmunityThreshold = 10;        // Default energy threshold

    // Enable/Disable (pro prázdné lokality - sklady, chaty, serverovny)
    bool _antiMaskEnabled = false;   // DEFAULT: VYPNUTO - většina míst je občas prázdná
    bool _loiterAlertEnabled = false; // Disabled - fusion handles alerts
    unsigned long _heartbeatInterval = 14400000; // 4 hodiny default (ne 12h)

    // RSSI monitoring
    long _baselineRSSI = 0;
    float _rssiEWMA = 0.0f;           // Exponentially weighted moving average (α=0.2, ~5s window)
    uint8_t _rssiDropCount = 0;       // Consecutive samples confirming drop (require ≥2 to alert)
    int _rssiThreshold = -80;         // Below this = weak signal
    int _rssiDropThreshold = 20;      // Sudden drop ≥ this vs EWMA baseline = potential jamming
    unsigned long _rssiStableTime = 0;
    unsigned long _lowRssiStartTime = 0;
    unsigned long _startTime = 0;        // For overflow-safe startup checks
    bool _rssiBaselineEstablished = false;

    // Tamper monitoring
    bool _lastTamperState = false;
    unsigned long _tamperStartTime = 0;

    // Radar health
    bool _lastRadarConnected = true;
    unsigned long _radarDisconnectedTime = 0;

    // System health
    bool _systemHealthy = true;
    unsigned long _lastHealthCheck = 0;

    // Alert cooldowns (to prevent spam)
    unsigned long _lastWiFiAnomalyAlert = 0;
    unsigned long _lastTamperAlert = 0;
    unsigned long _lastRadarAlert = 0;

    // v2.0 State
    String _lastDirection = "none"; // approaching, retreating, stationary, none
    uint16_t _lastDistance = 0;
    unsigned long _lastDistTime = 0;
    
    // Zones
    std::vector<AlertZone> _zones;
    String _currentZoneName = "none";
    String _prevZoneName = "none";       // Previous zone (for entry path validation)
    int _currentZoneIndex = -1;
    unsigned long _zoneEnterTime = 0;
    bool _zoneAlertTriggered = false;

    // Static reflector filter
    bool _isStaticFiltered = false;

    // Anti-Masking
    bool _isBlind = false;
    unsigned long _zeroEnergyStart = 0;
    
    // Loitering
    bool _isLoitering = false;
    unsigned long _loiterStart = 0;

    // Alarm event queue — MQTT (up to 4 events buffered) + API (persistentní)
    static constexpr uint8_t ALARM_QUEUE_SIZE = 4;
    AlarmTriggerEvent _pendingEvent;  // staging variable for building events
    AlarmTriggerEvent _pendingEvents[ALARM_QUEUE_SIZE];
    uint8_t _pendingEventHead = 0;
    uint8_t _pendingEventCount = 0;
    AlarmTriggerEvent _lastAlarmEvent;
    bool _lastAlarmEventValid = false;
    void enqueueAlarmEvent();

    // Heartbeat
    unsigned long _lastHeartbeat = 0;

    // Siren
    int8_t _sirenPin = -1;
    bool _sirenActive = false;

    // Approach tracker — records detections while ARMED for forensic analysis
    static constexpr uint8_t APPROACH_LOG_SIZE = 16;
    struct ApproachEntry {
        uint32_t timestamp_s;   // epoch or uptime
        uint16_t distance_cm;
        uint8_t move_energy;
        uint8_t static_energy;
    };
    ApproachEntry _approachLog[APPROACH_LOG_SIZE];
    uint8_t _approachHead = 0;
    uint8_t _approachCount = 0;
    void logApproach(uint16_t dist, uint8_t move_en, uint8_t stat_en);
    String formatApproachLog() const;
    void clearApproachLog();
};

#endif
