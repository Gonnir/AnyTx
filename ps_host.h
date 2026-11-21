#pragma once
#include <Arduino.h>

// ═══════════════════════════════════════════════════════════════════════════════
//  PlayStation controllers over USB
//
//  DualShock 4 and DualSense are ordinary HID devices, so they enumerate on
//  their own - but their report descriptors describe a large blob that generic
//  parsing turns into a confusing pile of axes. Decoding the known layouts
//  gives sensible sticks and buttons instead.
//
//  DualShock 3 is different: it enumerates and then stays silent forever until
//  the host sends it a specific feature report. That handshake is the only
//  reason it appears dead otherwise.
// ═══════════════════════════════════════════════════════════════════════════════

#define SONY_VID  0x054C

enum PSType : uint8_t { PS_NONE = 0, PS_DS3, PS_DS4, PS_DS5 };

static inline PSType psIdentify(uint16_t vid, uint16_t pid) {
    if (vid != SONY_VID) return PS_NONE;
    switch (pid) {
        case 0x0268: return PS_DS3;   // DualShock 3
        case 0x05C4:                  // DualShock 4 v1
        case 0x09CC:                  // DualShock 4 v2
        case 0x0BA0: return PS_DS4;   // DS4 USB dongle
        case 0x0CE6:                  // DualSense
        case 0x0DF2: return PS_DS5;   // DualSense Edge
        default:     return PS_NONE;
    }
}

static inline const char* psName(PSType t) {
    switch (t) {
        case PS_DS3: return "DS3";
        case PS_DS4: return "DS4";
        case PS_DS5: return "DS5";
        default:     return "";
    }
}

// The DualShock 3 sends nothing until it receives this feature report. Sony's
// driver issues it right after enumeration; without it the pad is mute.
static const uint8_t DS3_ENABLE[4] = { 0x42, 0x0C, 0x00, 0x00 };
#define DS3_ENABLE_REPORT_ID 0xF4

// ─── Shared button numbering ─────────────────────────────────────────────────
// Kept identical across all three pads so a profile learned on one is not
// nonsense on another.
//   0 Square   1 Cross    2 Circle   3 Triangle
//   4 L1       5 R1       6 L2       7 R2
//   8 Share    9 Options 10 L3      11 R3
//  12 PS      13 Touchpad
//  14 D-Up   15 D-Right 16 D-Down  17 D-Left
#define PS_BTN_COUNT 18

// The D-pad arrives as a direction value, not as four bits. 8 means centred.
static inline uint32_t psHatToButtons(uint8_t hat) {
    static const uint8_t dirs[8] = {
        0x1,        // N
        0x1 | 0x2,  // NE
        0x2,        // E
        0x2 | 0x4,  // SE
        0x4,        // S
        0x4 | 0x8,  // SW
        0x8,        // W
        0x8 | 0x1   // NW
    };
    if (hat > 7) return 0;
    return (uint32_t)dirs[hat] << 14;
}
