#include "services/EventLog.h"
#include "debug.h"
#include <time.h>
#include <new>

EventLog::EventLog(size_t capacity) : _capacity(capacity), _head(0), _count(0), _dirty(false), _lastFlush(0) {
    _buffer = new LogEvent[_capacity];
    _mutex = xSemaphoreCreateMutex();
}

EventLog::~EventLog() {
    delete[] _buffer;
}

void EventLog::begin() {
    // FIX #14: Try mount without auto-format first to preserve forensic history
    if (!LittleFS.begin(false)) {
        Serial.println("[EventLog] LittleFS mount failed — attempting format as last resort");
        if (!LittleFS.begin(true)) {
            Serial.println("[EventLog] LittleFS format+mount FAILED — event persistence disabled");
            _fsAvailable = false;
            return;
        }
        Serial.println("[EventLog] WARNING: LittleFS was formatted — previous event history LOST");
    }
    _fsAvailable = true;
    loadFromDisk();
}

void EventLog::addEvent(uint8_t type, uint16_t dist, uint8_t energy, const char* msg) {
    LogEvent evt;
    time_t epoch = time(nullptr);
    evt.timestamp = (epoch > 1700000000) ? (uint32_t)epoch : millis() / 1000;
    evt.type = type;
    evt.distance = dist;
    evt.energy = energy;
    strncpy(evt.message, msg, sizeof(evt.message) - 1);
    evt.message[sizeof(evt.message) - 1] = '\0';

    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    // Ring buffer logic
    size_t index = (_head + _count) % _capacity;

    if (_count < _capacity) {
        // Buffer not full, append
        _buffer[index] = evt;
        _count++;
    } else {
        // Buffer full, overwrite oldest (head)
        _buffer[_head] = evt;
        _head = (_head + 1) % _capacity;
    }

    _dirty = true;
    _sequence++;

    if (_mutex) xSemaphoreGive(_mutex);
    DBG("EventLog", "Added: %s", msg);
}

// FIX #16: Immediate flush bypassing rate-limit (for critical security events)
void EventLog::flushNow() {
    if (!_dirty || !_fsAvailable) return;

    LogEvent* snapshot = new(std::nothrow) LogEvent[_capacity];
    if (!snapshot) {
        DBG("EventLog", "CRIT: alloc failed heap=%u maxAlloc=%u",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        return;
    }
    size_t count, head;
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        memcpy(snapshot, _buffer, _capacity * sizeof(LogEvent));
        count = _count;
        head = _head;
        xSemaphoreGive(_mutex);
    } else {
        delete[] snapshot;
        return;
    }
    uint32_t snapSeq = 0;
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        snapSeq = _sequence;
        xSemaphoreGive(_mutex);
    }
    bool ok = saveToDisk(snapshot, count, head);
    delete[] snapshot;
    if (ok) {
        _lastFlush = millis();
        if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
            if (_sequence == snapSeq) _dirty = false;
            xSemaphoreGive(_mutex);
        }
    }
}

void EventLog::flush() {
    unsigned long now = millis();
    // Rate-limit: never flush more than once per 60s
    if (now - _lastFlush < 60000) return;
    if (!_dirty || !_fsAvailable) return;

    // Snapshot under mutex to avoid data race with addEvent()
    LogEvent* snapshot = new(std::nothrow) LogEvent[_capacity];
    if (!snapshot) {
        DBG("EventLog", "CRIT: alloc failed heap=%u maxAlloc=%u",
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        return;
    }
    size_t count, head;
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        memcpy(snapshot, _buffer, _capacity * sizeof(LogEvent));
        count = _count;
        head = _head;
        xSemaphoreGive(_mutex);
    } else {
        delete[] snapshot;
        return;
    }

    uint32_t snapSeq = 0;
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        snapSeq = _sequence;
        xSemaphoreGive(_mutex);
    }

    bool ok = saveToDisk(snapshot, count, head);
    delete[] snapshot;

    if (ok) {
        _lastFlush = now;
        if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
            // Only clear dirty if no new events were added during the save
            if (_sequence == snapSeq) {
                _dirty = false;
            }
            xSemaphoreGive(_mutex);
        }
    }
}

void EventLog::clear() {
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    _head = 0;
    _count = 0;
    _dirty = true;
    if (_mutex) xSemaphoreGive(_mutex);
    LittleFS.remove(_filename);
    DBG("EventLog", "Cleared");
}

void EventLog::getEventsJSON(JsonDocument& doc, int typeFilter) {
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    JsonObject root = doc.to<JsonObject>();
    JsonArray arr = root["events"].to<JsonArray>();
    int total = 0;

    for (size_t i = 0; i < _count; i++) {
        size_t logical_idx = _count - 1 - i;
        size_t physical_idx = (_head + logical_idx) % _capacity;
        LogEvent& e = _buffer[physical_idx];

        if (typeFilter >= 0 && e.type != (uint8_t)typeFilter) continue;
        total++;

        JsonObject obj = arr.add<JsonObject>();
        obj["ts"] = e.timestamp;
        obj["type"] = e.type;
        obj["dist"] = e.distance;
        obj["en"] = e.energy;
        obj["msg"] = e.message;
    }
    root["total"] = total;

    if (_mutex) xSemaphoreGive(_mutex);
}

void EventLog::loadFromDisk() {
    if (!LittleFS.exists(_filename)) return;

    File f = LittleFS.open(_filename, "r");
    if (!f) return;

    // Read header (count)
    size_t storedCount = 0;
    if (f.read((uint8_t*)&storedCount, sizeof(storedCount)) == sizeof(storedCount)) {
        if (storedCount > _capacity) storedCount = _capacity; // Sanity check
        
        // Read events directly into buffer
        // Note: This simple load assumes we fill from 0
        size_t bytesToRead = storedCount * sizeof(LogEvent);
        if (f.read((uint8_t*)_buffer, bytesToRead) == bytesToRead) {
            _count = storedCount;
            _head = 0; // Reset head to 0, data is contiguous
            DBG("EventLog", "Loaded %d events from disk", _count);
        }
    }
    f.close();
}

bool EventLog::saveToDisk(LogEvent* buf, size_t count, size_t head) {
    File f = LittleFS.open(_filename, "w");
    if (!f) {
        Serial.println("[EventLog] Failed to open file for writing"); // Keep as critical
        return false;
    }

    // Write count
    f.write((uint8_t*)&count, sizeof(count));

    // Write events in chronological order (oldest to newest)
    for (size_t i = 0; i < count; i++) {
        size_t idx = (head + i) % _capacity;
        f.write((uint8_t*)&buf[idx], sizeof(LogEvent));
    }

    f.close();
    DBG("EventLog", "Saved to disk");
    return true;
}
