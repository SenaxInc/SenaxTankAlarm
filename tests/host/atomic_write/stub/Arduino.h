// The little of Arduino.h that TankAlarm_Platform.h uses, for the host tests: F(), and a Serial
// that keeps what is printed.
#ifndef ATOMIC_WRITE_TEST_ARDUINO_H
#define ATOMIC_WRITE_TEST_ARDUINO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

class __FlashStringHelper;
#define F(text) (reinterpret_cast<const __FlashStringHelper *>(text))

struct HostSerial {
  std::string printed;
  void print(const char *text) { printed += text; }
  void print(const __FlashStringHelper *text) { printed += reinterpret_cast<const char *>(text); }
  void println(const char *text) {
    print(text);
    printed += '\n';
  }
  void println(const __FlashStringHelper *text) {
    print(text);
    printed += '\n';
  }
};

static HostSerial Serial;

#endif
