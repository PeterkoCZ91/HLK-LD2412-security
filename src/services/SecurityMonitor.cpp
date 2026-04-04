#include "services/SecurityMonitor.h"
#include "services/MQTTService.h"
#include "services/TelegramService.h"
#include "debug.h"
#include "constants.h"

static void fillISOTime(char* buf, size_t len) {
    struct tm t;
    if (getLocalTime(&t, 100) && t.tm_year > 100) { // year > 2000
        strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &t);
    } else {
        buf[0] = '\0';
    }
}

void SecurityMonitor::enqueueAlarmEvent() {
    if (_pendingEventCount < ALARM_QUEUE_SIZE) {
        uint8_t idx = (_pendingEventHead + _pendingEventCount) % ALARM_QUEUE_SIZE;
        _pendingEvents[idx] = _pendingEvent;
        _pendingEventCount++;
    } else {
        DBG("SecMon", "ALARM QUEUE FULL — event dropped! reason=%s", _pendingEvent.reason);
    }
    _lastAlarmEvent = _pendingEvent;
    _lastAlarmEventValid = true;
}

static const char* motionTypeStr(uint8_t mov, uint8_t stat) {
    if (mov > 0 && stat > 0) return "both";
    if (mov > 0) return "moving";
    if (stat > 0) return "static";
    return "none";
}

SecurityMonitor::SecurityMonitor() {
    _mutex = xSemaphoreCreateMutex();
}

void SecurityMonitor::begin(NotificationService* notifService, MQTTService* mqttService, TelegramService* telegramService, EventLog* eventLog, Preferences* prefs, const char* deviceLabel) {
    _notifService = notifService;
    _mqttService = mqttService;
    _telegramService = telegramService;
    _eventLog = eventLog;
    _prefs = prefs;
    if (deviceLabel) {
        strncpy(_deviceLabel, deviceLabel, sizeof(_deviceLabel) - 1);
        _deviceLabel[sizeof(_deviceLabel) - 1] = '\0';
    }
    _lastRSSI = WiFi.RSSI();
    _baselineRSSI = _lastRSSI;
    _startTime = millis();
    DBG("SecMon", "Security Monitor initialized (label: %s)", _deviceLabel);
    DBG("SecMon", "Baseline RSSI: %ld dBm", _baselineRSSI);
}

void SecurityMonitor::update() {
    // Increased to 200ms — critical state transitions (PENDING→TRIGGERED,
    // ARMING→ARMED) must not be silently skipped due to mutex contention
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        DBG("SecMon", "update() mutex timeout — state transitions deferred");
        return;
    }
    unsigned long now = millis();

    // Run health check every minute
    if (now - _lastHealthCheck > INTERVAL_HEALTH_CHECK_MS) {
        _lastHealthCheck = now;
        checkSystemHealth();
    }

    // Exit delay: ARMING -> ARMED (A4: presence warning at transition)
    if (_alarmState == AlarmState::ARMING && now - _exitDelayStart >= _exitDelay) {
        _alarmState = AlarmState::ARMED;
        DBG("SecMon", "ARMED (exit delay expired)");
        if (_lastDistance > 0) {
            triggerAlert(NotificationType::ALARM_STATE_CHANGE, "🔒 System ARMED", "⚠️ Presence still detected at activation! Distance: " + String(_lastDistance) + " cm");
        } else {
            triggerAlert(NotificationType::ALARM_STATE_CHANGE, "🔒 System ARMED", "Exit delay complete.");
        }
    }

    // A1: TRIGGERED timeout — auto-silence after configurable period
    if (_alarmState == AlarmState::TRIGGERED && _triggerTimeout > 0 && now - _triggerStartTime >= _triggerTimeout) {
        deactivateSiren();
        if (_autoRearm) {
            _alarmState = AlarmState::ARMED;
            clearApproachLog(); // FIX #7: Clear stale approach history from previous incident
            DBG("SecMon", "TRIGGERED timeout — auto-rearmed to ARMED");
            triggerAlert(NotificationType::ALARM_STATE_CHANGE, "🔕 Alarm auto-silenced", "Timeout " + String(_triggerTimeout / 60000) + " min. System re-ARMED.");
        } else {
            _alarmState = AlarmState::DISARMED;
            DBG("SecMon", "TRIGGERED timeout — disarmed");
            triggerAlert(NotificationType::ALARM_STATE_CHANGE, "🔕 Alarm auto-silenced", "Timeout " + String(_triggerTimeout / 60000) + " min. System DISARMED.");
        }
    }

    // Disarm reminder: presence detected while DISARMED
    if (_alarmState == AlarmState::DISARMED && _disarmReminderEnabled && _lastPresenceWhileDisarmed > 0) {
        if (now - _lastDisarmReminder > _disarmReminderInterval) {
            _lastDisarmReminder = now;
            triggerAlert(NotificationType::HEALTH_WARNING,
                "⚠️ System is still DISARMED",
                "Presence detected but alarm is not enabled.\nUse /arm to activate.");
        }
    }
    if (_mutex) xSemaphoreGive(_mutex);
}

void SecurityMonitor::setArmed(bool armed, bool immediate) {
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
    unsigned long now = millis();
    if (armed) {
        // FIX #1: Guard against re-arming while active — reject if PENDING or TRIGGERED
        if (_alarmState == AlarmState::PENDING || _alarmState == AlarmState::TRIGGERED) {
            DBG("SecMon", "setArmed(true) REJECTED — state is %s", getAlarmStateStr());
            if (_mutex) xSemaphoreGive(_mutex);
            return;
        }
        // Already arming/armed — idempotent, no-op
        if (_alarmState == AlarmState::ARMING || _alarmState == AlarmState::ARMED) {
            DBG("SecMon", "setArmed(true) ignored — already %s", getAlarmStateStr());
            if (_mutex) xSemaphoreGive(_mutex);
            return;
        }
        if (immediate) {
            _alarmState = AlarmState::ARMED;
            DBG("SecMon", "ARMED (immediate)");
            triggerAlert(NotificationType::ALARM_STATE_CHANGE, "🔒 System ARMED", "Immediate activation.");
        } else {
            _alarmState = AlarmState::ARMING;
            _exitDelayStart = now;
            DBG("SecMon", "ARMING (exit delay %lu s)", _exitDelay / 1000);
            triggerAlert(NotificationType::ALARM_STATE_CHANGE, "⏳ ARMING...", "Exit delay: " + String(_exitDelay / 1000) + "s");
        }
        clearApproachLog();
        _armedDebounceCount = 0;
        _lastPresenceWhileDisarmed = 0;
        _presenceWhileDisarmedStart = 0;
        _lastDisarmReminder = 0;
    } else {
        AlarmState prev = _alarmState;
        // FIX #1: Always deactivate siren on disarm (covers TRIGGERED + any corrupted state)
        deactivateSiren();
        _alarmState = AlarmState::DISARMED;
        _entryDelayStart = 0;
        _exitDelayStart = 0;
        _lastPresenceWhileDisarmed = 0;
        _presenceWhileDisarmedStart = 0;
        _lastDisarmReminder = 0;
        DBG("SecMon", "DISARMED");
        if (prev != AlarmState::DISARMED) {
            triggerAlert(NotificationType::ALARM_STATE_CHANGE, "🔓 System DISARMED", "");
            strncpy(_pendingEvent.reason, "disarmed", sizeof(_pendingEvent.reason) - 1);
            strncpy(_pendingEvent.zone, _currentZoneName.c_str(), sizeof(_pendingEvent.zone) - 1);
            _pendingEvent.distance_cm = 0; _pendingEvent.energy_mov = 0; _pendingEvent.energy_stat = 0;
            strncpy(_pendingEvent.motion_type, "none", sizeof(_pendingEvent.motion_type) - 1);
            _pendingEvent.uptime_s = now / 1000; fillISOTime(_pendingEvent.iso_time, sizeof(_pendingEvent.iso_time));
            enqueueAlarmEvent();
        }
    }
    // Persist
    if (_prefs) {
        _prefs->putBool("sec_armed", armed);
    }
    if (_mutex) xSemaphoreGive(_mutex);
}

const char* SecurityMonitor::getAlarmStateStr() const {
    switch (_alarmState) {
        case AlarmState::DISARMED:  return "disarmed";
        case AlarmState::ARMING:    return "arming";
        case AlarmState::ARMED:     return "armed_away";
        case AlarmState::PENDING:   return "pending";
        case AlarmState::TRIGGERED: return "triggered";
        default: return "disarmed";
    }
}

void SecurityMonitor::checkRSSIAnomaly(long currentRSSI) {
    unsigned long now = millis();

    // Establish baseline after 30 seconds of stable operation
    if (!_rssiBaselineEstablished && (now - _startTime) > INTERVAL_RSSI_BASELINE_MS) {
        _baselineRSSI = currentRSSI;
        _rssiBaselineEstablished = true;
        _rssiStableTime = now;
        DBG("SecMon", "RSSI baseline established: %ld dBm", _baselineRSSI);
    }

    // Check for sudden RSSI drop (potential WiFi jamming)
    long rssiDelta = _lastRSSI - currentRSSI;

    if (rssiDelta > _rssiDropThreshold && _rssiBaselineEstablished) {
        // Sudden drop detected
        if (now - _lastWiFiAnomalyAlert > COOLDOWN_WIFI_ANOMALY_MS) {
            String msg = "Sudden RSSI drop detected!";
            String details = "Previous: " + String(_lastRSSI) + " dBm\n";
            details += "Current: " + String(currentRSSI) + " dBm\n";
            details += "Drop: " + String(rssiDelta) + " dBm";

            DBG("SecMon", "WiFi Anomaly: RSSI dropped by %ld dBm", rssiDelta);
            triggerAlert(NotificationType::WIFI_ANOMALY, msg, details);
            _lastWiFiAnomalyAlert = now;

            _lastEvent.wifi_jamming_detected = true;
            _lastEvent.last_event_time = now;
        }
    }

    // Check for critically low RSSI (Sustained)
    if (currentRSSI < _rssiThreshold) {
        if (_lowRssiStartTime == 0) _lowRssiStartTime = now;
        
        // Only alert if sustained for > 2 minutes
        if (now - _lowRssiStartTime > TIMEOUT_LOW_RSSI_SUSTAINED_MS) {
            if (!_lastEvent.low_rssi && now - _lastWiFiAnomalyAlert > COOLDOWN_WIFI_ANOMALY_MS) {
                String msg = "WiFi signal unstable (Sustained)";
                String details = "Current RSSI: " + String(currentRSSI) + " dBm\n";
                details += "Below threshold (" + String(_rssiThreshold) + ") for > 2 min.";

                DBG("SecMon", "CRITICAL RSSI: %ld dBm", currentRSSI);
                triggerAlert(NotificationType::WIFI_ANOMALY, msg, details);
                _lastWiFiAnomalyAlert = now;

                _lastEvent.low_rssi = true;
                _lastEvent.last_event_time = now;
            }
        }
    } else {
        _lowRssiStartTime = 0; // Reset timer
        _lastEvent.low_rssi = false;
    }

    _lastRSSI = currentRSSI;
}

void SecurityMonitor::checkTamperState(bool isTamper) {
    unsigned long now = millis();

    // Tamper state changed from false to true
    if (isTamper && !_lastTamperState) {
        _tamperStartTime = now;

        if (now - _lastTamperAlert > COOLDOWN_TAMPER_ALERT_MS) {
            String msg = "🚨 TAMPER ALERT! Sensor may be obstructed or tampered with.";
            String details = "Immediate action required!\n";
            details += "Check sensor placement and surroundings.";

            DBG("SecMon", "TAMPER DETECTED!");
            triggerAlert(NotificationType::TAMPER_ALERT, msg, details);
            _lastTamperAlert = now;

            _lastEvent.tamper_detected = true;
            _lastEvent.last_event_time = now;
        }
    }

    // Tamper cleared
    if (!isTamper && _lastTamperState) {
        DBG("SecMon", "Tamper cleared");
        _lastEvent.tamper_detected = false;
    }

    _lastTamperState = isTamper;
}

void SecurityMonitor::checkRadarHealth(bool isConnected) {
    unsigned long now = millis();

    // Radar just disconnected
    if (!isConnected && _lastRadarConnected) {
        _radarDisconnectedTime = now;
        DBG("SecMon", "Radar disconnected");
    }

    // Radar been disconnected for more than 30 seconds
    if (!isConnected && (now - _radarDisconnectedTime > TIMEOUT_RADAR_DISCONNECT_MS)) {
        if (now - _lastRadarAlert > COOLDOWN_RADAR_ALERT_MS) {
            String msg = "Radar sensor connection lost";
            String details = "Duration: " + String((now - _radarDisconnectedTime) / 1000) + "s\n";
            details += "Check UART connection and power supply.";

            DBG("SecMon", "Radar offline for extended period");
            triggerAlert(NotificationType::SYSTEM_ERROR, msg, details);
            _lastRadarAlert = now;

            _lastEvent.radar_disconnected = true;
            _lastEvent.last_event_time = now;
        }
    }

    // Radar reconnected
    if (isConnected && !_lastRadarConnected) {
        DBG("SecMon", "Radar reconnected");
        _lastEvent.radar_disconnected = false;
    }

    _lastRadarConnected = isConnected;
}

// -------------------------------------------------------------------------
// Security Pack v2.0 Implementation
// -------------------------------------------------------------------------
void SecurityMonitor::processRadarData(uint16_t distance, uint8_t move_energy, uint8_t static_energy) {
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    unsigned long now = millis();

    // Reset per-frame filtered flag
    _isStaticFiltered = false;

    // Pet Immunity — ignore low energy (move and static)
    if (_petImmunityThreshold > 0) {
        if (move_energy > 0 && move_energy < _petImmunityThreshold) {
            move_energy = 0;
        }
        if (static_energy > 0 && static_energy < _petImmunityThreshold) {
            static_energy = 0;
        }
    }

    // 1. Direction Detection (Approach vs Retreat)
    if (distance > 0 && _lastDistance > 0 && (now - _lastDistTime > 500)) { // Check every 0.5s
        int delta = (int)distance - (int)_lastDistance;
        
        if (abs(delta) > 20) { // Threshold 20cm to avoid jitter
            if (delta < 0) _lastDirection = "approaching";
            else _lastDirection = "retreating";
        } else {
            if (move_energy > 0 || static_energy > 0) _lastDirection = "stationary";
            else _lastDirection = "none";
        }
        
        _lastDistance = distance;
        _lastDistTime = now;
    } else if (distance > 0 && _lastDistance == 0) {
        _lastDistance = distance; // Init
        _lastDistTime = now;
    } else if (distance == 0) {
        _lastDirection = "none";
        _lastDistance = 0;
    }

    // 2. Anti-Masking (Blind Detection) - CONFIGURABLE
    // For empty locations (warehouses, cabins, server rooms) silence may be normal
    if (move_energy == 0 && static_energy == 0) {
        if (_zeroEnergyStart == 0) _zeroEnergyStart = now;

        // Use configurable threshold (default 5 min)
        if (now - _zeroEnergyStart > _antiMaskThreshold) {
            if (!_isBlind) {
                _isBlind = true;
                DBG("SecMon", "Anti-Masking: Silence detected (>%lu min)", _antiMaskThreshold / MS_PER_MINUTE);

                // ONLY if anti-masking is ENABLED, send notification
                if (_antiMaskEnabled) {
                    String msg = "⚠️ ANTI-MASKING: Sensor detects no activity!";
                    String details = "Possible tampering (sensor covered) or empty room.\n";
                    details += "Silence duration: " + String(_antiMaskThreshold / MS_PER_MINUTE) + " min";
                    triggerAlert(NotificationType::TAMPER_ALERT, msg, details);
                }
            }
        }
    } else {
        _zeroEnergyStart = 0;
        if (_isBlind) {
            _isBlind = false;
            DBG("SecMon", "Anti-Masking: Activity restored");
        }
    }

    // 3. Heartbeat / Periodic Report (configurable interval)
    if (_lastHeartbeat == 0) _lastHeartbeat = now;

    if (_heartbeatInterval > 0 && now - _lastHeartbeat > _heartbeatInterval) {
        _lastHeartbeat = now;

        unsigned long uptimeHours = millis() / MS_PER_HOUR;
        unsigned long uptimeDays = uptimeHours / 24;

        String msg = "🟢 Heartbeat: ONLINE & GUARDING";
        String details = "Uptime: ";
        if (uptimeDays > 0) {
            details += String(uptimeDays) + "d " + String(uptimeHours % 24) + "h";
        } else {
            details += String(uptimeHours) + "h";
        }
        details += "\nWiFi: " + String(WiFi.RSSI()) + " dBm";
        details += "\nStatus: " + String(_isBlind ? "Silent (no activity)" : "Active");

        triggerAlert(NotificationType::HEALTH_WARNING, msg, details);
    }

    // 4. Loitering (Suspicious lingering) - CONFIGURABLE
    // Someone is close (< 2m) and staying for configurable time
    if (distance > 0 && distance < 200) {
        if (_loiterStart == 0) _loiterStart = now;

        if (now - _loiterStart > _loiterThreshold) {
            if (!_isLoitering) {
                _isLoitering = true;
                DBG("SecMon", "Loitering: Person <2m for >%lu sec", _loiterThreshold / 1000);

                if (_loiterAlertEnabled) {
                    String msg = "👤 LOITERING: Someone lingering in close zone!";
                    String details = "Distance: " + String(distance) + " cm\n";
                    details += "Duration: >" + String(_loiterThreshold / 1000) + " seconds";
                    triggerAlert(NotificationType::PRESENCE_DETECTED, msg, details);
                } else {
                    DBG("SecMon", "Loitering detected but notifications are DISABLED in config");
                }
            }
        }
    } else {
        _loiterStart = 0;
        _isLoitering = false;
    }

    // 5. Zone Logic — MUST run BEFORE alarm trigger to use current zone behavior
    int newZoneIndex = -1;
    String newZoneName = "none";

    if (distance > 0) {
        for (size_t i = 0; i < _zones.size(); i++) {
            if (_zones[i].enabled && distance >= _zones[i].min_cm && distance <= _zones[i].max_cm) {
                newZoneIndex = i;
                newZoneName = String(_zones[i].name);
                break; // Use first matching zone (priority by order)
            }
        }
    }

    if (newZoneName != _currentZoneName) {
        _prevZoneName = _currentZoneName;  // Track previous zone for path validation
        _currentZoneName = newZoneName;
        _currentZoneIndex = newZoneIndex;
        _zoneEnterTime = now;
        _zoneAlertTriggered = false;
        DBG("SecMon", "Entered Zone: %s (prev: %s)", _currentZoneName.c_str(), _prevZoneName.c_str());

        // Publish via MQTT
        if (_mqttService && _mqttService->connected()) {
            _mqttService->publish(_mqttService->getTopics().current_zone, _currentZoneName.c_str(), true);
        }
    }

    if (_currentZoneIndex >= 0) {
        const AlertZone& z = _zones[_currentZoneIndex];
        if (now - _zoneEnterTime > z.delay_ms && !_zoneAlertTriggered) {
             _zoneAlertTriggered = true;
             if (z.alert_level > 0 && _alarmState != AlarmState::DISARMED && _alarmState != AlarmState::ARMING) {
                 // FIX #18: alert_level 3 used TAMPER_ALERT which shares cooldown with real tamper events
                 // Zone alerts are informational, use HEALTH_WARNING for high-level zones too
                 NotificationType nt = NotificationType::PRESENCE_DETECTED;
                 if (z.alert_level >= 2) nt = NotificationType::HEALTH_WARNING;
                 String msg = "Zone entry: " + String(z.name);
                 String details = "Distance: " + String(distance) + " cm";
                 triggerAlert(nt, msg, details);
             }
        }
    }

    // 4a2. Static reflector filter — independent of armed state
    // If detection is in zone behavior==3 and purely static energy → always filter
    if (_currentZoneIndex >= 0 && (size_t)_currentZoneIndex < _zones.size() &&
        _zones[_currentZoneIndex].alarm_behavior == 3 &&
        move_energy < _alarmEnergyThreshold && distance > 0) {
        _isStaticFiltered = true;
        DBG("SecMon", "Static-filtered zone '%s' (move=%d < thr=%d)", _currentZoneName.c_str(), move_energy, _alarmEnergyThreshold);
    }

    // 4b. Approach logging — record every detection while ARMED (forensic trail)
    if ((_alarmState == AlarmState::ARMED || _alarmState == AlarmState::PENDING) &&
        distance > 0 && (move_energy > 0 || static_energy > 0)) {
        logApproach(distance, move_energy, static_energy);
    }

    // 4c. Armed state: entry delay / triggered logic (uses current zone from above)
    // FIX #4: Debounce — require N consecutive qualifying frames before transition
    bool armedQualifies = (_alarmState == AlarmState::ARMED && distance > 0 &&
        (move_energy >= _alarmEnergyThreshold || static_energy >= _alarmEnergyThreshold));

    if (armedQualifies) {
        _armedDebounceCount++;
    } else if (_alarmState == AlarmState::ARMED) {
        _armedDebounceCount = 0;
    }

    if (armedQualifies && _armedDebounceCount >= _alarmDebounceFrames) {
        _armedDebounceCount = 0; // Reset for next detection

        uint8_t behavior = 0; // default = entry delay
        if (_currentZoneIndex >= 0 && (size_t)_currentZoneIndex < _zones.size()) {
            behavior = _zones[_currentZoneIndex].alarm_behavior;

            // Entry path validation: if zone requires a specific previous zone,
            // and the intruder came from a different path → force immediate trigger
            const char* reqPrev = _zones[_currentZoneIndex].valid_prev_zone;
            if (reqPrev[0] != '\0' && behavior == 0) {
                if (_prevZoneName != String(reqPrev)) {
                    DBG("SecMon", "INVALID PATH: zone '%s' requires prev '%s' but got '%s' → immediate",
                        _currentZoneName.c_str(), reqPrev, _prevZoneName.c_str());
                    behavior = 1; // Override to immediate trigger
                }
            }
        }

        if (behavior == 2) {
            DBG("SecMon", "Detection in ignore-zone '%s', skipping alarm", _currentZoneName.c_str());
        } else if (behavior == 3) {
            // ignore_static_only: fixed reflectors (e.g. metal ladder) generate only static without movement
            if (move_energy < _alarmEnergyThreshold) {
                // _isStaticFiltered already set above
                DBG("SecMon", "Static-only in zone '%s' (move=%d < thr=%d), skip",
                    _currentZoneName.c_str(), move_energy, _alarmEnergyThreshold);
            } else {
                // Movement present → entry delay (real intruder)
                _alarmState = AlarmState::PENDING;
                _entryDelayStart = now;
                strncpy(_pendingEvent.reason, "entry_delay", sizeof(_pendingEvent.reason) - 1);
                strncpy(_pendingEvent.zone, _currentZoneName.c_str(), sizeof(_pendingEvent.zone) - 1);
                _pendingEvent.distance_cm = distance; _pendingEvent.energy_mov = move_energy; _pendingEvent.energy_stat = static_energy;
                strncpy(_pendingEvent.motion_type, motionTypeStr(move_energy, static_energy), sizeof(_pendingEvent.motion_type) - 1);
                _pendingEvent.uptime_s = now / 1000; fillISOTime(_pendingEvent.iso_time, sizeof(_pendingEvent.iso_time));
                enqueueAlarmEvent();
                DBG("SecMon", "PENDING — move in static-filter zone '%s'", _currentZoneName.c_str());
                triggerAlert(NotificationType::ENTRY_DETECTED, "⏳ ENTRY DETECTED!",
                    "Entry delay: " + String(_entryDelay / 1000) + "s\nZone: " + _currentZoneName + "\n" + formatApproachLog(), distance);
            }
        } else if (behavior == 1) {
            _alarmState = AlarmState::TRIGGERED;
            _triggerStartTime = now;
            strncpy(_pendingEvent.reason, "immediate", sizeof(_pendingEvent.reason) - 1);
            strncpy(_pendingEvent.zone, _currentZoneName.c_str(), sizeof(_pendingEvent.zone) - 1);
            _pendingEvent.distance_cm = distance; _pendingEvent.energy_mov = move_energy; _pendingEvent.energy_stat = static_energy;
            strncpy(_pendingEvent.motion_type, motionTypeStr(move_energy, static_energy), sizeof(_pendingEvent.motion_type) - 1);
            _pendingEvent.uptime_s = now / 1000; fillISOTime(_pendingEvent.iso_time, sizeof(_pendingEvent.iso_time));
            enqueueAlarmEvent();
            DBG("SecMon", "IMMEDIATE TRIGGER in zone '%s'!", _currentZoneName.c_str());
            triggerAlert(NotificationType::ALARM_TRIGGERED, "🚨 ALARM TRIGGERED!", "Immediate trigger in zone: " + _currentZoneName + "\n" + formatApproachLog(), distance);
            activateSiren();
        } else {
            _alarmState = AlarmState::PENDING;
            _entryDelayStart = now;
            strncpy(_pendingEvent.reason, "entry_delay", sizeof(_pendingEvent.reason) - 1);
            strncpy(_pendingEvent.zone, _currentZoneName.c_str(), sizeof(_pendingEvent.zone) - 1);
            _pendingEvent.distance_cm = distance; _pendingEvent.energy_mov = move_energy; _pendingEvent.energy_stat = static_energy;
            strncpy(_pendingEvent.motion_type, motionTypeStr(move_energy, static_energy), sizeof(_pendingEvent.motion_type) - 1);
            _pendingEvent.uptime_s = now / 1000; fillISOTime(_pendingEvent.iso_time, sizeof(_pendingEvent.iso_time));
            enqueueAlarmEvent();
            DBG("SecMon", "PENDING (entry delay %lu s)", _entryDelay / 1000);
            triggerAlert(NotificationType::ENTRY_DETECTED, "⏳ ENTRY DETECTED!", "Entry delay: " + String(_entryDelay / 1000) + "s\n" + formatApproachLog() + "Use /disarm to deactivate.", distance);
        }
    }
    else if (_alarmState == AlarmState::PENDING && now - _entryDelayStart >= _entryDelay) {
        _alarmState = AlarmState::TRIGGERED;
        _triggerStartTime = now;
        strncpy(_pendingEvent.reason, "entry_delay_expired", sizeof(_pendingEvent.reason) - 1);
        // zone/distance/energy preserved from last PENDING event — add uptime
        _pendingEvent.uptime_s = now / 1000; fillISOTime(_pendingEvent.iso_time, sizeof(_pendingEvent.iso_time));
        enqueueAlarmEvent();
        DBG("SecMon", "ALARM TRIGGERED!");
        triggerAlert(NotificationType::ALARM_TRIGGERED, "🚨 ALARM TRIGGERED!", "Entry delay expired!\n" + formatApproachLog(), _pendingEvent.distance_cm);
        activateSiren();
    }

    // Track sustained presence while disarmed (for reminder)
    if (_alarmState == AlarmState::DISARMED && distance > 0 && (move_energy > 0 || static_energy > 0)) {
        if (_presenceWhileDisarmedStart == 0) {
            _presenceWhileDisarmedStart = now;
        } else if (now - _presenceWhileDisarmedStart > 10000) {
            _lastPresenceWhileDisarmed = now;
        }
    } else {
        _presenceWhileDisarmedStart = 0;
    }
    if (_mutex) xSemaphoreGive(_mutex);
}

void SecurityMonitor::checkSystemHealth() {
    bool healthy = true;

    // Check WiFi
    if (WiFi.status() != WL_CONNECTED) {
        DBG("SecMon", "Health Check: WiFi disconnected");
        healthy = false;
    }

    // Check RSSI
    long rssi = WiFi.RSSI();
    if (rssi < _rssiThreshold) {
        DBG("SecMon", "Health Check: Weak WiFi signal (%ld dBm)", rssi);
        healthy = false;
    }

    // Check free heap
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < HEAP_LOW_WARNING) {
        DBG("SecMon", "Health Check: Low memory (%u bytes)", freeHeap);
        healthy = false;

        if (!_systemHealthy) {  // Was already unhealthy, send alert
            String msg = "System health warning: Low memory";
            String details = "Free heap: " + String(freeHeap) + " bytes";
            triggerAlert(NotificationType::HEALTH_WARNING, msg, details);
        }
    }

    // Check certificate expiry (once per day)
    static unsigned long lastCertCheck = 0;
    unsigned long now = millis();
    if (_mqttService && _mqttService->connected() && (now - lastCertCheck > INTERVAL_CERT_CHECK_MS)) {
        DBG("SecMon", "Checking MQTT certificate expiry...");
        _mqttService->checkCertificateExpiry();
        lastCertCheck = now;
    }

    if (healthy && !_systemHealthy) {
        DBG("SecMon", "System health restored");
    }

    _systemHealthy = healthy;
}

void SecurityMonitor::triggerAlert(NotificationType type, const String& message, const String& details, int16_t explicitDist) {
    // Log to EventLog if available
    if (_eventLog) {
        uint8_t evtType = EVT_SYSTEM;
        switch (type) {
            case NotificationType::PRESENCE_DETECTED: evtType = EVT_PRESENCE; break;
            case NotificationType::TAMPER_ALERT: evtType = EVT_TAMPER; break;
            case NotificationType::WIFI_ANOMALY: evtType = EVT_WIFI; break;
            case NotificationType::HEALTH_WARNING: evtType = EVT_HEARTBEAT; break;
            case NotificationType::ALARM_STATE_CHANGE: evtType = EVT_SECURITY; break;
            case NotificationType::ENTRY_DETECTED: evtType = EVT_SECURITY; break;
            case NotificationType::ALARM_TRIGGERED: evtType = EVT_SECURITY; break;
            default: evtType = EVT_SYSTEM; break;
        }
        // FIX #3: Use explicit distance when provided, not stale _lastDistance
        uint16_t logDist = (explicitDist >= 0) ? (uint16_t)explicitDist : _lastDistance;
        _eventLog->addEvent(evtType, logDist, 0, message.c_str());
        // FIX #16: Immediately persist critical security events
        if (evtType == EVT_SECURITY) {
            _eventLog->flushNow();
        }
    }

    // Try NotificationService first (has cooldown, filtering, multi-channel)
    if (_notifService && _notifService->isEnabled()) {
        _notifService->sendAlert(type, message, details);
        return;
    }

    // Fallback: send directly via TelegramService if NotificationService is disabled
    // BUT MUST respect individual feature toggles!
    if (_telegramService && _telegramService->isEnabled()) {
        bool shouldSend = true;

        if (type == NotificationType::PRESENCE_DETECTED && !_loiterAlertEnabled) shouldSend = false;
        if (type == NotificationType::HEALTH_WARNING && message.indexOf("Blind") >= 0 && !_antiMaskEnabled) shouldSend = false;
        // ALARM_STATE_CHANGE and ENTRY_DETECTED always send (security-critical)

        if (shouldSend) {
            String fullMsg;
            if (strlen(_deviceLabel) > 0) {
                fullMsg = "[" + String(_deviceLabel) + "] " + message;
            } else {
                fullMsg = message;
            }
            if (details.length() > 0) {
                fullMsg += "\n" + details;
            }
            _telegramService->sendMessage(fullMsg);
        }
    }
}

// --- Siren GPIO ---

void SecurityMonitor::setSirenPin(int8_t pin) {
    _sirenPin = pin;
    if (_sirenPin >= 0) {
        pinMode(_sirenPin, OUTPUT);
        digitalWrite(_sirenPin, LOW);
        DBG("SecMon", "Siren pin configured: GPIO %d", _sirenPin);
    }
}

void SecurityMonitor::activateSiren() {
    if (_sirenPin >= 0 && !_sirenActive) {
        digitalWrite(_sirenPin, HIGH);
        _sirenActive = true;
        DBG("SecMon", "SIREN ON (GPIO %d)", _sirenPin);
    }
}

void SecurityMonitor::deactivateSiren() {
    if (_sirenPin >= 0 && _sirenActive) {
        digitalWrite(_sirenPin, LOW);
        _sirenActive = false;
        DBG("SecMon", "SIREN OFF (GPIO %d)", _sirenPin);
    }
}

// --- Approach Tracker ---

void SecurityMonitor::logApproach(uint16_t dist, uint8_t move_en, uint8_t stat_en) {
    time_t epoch = time(nullptr);
    uint32_t ts = (epoch > 1700000000) ? (uint32_t)epoch : millis() / 1000;

    // Deduplicate: skip if same distance as last entry and less than 2s apart
    if (_approachCount > 0) {
        uint8_t lastIdx = (_approachHead + _approachCount - 1) % APPROACH_LOG_SIZE;
        if (_approachLog[lastIdx].distance_cm == dist && ts - _approachLog[lastIdx].timestamp_s < 2) {
            return;
        }
    }

    uint8_t idx = (_approachHead + _approachCount) % APPROACH_LOG_SIZE;
    if (_approachCount >= APPROACH_LOG_SIZE) {
        // Full — overwrite oldest
        _approachHead = (_approachHead + 1) % APPROACH_LOG_SIZE;
    } else {
        _approachCount++;
    }
    _approachLog[idx] = { ts, dist, move_en, stat_en };
}

void SecurityMonitor::clearApproachLog() {
    _approachHead = 0;
    _approachCount = 0;
}

String SecurityMonitor::formatApproachLog() const {
    if (_approachCount == 0) return "No data";

    String result = "📍 Approach (" + String(_approachCount) + " entries):\n";
    uint32_t firstTs = _approachLog[_approachHead].timestamp_s;
    bool isEpoch = firstTs > 1700000000;

    for (uint8_t i = 0; i < _approachCount; i++) {
        uint8_t idx = (_approachHead + i) % APPROACH_LOG_SIZE;
        const ApproachEntry& e = _approachLog[idx];

        if (isEpoch) {
            // Show HH:MM:SS from epoch
            time_t t = (time_t)e.timestamp_s;
            struct tm tm;
            localtime_r(&t, &tm);
            char timeBuf[9];
            strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tm);
            result += String(timeBuf);
        } else {
            // Show relative seconds from first entry
            result += "T+" + String(e.timestamp_s - firstTs) + "s";
        }
        result += " | " + String(e.distance_cm) + "cm";
        result += " M:" + String(e.move_energy) + " S:" + String(e.static_energy);
        result += "\n";
    }
    return result;
}
