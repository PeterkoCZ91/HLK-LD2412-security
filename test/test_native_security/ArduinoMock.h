#ifndef ARDUINO_MOCK_H
#define ARDUINO_MOCK_H

// Minimal Arduino + ESP32 mock for native testing of LD2412

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <vector>

// Arduino types
typedef uint8_t byte;

// ESP32 portMUX stubs (no-op on native)
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) (void)(mux)
#define portEXIT_CRITICAL(mux) (void)(mux)

// millis() mock
static unsigned long _mockMillis = 0;
inline unsigned long millis() { return _mockMillis; }
inline void setMockMillis(unsigned long m) { _mockMillis = m; }

// delay() mock (no-op)
inline void delay(unsigned long ms) { _mockMillis += ms; }

// yield() mock
inline void yield() {}

// Stream mock
class Stream {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t*, size_t) = 0;
    virtual void flush() = 0;
    virtual size_t readBytes(uint8_t* buffer, size_t length) {
        size_t count = 0;
        while (count < length && available() > 0) {
            int c = read();
            if (c < 0) break;
            buffer[count++] = (uint8_t)c;
        }
        return count;
    }
    virtual ~Stream() = default;
};

// Serial mock (global)
class MockSerialClass : public Stream {
public:
    std::vector<uint8_t> txBuffer;
    std::vector<uint8_t> rxBuffer;
    size_t rxReadPos = 0;

    void begin(unsigned long baud) { (void)baud; }
    void end() { txBuffer.clear(); rxBuffer.clear(); rxReadPos = 0; }

    int available() override { return rxBuffer.size() - rxReadPos; }

    int read() override {
        if (available() > 0) return rxBuffer[rxReadPos++];
        return -1;
    }

    int peek() override {
        if (available() > 0) return rxBuffer[rxReadPos];
        return -1;
    }

    size_t write(uint8_t c) override { txBuffer.push_back(c); return 1; }
    size_t write(const uint8_t *buf, size_t size) override {
        for (size_t i = 0; i < size; i++) write(buf[i]);
        return size;
    }

    void flush() override {}

    // printf stub for Serial.printf()
    int printf(const char* fmt, ...) { return 0; }
    void println(const char* msg) {}
    void print(const char* msg) {}

    // Test helpers
    void pushToRx(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; i++) rxBuffer.push_back(data[i]);
    }
    void pushToRx(uint8_t byte) { rxBuffer.push_back(byte); }
    void clearAll() { txBuffer.clear(); rxBuffer.clear(); rxReadPos = 0; }
};

// Global Serial instance
static MockSerialClass Serial;

// HardwareSerial alias
typedef MockSerialClass HardwareSerial;

#endif // ARDUINO_MOCK_H
