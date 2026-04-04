#include "ConfigManager.h"
#include "debug.h"

ConfigManager::ConfigManager() {}

void ConfigManager::begin() {
    _prefs.begin("radar_config", false);
    load();
}

void ConfigManager::load() {
    loadPref("mqtt_server", _config.mqtt_server, sizeof(_config.mqtt_server));
    loadPref("mqtt_port", _config.mqtt_port, sizeof(_config.mqtt_port));
    loadPref("mqtt_user", _config.mqtt_user, sizeof(_config.mqtt_user));
    loadPref("mqtt_pass", _config.mqtt_pass, sizeof(_config.mqtt_pass));
    loadPref("mqtt_id", _config.mqtt_id, sizeof(_config.mqtt_id));
    loadPref("hostname", _config.hostname, sizeof(_config.hostname));
    loadPref("auth_user", _config.auth_user, sizeof(_config.auth_user));
    loadPref("auth_pass", _config.auth_pass, sizeof(_config.auth_pass));
    loadPref("bk_ssid", _config.backup_ssid, sizeof(_config.backup_ssid));
    loadPref("bk_pass", _config.backup_pass, sizeof(_config.backup_pass));

    _config.mqtt_enabled = _prefs.getBool("mqtt_en", true);
    _config.led_enabled = _prefs.getBool("led_en", true);
    _config.startup_led_sec = _prefs.getUInt("led_start", 120);
    
    if (_prefs.isKey("radar_res")) {
        _config.radar_resolution = _prefs.getFloat("radar_res", 0.75);
    }

    // Timezone
    _config.tz_offset = _prefs.getInt("tz_offset", 3600);
    _config.dst_offset = _prefs.getInt("dst_offset", 3600);

    // Scheduled arm/disarm
    loadPref("sched_arm", _config.sched_arm_time, sizeof(_config.sched_arm_time));
    loadPref("sched_disarm", _config.sched_disarm_time, sizeof(_config.sched_disarm_time));

    // Auto-arm
    _config.auto_arm_minutes = _prefs.getUShort("auto_arm_min", 0);

    // Persist defaults to NVS on first boot (prevents NOT_FOUND errors)
    if (!_prefs.isKey("mqtt_server")) {
        save();
        DBG("CONFIG", "First boot — defaults written to NVS");
    }

    DBG("CONFIG", "Configuration loaded from NVS");
    if (isDefaultAuth()) {
        Serial.println("[SECURITY] WARNING: Default credentials (admin/admin) are in use!");
    }
}

void ConfigManager::save() {
    _prefs.putString("mqtt_server", _config.mqtt_server);
    _prefs.putString("mqtt_port", _config.mqtt_port);
    _prefs.putString("mqtt_user", _config.mqtt_user);
    _prefs.putString("mqtt_pass", _config.mqtt_pass);
    _prefs.putString("mqtt_id", _config.mqtt_id);
    _prefs.putString("hostname", _config.hostname);
    _prefs.putString("auth_user", _config.auth_user);
    _prefs.putString("auth_pass", _config.auth_pass);
    _prefs.putString("bk_ssid", _config.backup_ssid);
    _prefs.putString("bk_pass", _config.backup_pass);

    _prefs.putBool("mqtt_en", _config.mqtt_enabled);
    _prefs.putBool("led_en", _config.led_enabled);
    _prefs.putUInt("led_start", _config.startup_led_sec);
    _prefs.putFloat("radar_res", _config.radar_resolution);

    // Timezone
    _prefs.putInt("tz_offset", _config.tz_offset);
    _prefs.putInt("dst_offset", _config.dst_offset);

    // Scheduled arm/disarm
    _prefs.putString("sched_arm", _config.sched_arm_time);
    _prefs.putString("sched_disarm", _config.sched_disarm_time);

    // Auto-arm
    _prefs.putUShort("auto_arm_min", _config.auto_arm_minutes);

    DBG("CONFIG", "Configuration saved to NVS");
}

void ConfigManager::loadPref(const char* key, char* target, size_t maxLen) {
    String val = _prefs.getString(key, "");
    if (val.length() > 0) {
        val.toCharArray(target, maxLen);
    }
}
