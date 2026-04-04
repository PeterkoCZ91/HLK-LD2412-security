#ifndef MQTT_OFFLINE_BUFFER_H
#define MQTT_OFFLINE_BUFFER_H

#include <Arduino.h>
#include <LittleFS.h>

// Ring buffer for alarm events missed while MQTT was offline.
// Persists to LittleFS so events survive ESP reboots.
// LittleFS must be mounted before begin() is called (EventLog mounts it).

#define MQTT_OFB_CAPACITY   30
#define MQTT_OFB_TOPIC_LEN  64
#define MQTT_OFB_PAYLOAD_LEN 200

struct MQTTBufferedMsg {
    uint32_t timestamp;                  // uptime or epoch at push time
    char topic[MQTT_OFB_TOPIC_LEN];
    char payload[MQTT_OFB_PAYLOAD_LEN];
};  // 268 bytes × 30 slots = ~8 KB on disk

class MQTTOfflineBuffer {
public:
    void begin();
    void push(const char* topic, const char* payload);
    bool hasMessages() const { return _count > 0; }
    bool peek(char* topic, size_t topicLen, char* payload, size_t payloadLen) const;
    void consume();
    uint32_t count() const { return _count; }

private:
    void loadFromDisk();
    void saveToDisk();

    MQTTBufferedMsg _buf[MQTT_OFB_CAPACITY];
    uint32_t _head  = 0;
    uint32_t _count = 0;
    bool _fsAvailable = false;
    const char* _filename = "/mqtt_ofb.bin";
};

#endif
