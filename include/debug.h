#ifndef DEBUG_H
#define DEBUG_H

#ifndef SERIAL_DEBUG
#define SERIAL_DEBUG 0
#endif

#if SERIAL_DEBUG
  #define DBG(tag, fmt, ...) Serial.printf("[" tag "] " fmt "\n", ##__VA_ARGS__)
#else
  #define DBG(tag, fmt, ...)
#endif

#endif
