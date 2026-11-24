#pragma once
#include <Arduino.h>

// ═══════════════════════════════════════════════════════════════════════════════
//  AnyTx – global configuration
// ═══════════════════════════════════════════════════════════════════════════════

// ─── PINS (Waveshare RP2040 Zero) ────────────────────────────────────────────
// Four-button UI: UP / DOWN / OK (menu, select, confirm) / BACK (cancel,
// back out). UP/DOWN stay on GP6/7; OK/BACK moved off GP8/9 to make room for
// the DFPlayer's real UART1 pins there (see below), landing on GP10/GP11
// (plain GPIOs, no UART/I2C alt-function of their own - nothing else here
// uses them).
#define PIN_I2C_SDA      4
#define PIN_I2C_SCL      5
#define PIN_BTN_UP       6
#define PIN_BTN_DOWN     7
#define PIN_BTN_OK       10
#define PIN_BTN_BACK     11

// DFPlayer voice module - same wiring convention as the OpenI6X firmware's
// DFPlayer mod: a one-way command link plus the module's BUSY output read
// back on a plain GPIO pin. See sound.h.
//
// This uses Serial2, which arduino-pico ties to the RP2040's hardware UART1.
// The real RP2040 GPIO/UART alt-function table repeats every 16 GPIOs, not
// 8 (an earlier version of this comment/pin choice got that wrong):
//     GP0-3   UART0        GP12-15  UART0
//     GP4-11  UART1        GP16-19  UART0
//     (8 pins)             GP20-27  UART1 (8 pins)
//                           GP28-29  UART0
// so the UART1-capable TX/RX pairs are only GP4/5, GP8/9, GP20/21 and
// GP24/25 - GP12/GP13, for instance, are UART0 pins, not UART1, and
// assigning them to Serial2 hangs Serial2.begin() outright rather than
// failing quietly. GP4/5 are the I2C bus, so GP8/GP9 is the pair used here.
#define PIN_DFPLAYER_TX  8     // MCU UART TX  -> DFPlayer RX
#define PIN_DFPLAYER_RX  9     // unused - DFPlayer never talks back on this link
#define PIN_DFPLAYER_BUSY 3    // DFPlayer BUSY -> MCU input, LOW while playing
// Track number of the spoken "0" on the SD card. The number files are copied
// in order 0, 1, 2 ... 100 (then 200 ... 1000), so spoken number N is track
// SPOKEN_NUMBER_BASE + N. Used by the timer minute call.
#define SPOKEN_NUMBER_BASE 1
#define DFPLAYER_BAUD  9600

#define PIN_CRSF_TX      0    // UART0 TX → ELRS module (hardware inverted)
#define PIN_CRSF_RX      1    // UART0 RX ← ELRS module (hardware inverted)

#define PIN_AUX_RX      13    // SBUS (inverted) or ELRS-RX CRSF (normal)
#define PIN_PPM_IN      14    // PPM rising-edge interrupt

#define PIN_LED         16    // WS2812 NeoPixel
#define LED_BRIGHT      30

// USB host runs on the NATIVE USB hardware (rhport 0) = the onboard USB-C port.

// ─── GENERIC INPUT MODEL ─────────────────────────────────────────────────────
// Every input source (gamepad, EdgeTX radio, SBUS, PPM, CRSF) is normalised
// into the same shape: N analog axes + a 32-bit button mask.  This makes the
// channel-learning UI identical in every device mode.
#define MAX_INPUT_AXES     16
#define MAX_INPUT_BUTTONS  32

// ─── CRSF ────────────────────────────────────────────────────────────────────
#define CRSF_NUM_CHANNELS   16
#define CRSF_BAUDRATE       420000
#define CRSF_CHANNEL_MIN    172
#define CRSF_CHANNEL_MID    992
#define CRSF_CHANNEL_MAX    1811

// ─── SBUS ────────────────────────────────────────────────────────────────────
#define SBUS_BAUDRATE       100000
#define SBUS_FRAME_LEN      25
#define SBUS_HEADER         0x0F
#define SBUS_FOOTER         0x00
#define SBUS_FLAG_FAILSAFE  (1 << 3)
#define SBUS_FLAG_LOST      (1 << 2)

// ─── PPM ─────────────────────────────────────────────────────────────────────
#define PPM_MAX_CH      16
#define PPM_SYNC_US     3500u
#define PPM_MIN_US       700u
#define PPM_MAX_US      2300u

// ─── POT/BUTTON UART ─────────────────────────────────────────────────────────
// Matches the "rp2040_zero_pot_button_uart_binary" sender: a self-describing
// binary packet (pot/button counts are in the packet itself), little-endian
// uint16 pot values, packed button bits, 8-bit sum checksum. See README.txt.
#define POT_BTN_BAUDRATE    115200
#define POT_BTN_SYNC1       0xAA
#define POT_BTN_SYNC2       0x55

// ─── PPM OUTPUT (trainer port) ───────────────────────────────────────────────
#define PPM_OUT_MAX_CH  16
#define PIN_PPM_OUT     PIN_CRSF_TX     // GP0, shared with the CRSF line

// What comes out of GP0.
enum class OutputMode : uint8_t {
    CRSF = 0,   // ExpressLRS / CRSF module
    PPM         // PPM sum signal for another radio's trainer port
};

static inline const char* outputModeName(OutputMode m) {
    return (m == OutputMode::CRSF) ? "CRSF" : "PPM";
}

struct PPMConfig {
    uint8_t  channels = 8;        // 2..16
    uint16_t frame_us = 22500;    // whole frame length
    uint16_t pulse_us = 300;      // width of each separator pulse
    bool     invert   = false;    // false = positive pulses, true = negative
};

// ─── DISPLAY ─────────────────────────────────────────────────────────────────
#define OLED_ADDR   0x3C
#define OLED_WIDTH  128
#define OLED_HEIGHT  64

// ─── DEVICE MODES ────────────────────────────────────────────────────────────
enum class DeviceMode : uint8_t {
    USB_INPUT = 0,     // Any USB controller: gamepad, Xbox, EdgeTX radio, T8L
    SBUS_INPUT,        // SBUS receiver on PIN_AUX_RX
    PPM_INPUT,         // PPM stream on PIN_PPM_IN
    ELRS_REPEATER,     // CRSF RC-channels frames on PIN_AUX_RX (a second UART,
                        // separate from the CRSF link on GP0/GP1). Works with
                        // an ELRS receiver, but any sender that emits valid
                        // CRSF frames - including a second microcontroller -
                        // is read exactly the same way.
    POT_BTN_UART,       // Self-describing binary pot/button packet on
                        // PIN_AUX_RX - see README.txt for the exact format.
    _COUNT
};

static inline const char* deviceModeName(DeviceMode m) {
    switch (m) {
        case DeviceMode::USB_INPUT:     return "USB Input";
        case DeviceMode::SBUS_INPUT:    return "SBUS In";
        case DeviceMode::PPM_INPUT:     return "PPM In";
        case DeviceMode::ELRS_REPEATER: return "CRSF Aux In";
        case DeviceMode::POT_BTN_UART:  return "Pot/Btn UART";
        default:                        return "?";
    }
}

// ─── DETECTED INPUT SOURCE (runtime, not saved) ──────────────────────────────
enum class InputSource : uint8_t {
    NONE = 0,
    HID_GAMEPAD,     // generic HID (incl. EdgeTX radios, T8L joystick mode)
    XINPUT,          // Xbox 360 / XInput vendor class
    XBOX_GIP,        // Xbox One / Series
    CDC_CRSF,        // USB CDC CRSF (T8L serial mode)
    SBUS,
    PPM,
    ELRS_RX,
    POT_BTN,         // rp2040_zero_pot_button_uart_binary sender (or compatible)
};

static inline const char* inputSourceName(InputSource s) {
    switch (s) {
        case InputSource::HID_GAMEPAD: return "HID";
        case InputSource::XINPUT:      return "XBox";
        case InputSource::XBOX_GIP:    return "XB1";
        case InputSource::CDC_CRSF:    return "CRSF";
        case InputSource::SBUS:        return "SBUS";
        case InputSource::PPM:         return "PPM";
        case InputSource::ELRS_RX:     return "RX";
        case InputSource::POT_BTN:     return "Pot/Bt";
        default:                       return "--";
    }
}

// ─── CHANNEL MIXER ───────────────────────────────────────────────────────────
//
//  Each output channel starts at a resting position (base) and every mix slot
//  adds to it:
//
//    AXIS   slot: contribution = axis_value * weight/100
//                 a negative weight reverses that axis's influence
//    BUTTON slot: contribution = (target - base) * weight/100  while active
//
//  So "rests at centre, one button drives it low, another drives it high" is
//  base = MID with two button slots targeting LOW and HIGH.  Two axes feeding
//  one channel is simply two AXIS slots, each reversible on its own.
//
// CHANNEL is only ever used as a Special Function trigger (a channel's output
// value, trig_index = 0-based channel, trig_threshold = percent -100..100).
enum class SourceType : uint8_t { NONE = 0, AXIS, BUTTON, CHANNEL };
enum class ButtonMode : uint8_t { MOMENTARY = 0, STICKY = 1 };
enum class MixTarget  : uint8_t { LOW = 0, MID, HIGH };

#define MAX_MIX_SOURCES 3

static inline const char* mixTargetName(MixTarget t) {
    switch (t) { case MixTarget::LOW: return "LOW";
                 case MixTarget::MID: return "MID";
                 default:             return "HIGH"; }
}

// Resting / target value in normalised axis units
static inline int32_t mixTargetValue(MixTarget t) {
    switch (t) { case MixTarget::LOW: return -32768;
                 case MixTarget::MID: return 0;
                 default:             return  32767; }
}

struct MixSource {
    SourceType type     = SourceType::NONE;
    uint8_t    index    = 0;                    // axis or button number
    int8_t     weight   = 100;                  // -100..+100 (sign = reverse)
    MixTarget  target   = MixTarget::HIGH;      // buttons: where it pushes
    ButtonMode btn_mode = ButtonMode::MOMENTARY;
};

// What a channel does when the input link is lost
#define TRIM_BTN_NONE 0xFF

enum class FailsafeMode : uint8_t {
    HOLD = 0,   // freeze at the last good value
    VALUE       // jump to a preset position
};

static inline const char* failsafeModeName(FailsafeMode m) {
    return (m == FailsafeMode::HOLD) ? "Hold" : "Value";
}

// What the whole link does when the input goes away.
//
//   CHANNELS  keep streaming CRSF using each channel's failsafe setting.
//             The receiver still sees a good link, so the flight controller's
//             own failsafe will NOT fire.
//   CUT_TX    stop sending RC frames altogether, exactly like switching a
//             normal radio off. The receiver detects signal loss and applies
//             whatever failsafe it is configured for (usually disarm).
enum class LinkLossAction : uint8_t { CHANNELS = 0, CUT_TX };

static inline const char* linkLossName(LinkLossAction a) {
    return (a == LinkLossAction::CHANNELS) ? "Channels" : "Cut TX";
}

struct ChannelConfig {
    MixTarget    base    = MixTarget::MID;      // value with nothing active
    MixSource    src[MAX_MIX_SOURCES];

    // Applied after mixing, in this order: expo shapes the curve, then weight
    // scales the travel.
    int8_t       expo    = 0;                   // -100..+100
                                                //  >0 softer around centre
                                                //  <0 sharper around centre

    // Optional 5-point curve, used instead of expo when enabled. Points sit at
    // input -100 / -50 / 0 / +50 / +100 percent and the output is linearly
    // interpolated between them, so any shape can be drawn by hand.
    bool         curve_on = false;
    int8_t       curve[5] = { -100, -50, 0, 50, 100 };   // linear by default
    int8_t       weight  = 100;                 // -125..+125 output scale,
                                                // negative reverses the channel
    int8_t       trim    = 0;                   // -100..+100, shifts the centre
                                                // by that % of full travel.
                                                // Applied after weight so the
                                                // offset is absolute.

    FailsafeMode fs_mode  = FailsafeMode::HOLD;
    int8_t       fs_value = 0;                  // percent, -100 = low, 0 = mid

    // Trim buttons: nudge this channel's trim by trim_step on each press, the
    // way the trim rockers on a normal transmitter do. 0xFF means unassigned.
    // Appended here deliberately - see the persistence note below.
    uint8_t      trim_up   = TRIM_BTN_NONE;
    uint8_t      trim_dn   = TRIM_BTN_NONE;
    uint8_t      trim_step = 1;                 // percent per press, 1..25
};

// ─── AXIS CALIBRATION ────────────────────────────────────────────────────────
// Sticks rarely reach the extremes their HID descriptor claims, and pots drift
// off centre with age. Calibration records the travel actually achievable and
// rescales to full range; deadband ignores jitter around the centre.
struct AxisCal {
    int16_t min      = -32768;
    int16_t center   = 0;
    int16_t max      =  32767;
    uint8_t deadband = 0;        // percent of full travel, 0..25
};

// ─── ELRS SETTINGS ───────────────────────────────────────────────────────────
struct ELRSConfig {
    uint8_t rate_idx   = 5;   // 250 Hz
    uint8_t power_idx  = 1;   // 25 mW
    uint8_t ratio_idx  = 3;   // 1:16
    uint8_t dynpwr_idx = 0;   // Off
};

// ─── DFPLAYER VOICE ──────────────────────────────────────────────────────────
// Every event AnyTx can announce, in a fixed order shared between the
// config struct, the defaults below and the menu that edits them.
enum SoundEvent : uint8_t {
    SE_STARTUP = 0, SE_LINK_UP, SE_LINK_LOST, SE_THR_WARN,
    SE_SIG_LOW, SE_SIG_CRIT, SE_TRIM_MAX, SE_TRIM_MIN,
    SE_TRIM_CENTER, SE_INACTIVITY,
    SE_MIN_CALL,     // timer minute call: the "minute" word (count is 1)
    SE_MINS_CALL,    // timer minute call: the "minutes" word (count != 1)
    SE_TIMER_OVER,   // a countdown timer reaching zero
    SOUND_EVENT_COUNT
};

// Track numbers are exactly the OpenI6X SD-card convention: the Nth file by
// copy order, 1-based, matching the numeric prefix in the filename. Every
// one of these defaults to a real file in OpenI6X's stock English voice
// pack - confirmed against the actual file listing, not guessed - so none
// of them need a custom-recorded clip out of the box. Every default below
// is editable at runtime from Menu > Sound Setup > Events; this is only
// the starting point new settings are pre-populated with. See sound.h and
// README.txt.
static const char* const SOUND_EVENT_LABEL[SOUND_EVENT_COUNT] = {
    "Startup", "Link up", "Link lost", "Thr warn",
    "Sig low", "Sig crit", "Trim max", "Trim min",
    "Trim ctr", "Inactive", "Minute", "Minutes", "Timer over",
};
static const uint16_t SOUND_EVENT_DEFAULT[SOUND_EVENT_COUNT] = {
    114,   // Startup     - "hello"
    115,   // Link up     - "telemok"
    116,   // Link lost   - "telemko"
    117,   // Thr warn    - "thralert"
    118,   // Sig low     - "siglow"
    119,   // Sig crit    - "sigcrt"
    120,   // Trim max    - "maxtrim"
    121,   // Trim min    - "mintrim"
    122,   // Trim center - "midtrim"
    123,   // Inactive    - "inactiv"
    111,   // Minute      - "minute" (singular)
    112,   // Minutes     - "minutes" (plural)
    113,   // Timer over  - "timovr1"
};

// Fires on any channel's trim reaching +100/-100/0, from any cause -
// the trim buttons or manually editing Trim in the menu. Which channel
// isn't announced, only that it happened, same as a real radio's trim
// beep. See UIEngine::update()'s trim-watching block.
//
// Fires once after no button press or stick/axis movement for the
// configured number of minutes, then again every further interval of the
// same length for as long as nothing moves - a real radio's inactivity
// alarm, not just a one-shot reminder.
struct InactivityConfig {
    bool    enabled = false;
    uint8_t minutes = 5;   // 1..99
};

// A sound that plays when a mix source (a controller button, or an axis
// crossing a threshold) fires, independent of anything the channel it might
// also be mixed into does. Track 0 means the slot is unassigned.
#define MAX_SOUND_TRIGGERS 6

struct SoundTrigger {
    SourceType type      = SourceType::NONE;
    uint8_t    index     = 0;
    int16_t    threshold = 0;      // AXIS only: raw crossing point, -32768..32767
    bool       rising    = true;   // AXIS only: fires crossing upward (true) or downward
    uint16_t   track     = 0;      // 0 = unassigned
};

struct SoundConfig {
    bool         enabled = true;
    uint8_t      volume  = 20;    // DFPlayer's native range is 0..30
    uint16_t     event_track[SOUND_EVENT_COUNT] = {
        114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 111, 112, 113
    };
    SoundTrigger triggers[MAX_SOUND_TRIGGERS];
};

// ─── TIMERS ───────────────────────────────────────────────────────────────────
// Plain timer objects - just what kind they are and, for a countdown, what
// they start at. They don't have a trigger of their own; they're controlled
// by Special Function rows below (Start/Stop/Reset/Set Timer), the same way
// EdgeTX splits "Timers" from "Special Functions" into two lists. Any timer
// that exists shows up on the main Status screen (UP/DOWN cycle between the
// channel bars and each defined timer there).
enum class TimerKind : uint8_t { COUNTDOWN = 0, COUNTUP = 1 };

#define MAX_TIMERS 4

struct TimerDef {
    bool      enabled = false;   // false = empty slot
    TimerKind kind     = TimerKind::COUNTDOWN;
    uint16_t  start_s  = 300;    // countdown only, up to 99:59 (5999s)
};

// ─── SPECIAL FUNCTIONS ────────────────────────────────────────────────────────
// An EdgeTX-style flat list. Each row reads as one sentence:
//     Trigger, Active|Inactive, Function, value(s)
// e.g. "Button 1, Active, Play Sound, 217" or "Button 2, Inactive, Reset
// Timer, 1". Creating an item means picking an empty slot ("+ Add New" on
// the list finds one for you); deleting resets it back to SpecialFunction{}.
//
// "Active" / "Inactive" is which state of the trigger fires this row - a
// button is Active while held, Inactive while released; an axis is Active
// on whichever side of its threshold trig_rising points at, Inactive on the
// other. The same physical switch can drive two different rows, one firing
// on each state, exactly like "button 1 active -> play 217" paired with
// "button 1 inactive -> play 220" for an armed/disarmed pair of call-outs.
//
// Override Channel applies continuously for as long as the row's condition
// holds, same as EdgeTX. Every other function type fires once, the moment
// the condition starts holding (a press-and-hold doesn't repeat it).
enum class SFType : uint8_t {
    NONE = 0,
    OVERRIDE_CHANNEL,   // forces a channel's output while the condition holds
    PLAY_SOUND,         // plays a track once
    TIMER_SET,          // sets a timer's current value directly (does not
                         // touch its configured Timers-list starting value)
    TIMER_START,
    TIMER_STOP,
    TIMER_RESET,        // back to its configured start (0 for count-up)
    VOLUME,             // continuously ties voice volume to an axis position
    TIMER_RUN,          // level-based: the timer runs while the condition holds,
                         // stops when it doesn't (appended - values are persisted)
    _COUNT
};

static inline const char* sfTypeName(SFType t) {
    switch (t) {
        case SFType::OVERRIDE_CHANNEL: return "Override Ch";
        case SFType::PLAY_SOUND:       return "Play Sound";
        case SFType::TIMER_SET:        return "Set Timer";
        case SFType::TIMER_START:      return "Start Timer";
        case SFType::TIMER_STOP:       return "Stop Timer";
        case SFType::TIMER_RESET:      return "Reset Timer";
        case SFType::VOLUME:           return "Volume";
        case SFType::TIMER_RUN:        return "Run Timer";
        default:                       return "(pick one)";
    }
}

#define MAX_SPECIAL_FUNCS 12

struct SpecialFunction {
    SFType     type          = SFType::NONE;   // NONE = empty slot

    SourceType trig_type      = SourceType::NONE;
    uint8_t    trig_index     = 0;
    int16_t    trig_threshold = 0;    // AXIS only
    bool       trig_rising    = true; // AXIS only: which side of the threshold is "Active"
    bool       fire_active    = true; // fires when the trigger is Active (true) or Inactive (false)

    uint8_t    override_ch   = 0;     // OVERRIDE_CHANNEL: 0-based channel
    int8_t     override_pct  = 0;     // OVERRIDE_CHANNEL: -100..100
    uint16_t   sound_track   = 0;     // PLAY_SOUND: track number, 0 = unassigned
    uint8_t    timer_idx     = 0;     // TIMER_*: which timers[] slot, 0-based
    uint16_t   timer_set_s   = 330;   // TIMER_SET: value to assign, seconds
    uint8_t    vol_min       = 0;     // VOLUME: volume at the trigger axis's minimum, 0..30
    uint8_t    vol_max       = 30;    // VOLUME: volume at the trigger axis's maximum, 0..30
};

// ─── PERSISTED CONFIG ────────────────────────────────────────────────────────
// ─── PERSISTENCE ─────────────────────────────────────────────────────────────
//  Settings live in /settings.bin on the flash filesystem and survive a
//  firmware update. Loading is size-tolerant (see storage.h), so:
//
//      ONLY EVER APPEND FIELDS to AppConfig, ChannelConfig and DeviceProfile.
//      Never reorder, remove, or change the type of an existing field.
//
//  Follow that and the magic below never needs to change again - which is the
//  whole point, because bumping it wipes every saved setting and profile.
//  CONFIG_VERSION is informational only; it is not checked when loading.
#define CONFIG_MAGIC   0xC0FFEE70
#define CONFIG_VERSION 27   // +1: grew SoundConfig.event_track[] from 10 to
                             // 13 entries (added Minute/Minutes/Timer over,
                             // now editable from Sound Setup > Events instead
                             // of being hardcoded track numbers) - like v25's
                             // resize, this shifts everything after it in
                             // AppConfig, so Functions/Timers/Model ID saved
                             // before this version may load scrambled and
                             // need re-entering.
                             // v26: appended AppConfig.timer_minute_call[] -
                             // pure append, old files simply default every
                             // timer's minute call to on.
                             // v25: appended AppConfig.inactivity. Also shrank
                             // SoundConfig.event_track[] from 11 to 10 entries
                             // (reworked which events exist) - like the v22
                             // SpecialFunction change, this shifts everything
                             // after it in AppConfig, so Functions/Timers/
                             // Model ID saved before this version may load
                             // scrambled and need re-entering.

struct AppConfig {
    uint32_t      magic;
    uint8_t       version;
    DeviceMode    mode;
    ChannelConfig channels[CRSF_NUM_CHANNELS];
    ELRSConfig    elrs;

    // ── CRSF line settings ──────────────────────────────────────────────────
    // invert:      S.Port-style inverted TTL (true) vs plain UART (false).
    // half_duplex: single-wire JR modules share one line for TX and RX, so the
    //              TX pin must be released to high-Z between frames, otherwise
    //              we hold the line and the module can never answer.
    // CRSF to an ExpressLRS module is plain 420k UART, idle high - not the
    // inverted S.Port signalling some other links use.
    bool crsf_invert      = false;
    bool crsf_half_duplex = true;

    // ── Output selection ────────────────────────────────────────────────────
    OutputMode out_mode = OutputMode::CRSF;
    PPMConfig  ppm;

    // True single-pin operation: transmit and receive both on GP0, leaving GP1
    // unused. Implies half duplex.
    // Two pins joined on the board, one wire to the module. This uses the
    // hardware UART to receive, which is the dependable path; single-pin
    // relies on a PIO receiver and has proved fragile.
    bool crsf_single_pin = false;

    // Search both polarities at startup until telemetry decodes.
    bool crsf_auto_pol   = true;

    // Which controller port to read on an adapter that presents more than one.
    // 0 is the first, which is what a single controller normally lands on.
    uint8_t hid_port = 0;

    // ── Per-axis calibration ────────────────────────────────────────────────
    AxisCal cal[MAX_INPUT_AXES];

    // ── Startup throttle safety ─────────────────────────────────────────────
    // Refuse to output anything but minimum on the throttle channel until it
    // has been seen at idle at least once. Stops a resting gamepad trigger
    // from spinning a motor the instant the board powers up.
    bool    thr_check = true;
    uint8_t thr_ch    = 2;       // 0-based, so CH3 by default

    // ── Behaviour when the input disconnects ────────────────────────────────
    // Force raw byte mode for HID devices instead of trusting the report
    // descriptor. Useful for joysticks whose descriptor is unusual.
    bool force_raw_hid = false;

    // Abort and re-issue a HID transfer after a period of silence. Recovers a
    // hung endpoint on some sticks, but upsets others badly enough that they
    // stop reporting entirely, so it is off unless asked for.
    bool stall_recovery = false;

    LinkLossAction loss_action = LinkLossAction::CUT_TX;
    uint16_t       loss_grace_ms = 1000;   // ignore brief dropouts

    SoundConfig sound;

    SpecialFunction sf[MAX_SPECIAL_FUNCS];
    TimerDef        timers[MAX_TIMERS];

    // ELRS Model ID - see the "MODEL MATCH" note near DeviceProfile below.
    // 0 means "not set" (matches ELRS's own default/disabled state).
    uint8_t model_id = 0;

    InactivityConfig inactivity;

    // Per-timer "minute call": announce each whole minute - elapsed for a
    // count-up timer, remaining for a countdown - with the same spoken-number
    // + "minute(s)" tracks the rest of this firmware already uses (OpenI6X
    // numbering), same idea as EdgeTX's own per-timer "Minute call" option.
    // A new, parallel trailing array rather than a field added to TimerDef:
    // TimerDef sits inside timers[MAX_TIMERS] above, so growing it would
    // shift model_id/inactivity to new offsets and scramble them in a file
    // saved by older firmware - see the ONLY EVER APPEND rule above.
    // On by default, matching the count-up minute call this firmware already
    // played unconditionally before this became switchable. NOTE: this
    // initializer has one `true` per MAX_TIMERS slot (currently 4) - add or
    // remove entries if MAX_TIMERS above ever changes.
    bool timer_minute_call[MAX_TIMERS] = { true, true, true, true };

    // The profile that was active when settings were last written, so it can
    // be loaded again after a power cycle (see AnyTx.ino setup()). Appended.
    bool     last_profile_valid = false;
    uint16_t last_profile_vid   = 0;
    uint16_t last_profile_pid   = 0;

    // Bit i set = channel i's trim does NOT play its max/min/centre sound
    // (Channels > CHn > Trim > Sound). 0 = every channel plays them.
    // A bitmask rather than a field in ChannelConfig so the saved layout of
    // the 16-channel array doesn't move.
    uint16_t trim_sound_off = 0;
};

// ─── PER-DEVICE PROFILE ──────────────────────────────────────────────────────
// Channel mixes are saved per USB device (keyed by VID/PID) in LittleFS, so
// each controller keeps its own mapping and is restored automatically when
// plugged back in.
// Same append-only rule as above; this should not need bumping again either.
#define PROFILE_MAGIC 0x50524F50u
#define PROFILE_NAME_LEN 14

// ─── MODEL MATCH ──────────────────────────────────────────────────────────────
// ExpressLRS TX modules use a small Model ID (0-255, 0 = unset) both to pick
// which stored RF parameters to use and, if the module's own "Model Match"
// option is separately turned on, to refuse to connect to a receiver bound
// for a different model - the same mechanism EdgeTX uses (Model Setup >
// Receiver number). It travels to the module as a CRSF PARAMETER_WRITE
// frame with field id 0, inferred from ExpressLRS's own source (CRSF.h
// keeps a dedicated `modelId` alongside its normal per-field parameter
// data) and from an EdgeTX bugfix ("send modelID after module reset")
// whose description confirms field 0 is how a handset communicates it -
// not from an official written spec, since one doesn't seem to exist for
// this specific frame. AnyTx sends it whenever it changes and again every
// time the module is (re)detected, matching that same EdgeTX fix's reasoning
// (a module that reset comes back assuming Model ID 0 until told otherwise).
// The "Model Match" enable/disable toggle itself is an ordinary parameter in
// the module's own live tree - see Menu > ELRS Setup, not this project.
struct DeviceProfile {
    uint32_t      magic;
    uint16_t      vid, pid;
    char          name[PROFILE_NAME_LEN];
    ChannelConfig channels[CRSF_NUM_CHANNELS];
    AxisCal       cal[MAX_INPUT_AXES];    // calibration travels with the device
    uint8_t       model_id = 0;
    SpecialFunction sf[MAX_SPECIAL_FUNCS];  // appended: special functions travel with the profile
    TimerDef        timers[MAX_TIMERS];     // appended: timers travel with the profile too
    bool            timer_minute_call[MAX_TIMERS] = { true, true, true, true };
    uint16_t        trim_sound_off = 0;     // appended: see AppConfig
};
