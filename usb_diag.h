#pragma once
#include <Arduino.h>

// ═══════════════════════════════════════════════════════════════════════════════
//  USB enumeration diagnostics
//
//  Populated by the USB callbacks on Core1, displayed on the Input Info page.
//  Lets you see exactly how far a controller gets:
//     mounts=0            → USB host stack isn't running at all
//                           (check Tools > USB Stack = "Adafruit TinyUSB Host")
//     mounts>0, hid=0     → device enumerated but isn't HID (Xbox? CDC?)
//     hid>0, axes=0       → HID mounted but the report descriptor gave nothing
//     axes>0, reports=0   → descriptor parsed but no data arriving
//     reports>0           → data flowing; check channel mapping
// ═══════════════════════════════════════════════════════════════════════════════

struct UsbDiag {
    volatile uint16_t mounts      = 0;   // tuh_mount_cb calls
    volatile uint16_t hid_mounts  = 0;   // tuh_hid_mount_cb calls
    volatile uint16_t cdc_mounts  = 0;   // tuh_cdc_mount_cb calls
    volatile uint32_t reports     = 0;   // HID reports received
    volatile uint32_t xinput_rpts = 0;   // XInput/GIP reports received
    volatile uint16_t last_vid    = 0;
    volatile uint16_t last_pid    = 0;
    volatile uint16_t desc_len    = 0;   // HID descriptor length seen
    volatile uint8_t  map_axes    = 0;   // axes found by the descriptor parser
    volatile uint8_t  map_btns    = 0;   // buttons found
    volatile uint8_t  last_rpt_len= 0;   // length of the most recent report
    volatile bool     xinput_open = 0;   // Xbox IN endpoint opened
    volatile bool     xinput_out  = 0;   // Xbox OUT endpoint opened (LED/rumble)
    volatile uint32_t xfer_fail   = 0;   // interrupt transfers that completed with an error
    volatile uint32_t arm_ok      = 0;   // tuh_edpt_xfer() accepted the request
    volatile uint32_t arm_fail    = 0;   // tuh_edpt_xfer() refused it (never queued)
    volatile uint32_t core1_beat  = 0;   // incremented every loop1() iteration
    volatile uint32_t core1_stall = 0;   // times Core1 stopped responding
    volatile uint32_t hid_stuck   = 0;   // times the HID endpoint had to be recovered
    volatile uint32_t report_age_ms = 0; // time since the newest HID report
    volatile uint32_t aborts        = 0; // stuck transfers aborted and re-issued

    // Copy of the most recent input report, for the raw hex view.
    // Lets you decode an unrecognised device by hand.
    volatile uint8_t  raw[16]     = {};
    volatile uint8_t  raw_len     = 0;

    volatile uint32_t decode_ok   = 0;   // reports successfully decoded
    volatile uint32_t decode_fail = 0;   // reports the descriptor map rejected
    volatile uint8_t  hid_itf     = 0;   // HID interfaces carrying axes
    volatile uint8_t  ps_type     = 0;   // PlayStation pad type, if any
    volatile bool     raw_mode    = 0;   // descriptor bypassed, raw bytes in use
    volatile bool     used_rid    = 0;   // descriptor declares a Report ID
    volatile bool     rid_fallback= 0;   // had to ignore the Report ID to decode
};

extern UsbDiag g_diag;
