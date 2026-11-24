#pragma once
// Non-blocking SSD1306 flush.
//
// Adafruit_SSD1306::display() pushes the whole 1 KB frame in one go: about
// 20-25 ms of blocked CPU at 400 kHz I2C. Redrawn 40 times a second that
// leaves the main loop with almost no time to send CRSF frames on schedule,
// so an ELRS module asking for 4 ms (250 Hz) or 2 ms (500 Hz) frames sees
// bursts and gaps instead - the link connects but is unstable.
//
// TinyOled hides display(): it snapshots the frame, skips it if nothing
// changed since the last one sent, and then pump() (called every loop pass)
// sends one 64-byte slice at a time, about 2 ms each.
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>

class TinyOled : public Adafruit_SSD1306 {
public:
    using Adafruit_SSD1306::Adafruit_SSD1306;

    void setAddr(uint8_t a)  { _addr = a; }
    void setAsync(bool a)    { _async = a; }
    bool busy() const        { return _unit < UNITS; }

    // Hides Adafruit_SSD1306::display() (called through a TinyOled object).
    void display() {
        if (!_async) { Adafruit_SSD1306::display(); return; }
        if (busy()) return;                       // still sending the last frame: drop this one
        uint8_t* b = getBuffer();
        if (_have && memcmp(b, _last, SZ) == 0) return;   // unchanged: nothing to send
        memcpy(_snap, b, SZ);
        memcpy(_last, b, SZ);
        _have = true;
        _unit = 0;
        pump();
    }

    // Send the next slice, if any. Cheap to call when idle.
    void pump() {
        if (!_async || _unit >= UNITS) return;
        uint8_t page = _unit >> 1;
        uint8_t col  = (_unit & 1) ? 64 : 0;
        Wire.beginTransmission(_addr);
        Wire.write((uint8_t)0x00);                // command stream
        Wire.write((uint8_t)0x22); Wire.write(page); Wire.write(page);          // page range
        Wire.write((uint8_t)0x21); Wire.write(col);  Wire.write((uint8_t)(col + 63));  // column range
        Wire.endTransmission();
        const uint8_t* p = _snap + page * 128 + col;
        for (uint8_t c = 0; c < 2; c++) {
            Wire.beginTransmission(_addr);
            Wire.write((uint8_t)0x40);            // data stream
            Wire.write(p + c * 32, (size_t)32);
            Wire.endTransmission();
        }
        _unit++;
    }

    // Blocking: finish whatever is still pending (before a reboot, say).
    void finish() { while (busy()) pump(); }

private:
    static const uint8_t  UNITS = 16;
    static const uint16_t SZ    = 1024;     // 128x64 / 8
    uint8_t  _snap[SZ];
    uint8_t  _last[SZ];
    bool     _have  = false;
    bool     _async = false;
    uint8_t  _unit  = UNITS;
    uint8_t  _addr  = 0x3C;
};
