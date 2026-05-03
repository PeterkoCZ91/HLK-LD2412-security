#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <LittleFS.h>
#include <freertos/semphr.h>

enum EventType {
    EVT_SYSTEM = 0,
    EVT_PRESENCE = 1,
    EVT_TAMPER = 2,
    EVT_WIFI = 3,
    EVT_HEARTBEAT = 4,
    EVT_SECURITY = 5
};

struct LogEvent {
    uint32_t timestamp;      // uptime seconds
    uint8_t type;            // EventType
    uint16_t distance;       // cm
    uint8_t energy;          // max energy
    char message[48];        // Short description
};

class EventLog {
public:
    EventLog(size_t capacity = 20);
    ~EventLog();

    void begin();
    void addEvent(uint8_t type, uint16_t dist, uint8_t energy, const char* msg);
    void flush();
    void flushNow();  // FIX #16: Bypass rate-limit for critical events
    void clear();
    
    // Fill a JsonDocument with events. typeFilter -1 = all, 0-5 = specific EventType.
    // Format: { "total": N, "events": [...] }
    void getEventsJSON(JsonDocument& doc, int typeFilter = -1);

private:
    void loadFromDisk();
    bool saveToDisk(LogEvent* buf, size_t count, size_t head);

    SemaphoreHandle_t _mutex = nullptr;
    LogEvent* _buffer;
    size_t _capacity;
    size_t _head; // Index of the oldest element
    size_t _count;

    bool _dirty;
    bool _fsAvailable = false;  // FIX #14: Track whether filesystem is usable
    uint32_t _sequence = 0;  // Incremented on every addEvent(); flush only clears _dirty if sequence unchanged
    unsigned long _lastFlush;
    const char* _filename = "/events.bin";
};

#endif
