#pragma once
#include <Arduino.h>
#include "config.h"

// ═══════════════════════════════════════════════════════════════════════════════
//  HID Report Descriptor Parser
//
//  Generic USB gamepads and EdgeTX/OpenTX radios in "USB Joystick" mode all
//  describe their report layout in a HID report descriptor.  Layouts differ
//  wildly (8-bit vs 16-bit axes, signed vs unsigned, different orderings,
//  optional report IDs), so hardcoding byte offsets never works reliably.
//
//  This parser walks the descriptor once at mount time and records, for each
//  analog axis and button, exactly where its bits live and how to scale them.
//  At runtime we then extract and normalise every axis to int16 -32768..32767.
// ═══════════════════════════════════════════════════════════════════════════════

// HID Usage Pages
#define HID_UP_GENERIC_DESKTOP  0x01
#define HID_UP_SIMULATION       0x02
#define HID_UP_BUTTON           0x09

// Generic Desktop usages we treat as analog axes
#define HID_USAGE_X        0x30
#define HID_USAGE_Y        0x31
#define HID_USAGE_Z        0x32
#define HID_USAGE_RX       0x33
#define HID_USAGE_RY       0x34
#define HID_USAGE_RZ       0x35
#define HID_USAGE_SLIDER   0x36
#define HID_USAGE_DIAL     0x37
#define HID_USAGE_WHEEL    0x38
#define HID_USAGE_HATSWITCH 0x39

struct HIDField {
    uint16_t bit_offset;   // bit position within the report (after report-ID byte)
    uint8_t  bit_size;     // bits per value
    int32_t  logical_min;
    int32_t  logical_max;
    uint8_t  report_id;    // 0 = descriptor uses no report IDs
};

// ─── RAW MODE ────────────────────────────────────────────────────────────────
// Fallback used when the report descriptor is missing, too large for the
// enumeration buffer, or simply not understood.
//
// Instead of interpreting the descriptor, every byte of the report is exposed
// as an axis and every bit as a button. Nothing needs to be understood about
// the device: the channel-learning screen already works by watching for
// whichever input moves, so the user just wiggles the control they want and it
// gets bound. Works with any HID device that reports anything at all.
enum class HIDMode : uint8_t { DESCRIPTOR = 0, RAW };

class HIDReportMap {
public:
    HIDMode mode() const { return _mode; }
    void    forceRaw()   { _mode = HIDMode::RAW; }
    // Parse a raw HID report descriptor. Safe to call repeatedly.
    void parse(const uint8_t* desc, uint16_t len);

    // Clear all learned layout (call on unmount)
    void reset();

    // Extract normalised values from an incoming report.
    // Returns true if the report matched a known report ID.
    bool decode(const uint8_t* report, uint16_t len,
                int16_t axes_out[MAX_INPUT_AXES], uint8_t* num_axes_out,
                uint32_t* buttons_out,             uint8_t* num_buttons_out) const;

    uint8_t axisCount()   const { return _num_axes; }
    uint8_t buttonCount() const { return _num_buttons; }
    bool    valid()       const { return _num_axes > 0 || _num_buttons > 0; }
    bool    usesReportID()const { return _uses_report_id; }
    bool    ridFallback() const { return _rid_fallback;    }

private:
    HIDField _axes[MAX_INPUT_AXES]   = {};
    HIDField _buttons[MAX_INPUT_BUTTONS] = {};
    uint8_t  _num_axes    = 0;
    uint8_t  _num_buttons = 0;
    HIDMode  _mode = HIDMode::DESCRIPTOR;
    bool     _uses_report_id = false;
    // Some devices declare a Report ID but then send reports without the ID
    // byte. Set once we detect that and start parsing from offset 0 instead.
    mutable bool _rid_fallback = false;

    bool decodeAt(const uint8_t* data, uint16_t data_len, uint8_t rid,
                  bool match_rid,
                  int16_t axes_out[MAX_INPUT_AXES], uint8_t* num_axes_out,
                  uint32_t* buttons_out, uint8_t* num_buttons_out) const;
};
