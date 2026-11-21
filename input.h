#pragma once
#include <Arduino.h>
#include <SerialPIO.h>
#include "config.h"
#include "hid_parser.h"

// ═══════════════════════════════════════════════════════════════════════════════
//  Unified input model
//
//  Every source — USB gamepad, Xbox controller, EdgeTX radio, SBUS receiver,
//  PPM stream, ELRS receiver — is normalised into:
//      axes[]   : int16, -32768 .. +32767
//      buttons  : 32-bit mask
//  so the channel-learning UI and mixer are identical in every device mode.
//
//  CROSS-CORE SAFETY
//  Core1 (USB callbacks) and Core0 (main loop) share this state. A blocking
//  mutex is unsuitable: TinyUSB callbacks can run in interrupt context, where
//  waiting on a lock held by the other core can stall the USB stack, and at
//  high report rates the contention alone was enough to wedge the firmware.
//
//  Instead the writer publishes through a sequence counter (seqlock):
//     odd  _seq → a write is in progress
//     even _seq → the published copy is stable
//  Readers copy, then re-check the counter, and retry if it moved. Nobody ever
//  blocks, so a deadlock is impossible by construction.
//
//  Only one core ever writes: Core1 in USB_INPUT mode, Core0 in the serial
//  modes. That is enforced by update() returning early in USB mode.
// ═══════════════════════════════════════════════════════════════════════════════

struct InputState {
    int16_t     axes[MAX_INPUT_AXES]        = {};
    uint32_t    buttons                     = 0;
    bool        toggles[MAX_INPUT_BUTTONS]  = {};   // latched (sticky) buttons
    uint8_t     num_axes                    = 0;
    uint8_t     num_buttons                 = 0;
    bool        connected                   = false;
    bool        failsafe                    = false;
    InputSource source                      = InputSource::NONE;
};

// PPM ISR globals
extern volatile uint16_t g_ppm_raw[PPM_MAX_CH];
extern volatile uint8_t  g_ppm_count;
extern volatile bool     g_ppm_updated;
void ppm_isr();

class InputManager {
public:
    void begin(DeviceMode mode);
    void update();     // Core0: serial-input timeouts (no-op in USB mode)
    void pollAux();    // Core0: SBUS / ELRS-RX serial + PPM

    // Lock-free read. Safe from either core, never blocks.
    InputState snapshot() const;

    // ══ Core1 USB callbacks ══════════════════════════════════════════════════
    // These do almost nothing: they copy the raw report into a staging buffer
    // and return. Decoding, mixing and publishing all happen on Core0 in
    // processUSB(). Keeping the USB callbacks short stops a fast-reporting
    // device from starving the USB task, which previously killed the whole
    // host stack under rapid stick movement.
    enum RptType : uint8_t { RPT_NONE = 0, RPT_HID, RPT_XINPUT, RPT_GIP,
                             RPT_DS3, RPT_DS4, RPT_DS5 };

    void submitReport(uint8_t type, const uint8_t* r, uint16_t len);
    void submitCDC(const uint8_t* b, uint32_t n);
    void setPresent(bool p);          // records a request; applied on Core0
    void notifyDisconnect() { setPresent(false); }

    // ══ Core0 ════════════════════════════════════════════════════════════════
    void processUSB();                // decode whatever Core1 staged

    // millis() when the device was last present. Lets the caller tell a brief
    // USB glitch apart from a deliberate unplug.
    uint32_t lastPresentMs() const { return _last_present_ms; }

    void setHIDMap(const HIDReportMap* map) { _hid_map = map; }

    // Calibration is applied when publishing, so _work keeps raw values.
    // Bypass feeds raw axes straight through, which the calibration wizard
    // needs in order to observe the true travel.
    void setCalibration(const AxisCal* cal) { _cal = cal; }
    void setCalBypass(bool b)               { _cal_bypass = b; }

    // Bypass the report descriptor and read the report as plain bytes:
    // byte N -> axis N, bit N -> button N. Takes effect on the very next
    // report, so the menu toggle can be judged instantly rather than needing
    // the device to be unplugged and back in.
    void setForceRaw(bool b)                { _force_raw = b; }

private:
    DeviceMode _mode = DeviceMode::USB_INPUT;

    InputState        _work;        // writer-owned working copy
    InputState        _pub;         // published copy
    volatile uint32_t _seq  = 0;    // seqlock counter

    uint32_t _last_recv    = 0;
    uint32_t _prev_buttons = 0;
    bool     _present      = false;

    const HIDReportMap* _hid_map = nullptr;

    // ── Core1 -> Core0 handoff ───────────────────────────────────────────────
    // Guarded by a sequence counter: odd while Core1 is writing. Core0 skips a
    // report if it was torn mid-copy; the next one arrives milliseconds later.
    volatile uint8_t  _stage[64]  = {};
    volatile uint8_t  _stage_len  = 0;
    volatile uint8_t  _stage_type = RPT_NONE;
    volatile uint32_t _stage_seq  = 0;
    uint32_t          _stage_seen = 0;

    // CDC arrives as a byte stream, so it gets a small ring buffer.
    volatile uint8_t  _cdc_ring[256] = {};
    volatile uint16_t _cdc_head = 0, _cdc_tail = 0;

    volatile int8_t   _present_req = -1;   // -1 none, 0 absent, 1 present

    // Sticky switch states are kept across a short dropout so a momentary USB
    // glitch does not silently disarm the model. A genuine unplug (longer than
    // STICKY_KEEP_MS) still clears them, which is the safe behaviour.
    bool     _tgl_saved[MAX_INPUT_BUTTONS] = {};
    uint32_t _absent_at       = 0;
    uint32_t _last_present_ms = 0;
    static const uint32_t STICKY_KEEP_MS = 3000;

    void parseHIDReport(const uint8_t* r, uint16_t len);
    void parseXInput   (const uint8_t* r, uint16_t len);
    void parseGIP      (const uint8_t* r, uint16_t len);
    void parsePS       (const uint8_t* r, uint16_t len, uint8_t type);
    void parseCDCByte  (uint8_t b);
    const AxisCal*      _cal        = nullptr;
    bool                _cal_bypass = false;
    bool                _force_raw  = false;

    uint8_t _sbus_buf[SBUS_FRAME_LEN] = {}; uint8_t _sbus_len = 0;
    uint8_t _crsf_buf[64] = {};             uint8_t _crsf_len = 0;
    uint8_t _cdc_buf[64]  = {};             uint8_t _cdc_len  = 0;

    // ── Pot/Btn UART ─────────────────────────────────────────────────────────
    // SYNC1 SYNC2 NUM_POTS NUM_BUTTONS, then NUM_POTS x uint16 LE pot values,
    // then ceil(NUM_BUTTONS/8) packed button bytes, then an 8-bit sum
    // checksum. See README.txt. _pb_buf holds everything from NUM_POTS
    // onward (i.e. excluding the two sync bytes, which are handled by
    // _pb_sync directly), since the checksum is defined over exactly that
    // range and it saves re-deriving the offset later.
    uint8_t  _pb_buf[2 + MAX_INPUT_AXES * 2 + (MAX_INPUT_BUTTONS / 8) + 1] = {};
    uint8_t  _pb_len  = 0;    // bytes collected into _pb_buf so far
    uint8_t  _pb_sync = 0;    // 0 = need SYNC1, 1 = need SYNC2, 2 = collecting body
    uint16_t _pb_need = 0;    // total body length once the header is known (0 = not yet known)

    SerialPIO* _aux = nullptr;

    void publish();                        // copy _work -> _pub via seqlock
    void parseSBUSByte(uint8_t b);
    void parseCRSFRxByte(uint8_t b);
    void parsePotBtnByte(uint8_t b);
    void storeCRSFChannels(const uint8_t* payload, InputSource src);
    void edges(uint32_t nb);               // sticky-button latching
};

// ─── CHANNEL MIXER ───────────────────────────────────────────────────────────
class ChannelMixer {
public:
    void mix(const ChannelConfig cfg[CRSF_NUM_CHANNELS],
             const InputState&   in,
             uint16_t            out[CRSF_NUM_CHANNELS]);

    // True while the input link is considered lost
    bool inFailsafe() const { return _fs; }

    // Act on the trim buttons. Separate from mix() because it writes back to
    // the configuration, and only on the press edge - holding a button must
    // not run the trim away.
    // Returns true when a value changed, so the caller can schedule a save.
    bool applyTrimButtons(ChannelConfig cfg[CRSF_NUM_CHANNELS], const InputState& in);

private:
    // Last values produced with a healthy link, used by HOLD failsafe.
    uint16_t _last[CRSF_NUM_CHANNELS] = {};
    bool     _fs        = false;
    bool     _have      = false;
    bool     _seen_link = false;   // a good link has existed at least once
    uint32_t _trim_prev = 0;       // button state at the last trim check
    // Held-button auto-repeat for trim buttons (EdgeTX-style): one step on
    // press, then repeats after a delay, faster the longer it is held, and
    // the repeat halts at centre until the button is released and pressed.
    uint32_t _trim_next[MAX_INPUT_BUTTONS] = {};   // when the next repeat is due
    uint8_t  _trim_reps[MAX_INPUT_BUTTONS] = {};   // repeats so far this hold
    uint32_t _trim_stopped = 0;                    // bit per button: parked at centre
};
