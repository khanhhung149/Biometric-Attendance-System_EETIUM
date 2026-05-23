#pragma once

// Shim that provides the SoftwareSerial API but is backed by ESP32's HardwareSerial Serial2.
// The FPM383C library #include <SoftwareSerial.h> and uses serial->begin/read/write etc.
// Bit-banged software serial drops bytes at 57600 baud when WiFi is active on ESP32,
// causing FPM383C protocol frames to be parsed incorrectly. UART2 (pins 16/17) is reliable.

#include <Arduino.h>
#include <HardwareSerial.h>

class SoftwareSerial : public Stream {
private:
  int _rxPin;
  int _txPin;
public:
  SoftwareSerial(int rxPin, int txPin) : _rxPin(rxPin), _txPin(txPin) {}

  void begin(unsigned long baud) {
    Serial2.begin(baud, SERIAL_8N1, _rxPin, _txPin);
  }

  void end() { Serial2.end(); }

  int available() override { return Serial2.available(); }
  int read() override      { return Serial2.read(); }
  int peek() override      { return Serial2.peek(); }
  void flush() override    { Serial2.flush(); }

  size_t write(uint8_t b) override { return Serial2.write(b); }
  using Print::write;

  operator bool() const { return true; }
};
