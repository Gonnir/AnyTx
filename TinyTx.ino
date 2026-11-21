// ═══════════════════════════════════════════════════════════════════════════════
//  TinyTx  –  RP2040 Zero → CRSF / ExpressLRS bridge
//
//  Plug a controller into the onboard USB-C port, or feed SBUS / PPM / an ELRS
//  receiver into GP13/GP14, and the board outputs CRSF to an ELRS TX module.
//
//  MODES
//    USB Input      any USB controller on the USB-C port:
//                     • generic HID gamepads (PS4, PS5, Switch Pro, no-names)
//                     • Xbox 360 / Xbox One / Series  (vendor class)
//                     • EdgeTX / OpenTX radios in USB Joystick mode
//                     • Radiomaster T8L (joystick mode or CRSF serial mode)
//    SBUS Input     inverted SBUS receiver on GP13
//    PPM Input      PPM stream on GP14
//    ELRS Repeater  ELRS receiver CRSF on GP13 → re-transmit
//    Pot/Btn UART   binary pot/button packet on GP13 (see README.txt)
//
//  WIRING
//    GP0  CRSF TX ─┬─[100R]── ELRS module signal pin
//    GP1  CRSF RX ─┘
//    GP3          DFPlayer BUSY (LOW while a track is playing)
//    GP4/GP5      OLED SDA/SCL (tries 0x3C then 0x3D at boot)
//    GP6/GP7      UP / DOWN buttons
//    GP8          DFPlayer RX (voice module, one-way command link)
//    GP10/GP11    OK / BACK buttons
//    GP13         SBUS in (inverted) / ELRS-RX CRSF in
//    GP14         PPM in
//    GP16         NeoPixel
//
//  ARDUINO IDE SETUP
//    Board:      Raspberry Pi Pico / RP2040  →  Waveshare RP2040 Zero
//    USB Stack:  Adafruit TinyUSB            ← required
//    Libraries:  Adafruit TinyUSB, Adafruit SSD1306, Adafruit GFX,
//                Adafruit NeoPixel
//    To upload:  hold BOOTSEL, tap RESET (host mode frees the USB port)
//
//  Core0 = application (CRSF out, mixer, UI, SBUS/PPM polling)
//  Core1 = USB host stack
// ═══════════════════════════════════════════════════════════════════════════════

#include <Wire.h>
#include <Adafruit_NeoPixel.h>
#include <hardware/watchdog.h>
#include <Adafruit_TinyUSB.h>

Adafruit_USBH_Host USBHost;

#include "config.h"
#include "storage.h"
#include "config_manager.h"
#include "profiles.h"
#include "crsf.h"
#include "ppm_out.h"
#include "hid_parser.h"
#include "ps_host.h"
#include "input.h"
#include "usb_diag.h"
#include "xinput_host.h"
#include "sound.h"
#include "ui.h"
#include "companion.h"

// ─── GLOBALS ─────────────────────────────────────────────────────────────────
ConfigManager     g_cfg;
CRSFEngine        g_crsf;
InputManager      g_input;
ChannelMixer      g_mixer;
UIEngine          g_ui;
HIDReportMap      g_hid_map;
HIDReportMap      g_hid_saved;   // descriptor of the adopted interface
PPMOut            g_ppm_out;
PPMOut*           PPMOut::_self = nullptr;
ProfileStore      g_profiles;
UsbDiag           g_diag;
DFPlayerVoice     g_voice;
Companion         g_companion;    // TinyTxCompanion serial settings link
Adafruit_NeoPixel g_led(1, PIN_LED, NEO_GRB + NEO_KHZ800);

uint16_t g_ch[CRSF_NUM_CHANNELS];

// ─── HID re-arm watchdog state ───────────────────────────────────────────────
// tuh_hid_receive_report() can be refused if the endpoint is momentarily busy.
// Giving up means no further report is ever requested and the device goes
// silent for good, so loop1() keeps retrying while the device is mounted.
static volatile uint8_t  g_hid_addr = 0;
static volatile uint8_t  g_hid_inst = 0;
static volatile bool     g_hid_up   = false;
static volatile uint32_t g_hid_last = 0;   // when the last report actually arrived
static volatile uint32_t g_hid_try  = 0;   // when we last attempted a re-arm

// A transfer can also hang while still holding the endpoint. Re-arming is then
// refused, which looks exactly like a healthy idle joystick, so it cannot be
// detected. Instead of trying, just tear the transfer down and start a new one
// after a period of silence: on an idle device the pending transfer holds no
// data, so aborting it loses nothing, and on a stuck one it is the only cure.
static volatile uint32_t g_abort_try = 0;
#define HID_QUIET_MS   3000        // long silence before assuming a stall
#define HID_ABORT_MS   1000        // how often to retry once quiet

// ─── Startup throttle gate ───────────────────────────────────────────────────
// Output on the throttle channel is pinned to minimum until the stick has
// actually been seen at idle, so a resting trigger cannot spin a motor the
// moment the board powers on. Latches open for the rest of the session.
static bool g_thr_ok = false;

// ─── Link-loss handling ──────────────────────────────────────────────────────
// A short grace period stops a momentary USB hiccup from cutting transmission.
static uint32_t g_loss_since = 0;
static bool     g_tx_cut     = false;

// ─── Device-change signal (Core1 -> Core0) ───────────────────────────────────
// Flash access must happen on Core0, so USB callbacks only record the VID/PID
// and raise a flag; loop() picks it up and loads that device's profile.
static volatile uint16_t g_new_vid = 0, g_new_pid = 0;
// Set when setup() restored the last-used profile; the first controller to
// attach afterwards keeps it instead of swapping in its own device profile.
static bool g_boot_profile = false;

static volatile bool     g_dev_new = false;

// Set at boot if RST was held: USB host stays off for this session.
static bool g_safe_mode = false;

// ─── STATUS LED ──────────────────────────────────────────────────────────────
static void ledSet(uint8_t r, uint8_t g, uint8_t b) {
    g_led.setPixelColor(0, (uint32_t)r * LED_BRIGHT / 255,
                            (uint32_t)g * LED_BRIGHT / 255,
                            (uint32_t)b * LED_BRIGHT / 255);
    g_led.show();
}

static void ledUpdate(const InputState& in) {
    static uint32_t last = 0;
    if (millis() - last < 100) return;
    last = millis();

    // The screen failing to come up is otherwise invisible - everything
    // else keeps running, including this LED - so it gets its own steady
    // colour (magenta, unused by any other state) rather than folding into
    // the normal link/input colours below, which would just look like a
    // stuck "no input" blue with no clue why.
    if (!g_ui.oledOk()) { ledSet(80, 0, 80); return; }

    LinkStats lk = g_crsf.linkStats();

    if (!g_crsf.deviceFound())        ledSet(80,  0,  0);   // red    - no ELRS module
    else if (in.failsafe)             ledSet(80, 40,  0);   // orange - failsafe
    else if (!in.connected)           ledSet( 0,  0, 80);   // blue   - no input
    else if (lk.valid && lk.lq < 40)  ledSet(80, 40,  0);   // orange - weak link
    else                              ledSet( 0, 80,  0);   // green  - all good
}

// ═══════════════════════════════════════════════════════════════════════════════
//  CORE 0
// ═══════════════════════════════════════════════════════════════════════════════
void setup() {
    // ── Safe mode ───────────────────────────────────────────────────────────
    // Hold BACK at power-on to boot without starting the USB host. If a device
    // makes the board reset in a loop, this is the way back in: the menu works
    // normally and settings can be changed, USB is simply left off until the
    // next restart.
    pinMode(PIN_BTN_BACK, INPUT_PULLUP);
    delay(10);
    g_safe_mode = (digitalRead(PIN_BTN_BACK) == LOW);

    Wire.setSDA(PIN_I2C_SDA);
    Wire.setSCL(PIN_I2C_SCL);
    Wire.setClock(400000);
    Wire.begin();

    g_led.begin();
    if (watchdog_caused_reboot()) {
        for (int i = 0; i < 3; i++) { ledSet(255,0,0); delay(100); ledSet(0,0,0); delay(100); }
    }
    ledSet(0, 0, 20);

    for (auto& c : g_ch) c = CRSF_CHANNEL_MID;

    g_cfg.begin();
    g_profiles.begin();

    g_input.begin(g_cfg.cfg.mode);
    g_input.setHIDMap(&g_hid_map);
    g_input.setCalibration(g_cfg.cfg.cal);
    g_thr_ok = !g_cfg.cfg.thr_check;

    g_voice.begin(g_cfg.cfg.sound.enabled, g_cfg.cfg.sound.volume);
    { uint16_t t = g_cfg.cfg.sound.event_track[SE_STARTUP]; if (t) g_voice.play(t); }

    if (g_cfg.cfg.out_mode == OutputMode::CRSF) {
        Serial1.setTX(PIN_CRSF_TX);
        Serial1.setRX(PIN_CRSF_RX);
        Serial1.setFIFOSize(512);   // default 32 bytes overflows during display/flash stalls at 420k baud; long replies (DEVICE_INFO) arrived truncated

        g_crsf.begin(Serial1, CRSF_BAUDRATE,
                     g_cfg.cfg.crsf_invert, g_cfg.cfg.crsf_half_duplex,
                     g_cfg.cfg.crsf_single_pin);

        g_crsf.setAutoPolarity(g_cfg.cfg.crsf_auto_pol);

        // TinyTxCompanion: with a USB-serial adapter on GP0/GP1 instead of
        // the ELRS module, the page's "HELLO" takes the UART over.
        g_companion.begin(&Serial1, &g_cfg, &g_profiles, &g_crsf, &g_ui, &g_voice, g_ch);
    } else {
        // GP0 becomes a plain output driving the trainer signal.
        g_ppm_out.begin(PIN_PPM_OUT, g_cfg.cfg.ppm);
    }

    if (g_cfg.cfg.mode == DeviceMode::PPM_INPUT) {
        pinMode(PIN_PPM_IN, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(PIN_PPM_IN), ppm_isr, RISING);
    }

    g_ui.begin(&g_cfg, &g_crsf, &g_input, &g_profiles, &g_mixer, &g_voice);

    // Load the profile that was active at the last power-off.
    if (g_cfg.cfg.last_profile_valid && g_profiles.available()) {
        char pname[PROFILE_NAME_LEN] = {};
        if (g_profiles.load(g_cfg.cfg.last_profile_vid, g_cfg.cfg.last_profile_pid,
                            g_cfg.cfg.channels, g_cfg.cfg.cal, &g_cfg.cfg.model_id,
                            pname, sizeof(pname), g_cfg.cfg.sf,
                            g_cfg.cfg.timers, g_cfg.cfg.timer_minute_call,
                         &g_cfg.cfg.trim_sound_off)) {
            g_ui.setActiveProfile(g_cfg.cfg.last_profile_vid, g_cfg.cfg.last_profile_pid, pname);
            g_ui.resetTimers();
            g_boot_profile = true;
        }
    }
    g_ui.setSafeMode(g_safe_mode);

    // 5 s window. Enumerating an awkward device plus a flash commit can take a
    // surprising amount of time, and a reset loop is far worse than a slow boot.
    watchdog_enable(5000, true);
}

// Apply the profile for a newly attached controller, or fall back to defaults.
static void handleDeviceChange() {
    if (!g_dev_new) return;
    uint16_t vid = g_new_vid, pid = g_new_pid;
    g_dev_new = false;

    g_ui.setDevice(vid, pid);
    g_voice.refresh();   // re-send the volume once the new device's power draw has settled

    char pname[PROFILE_NAME_LEN] = {};
    if (g_boot_profile) {
        g_boot_profile = false;   // the last-used profile stays in charge
    } else if (g_profiles.available() &&
        g_profiles.load(vid, pid, g_cfg.cfg.channels, g_cfg.cfg.cal, &g_cfg.cfg.model_id,
                         pname, sizeof(pname), g_cfg.cfg.sf,
                         g_cfg.cfg.timers, g_cfg.cfg.timer_minute_call,
                         &g_cfg.cfg.trim_sound_off)) {
        // Saved mapping, calibration, and Model ID restored for this exact
        // controller - push the Model ID to the module right away rather
        // than waiting for the link to bounce (UIEngine::update() handles
        // the separate case of the module itself resetting, independent of
        // any USB device change, the same way EdgeTX does).
        g_crsf.sendModelId(g_cfg.cfg.model_id);
        g_ui.setActiveProfile(vid, pid, pname);
        g_ui.resetTimers();   // the profile's timers start fresh
    } else {
        ConfigManager::setChannelDefaults(g_cfg.cfg.channels, g_cfg.cfg.mode);
        for (uint8_t i = 0; i < MAX_INPUT_AXES; i++) g_cfg.cfg.cal[i] = AxisCal{};
        // Special functions and timers are part of a profile too, so the
        // defaults for a device with no profile are empty rows, not leftovers.
        for (uint8_t i = 0; i < MAX_SPECIAL_FUNCS; i++) g_cfg.cfg.sf[i] = SpecialFunction{};
        for (uint8_t i = 0; i < MAX_TIMERS; i++) { g_cfg.cfg.timers[i] = TimerDef{}; g_cfg.cfg.timer_minute_call[i] = true; }
        g_cfg.cfg.trim_sound_off = 0;
        g_ui.resetTimers();
        g_ui.setActiveProfile(0, 0, nullptr);   // no saved profile for this device
    }
    // Deliberately NOT written to flash here. A flash erase needs the other
    // core parked, and this runs while USB is actively streaming. The profile
    // is already stored, so there is nothing to preserve.

    // Re-arm the throttle check only for a genuinely new connection. A device
    // that re-enumerated after a glitch keeps its gate open, otherwise the
    // throttle would be forced back to minimum mid-flight.
    bool blip = (millis() - g_input.lastPresentMs()) < 3000;
    if (!blip) g_thr_ok = !g_cfg.cfg.thr_check;
}

// If Core1 stops running, USB dies silently and the device looks frozen even
// though Core0 is fine. Detect that and stop feeding the watchdog, so the board
// resets and comes back rather than sitting there wedged.
static bool core1Alive() {
    static uint32_t last_beat = 0;
    static uint32_t last_ms   = 0;
    uint32_t now  = millis();
    uint32_t beat = g_diag.core1_beat;

    if (beat != last_beat) { last_beat = beat; last_ms = now; return true; }
    if (last_ms == 0)      { last_ms = now;                   return true; }

    if (now - last_ms > 1500) {      // stalled well beyond any normal delay
        g_diag.core1_stall++;
        return false;
    }
    return true;
}

// NOTE: there is deliberately no automatic reboot for a dead endpoint.
// Aborting and re-issuing the transfer (see loop1) recovers it in place, which
// keeps sticky switch states, the throttle gate and the link intact. Restarting
// would silently disarm the model, so it is left as a manual choice under
// Menu > Safety > Restart device.

void loop() {
    if (core1Alive()) watchdog_update();
    g_diag.report_age_ms = g_hid_up ? (millis() - g_hid_last) : 0;

    handleDeviceChange();
    watchdog_update();      // device handling can be slow; do not let it bite
    g_input.setForceRaw(g_cfg.cfg.force_raw_hid);   // applies to the next report
    g_input.processUSB();   // decode whatever the USB callbacks staged
    g_input.pollAux();
    g_input.update();

    // One lock-free snapshot per iteration, shared by mixer, UI and LED.
    InputState in = g_input.snapshot();

    // Trim buttons run before mixing so a press shows up on this same frame.
    if (g_mixer.applyTrimButtons(g_cfg.cfg.channels, in)) g_cfg.markDirty();

    g_mixer.mix(g_cfg.cfg.channels, in, g_ch);
    g_ui.applyOverrides(g_ch, in);

    // ── Startup throttle safety ─────────────────────────────────────────────
    if (!g_thr_ok) {
        uint8_t t = g_cfg.cfg.thr_ch;
        if (t < CRSF_NUM_CHANNELS) {
            // Release once throttle sits in the bottom 10% of its travel
            const uint16_t idle =
                CRSF_CHANNEL_MIN + (CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN) / 10;
            if (in.connected && g_ch[t] <= idle) g_thr_ok = true;
            else                                  g_ch[t] = CRSF_CHANNEL_MIN;
        } else {
            g_thr_ok = true;
        }
    }
    g_ui.setThrottleWarn(!g_thr_ok);

    // ── Link loss: either keep streaming failsafe values, or stop sending RC
    //    frames so the receiver detects the loss and runs its own failsafe.
    if (g_mixer.inFailsafe()) {
        if (g_loss_since == 0) g_loss_since = millis();
        bool past_grace = (millis() - g_loss_since) >= g_cfg.cfg.loss_grace_ms;
        g_tx_cut = past_grace &&
                   (g_cfg.cfg.loss_action == LinkLossAction::CUT_TX);
    } else {
        g_loss_since = 0;
        g_tx_cut     = false;
    }
    g_ui.setTxCut(g_tx_cut);

    if (g_cfg.cfg.out_mode == OutputMode::CRSF) {
        g_companion.service();            // owns the UART while a session is open
        if (!g_companion.active()) {
            g_crsf.setRCEnabled(!g_tx_cut);
            g_crsf.setChannels(g_ch);
            g_crsf.update();
        }
    } else {
        // Trainer output: link loss silences the pulses, which is how a
        // trainer port detects that the student box has stopped.
        g_ppm_out.setEnabled(!g_tx_cut);
        g_ppm_out.setChannels(g_ch);
    }

    g_ui.update(g_ch, in);
    ledUpdate(in);
    g_voice.update();

    // Deferred config write, once the user stops turning knobs.
    g_cfg.maybeCommit();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  CORE 1 – USB host
// ═══════════════════════════════════════════════════════════════════════════════
void setup1() {
    // Give Core0 time to sample the RST pin before deciding.
    delay(60);
    if (g_safe_mode) return;               // USB host never starts
    USBHost.begin(0);   // rhport 0 = native USB hardware = onboard USB-C port
}

void loop1() {
    g_diag.core1_beat++;      // heartbeat, watched by Core0
    if (g_safe_mode) { delay(5); return; }
    USBHost.task();
    XInputHost::poll();

    // Restart HID polling if nothing has arrived recently. When a transfer is
    // already pending this call simply returns false and costs nothing.
    // Re-arm aggressively. When a transfer is already outstanding this call is
    // refused and costs almost nothing, but if the HID driver ever drops the
    // chain - typically after a transfer completes with an error - we pick it
    // straight back up instead of going silent for good.
    if (g_hid_up && (millis() - g_hid_try >= 8)) {
        g_hid_try = millis();
        if (!tuh_mounted(g_hid_addr)) {
            g_hid_up = false;
        } else if (tuh_hid_receive_report(g_hid_addr, g_hid_inst)) {
            g_diag.arm_ok++;
        } else {
            g_diag.arm_fail++;
        }
    }

    // After a stretch of silence, abort whatever transfer is outstanding and
    // start a fresh one. Harmless while genuinely idle; the only thing that
    // recovers an endpoint whose transfer has hung.
    if (g_cfg.cfg.stall_recovery &&
        g_hid_up && tuh_mounted(g_hid_addr) &&
        (millis() - g_hid_last  > HID_QUIET_MS) &&
        (millis() - g_abort_try > HID_ABORT_MS)) {
        g_abort_try = millis();
        // Proper HID-level abort: the driver knows its own endpoint, so
        // nothing has to be guessed and no unrelated endpoint gets disturbed.
        tuh_hid_receive_abort(g_hid_addr, g_hid_inst);
        tuh_hid_receive_report(g_hid_addr, g_hid_inst);
        g_diag.aborts++;
        // Count it as a recovery once data starts flowing again, so the
        // counter reflects real faults rather than idle housekeeping.
        if (millis() - g_hid_last > HID_QUIET_MS * 2) g_diag.hid_stuck++;
    }

}

// ─── DEVICE MOUNT / UNMOUNT (fires for every USB device, any class) ──────────
void tuh_mount_cb(uint8_t dev_addr) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    g_diag.last_vid = vid;
    g_diag.last_pid = pid;
    g_diag.mounts++;
    g_new_vid = vid;
    g_new_pid = pid;
    g_dev_new = true;
    XInputHost::onMount(dev_addr);   // ignores anything that isn't Xbox/XInput
}

void tuh_umount_cb(uint8_t dev_addr) {
    XInputHost::onUnmount(dev_addr);
}

// ─── VENDOR CLASS (needed so TinyUSB claims Xbox interfaces) ──────────────────
void tuh_vendor_mount_cb  (uint8_t, uint8_t) {}
void tuh_vendor_umount_cb (uint8_t, uint8_t) {}

// ─── HID ─────────────────────────────────────────────────────────────────────
// Parse the report descriptor so we know where each axis and button lives.
// This is what makes generic gamepads and EdgeTX radios work without any
// device-specific code.
// A joystick can expose several HID interfaces - a gamepad collection plus a
// consumer-control or vendor one, say. Only the interface that actually
// describes axes is adopted; the others are still armed so they do not stall,
// but their descriptors must not overwrite the good one.
static volatile bool g_hid_adopted = false;

// Which PlayStation pad is attached, if any. Their reports are decoded from a
// known layout rather than from the descriptor.
static volatile uint8_t g_ps_type = PS_NONE;

// How many HID interfaces carrying axes this device has offered so far. A
// two-port adapter presents one per controller port, so this is what the Pad
// port setting counts against. Reset when the device goes away.
static volatile uint8_t g_axis_itf = 0;

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                       uint8_t const* desc_report, uint16_t desc_len) {
    // PlayStation pads get their own decoding: their descriptors describe one
    // big blob that generic parsing turns into a heap of meaningless axes.
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    PSType ps = psIdentify(vid, pid);
    g_ps_type = ps;

    if (ps == PS_DS3) {
        // A DualShock 3 enumerates and then says nothing at all until it is
        // sent this feature report. It is the whole reason the pad otherwise
        // looks broken.
        static uint8_t en[sizeof(DS3_ENABLE)];
        memcpy(en, DS3_ENABLE, sizeof(en));
        tuh_hid_set_report(dev_addr, instance, DS3_ENABLE_REPORT_ID,
                           HID_REPORT_TYPE_FEATURE, en, sizeof(en));
    }

    // desc_report is NULL when the descriptor was larger than TinyUSB's
    // enumeration buffer, and some joysticks declare descriptors this stack
    // cannot make sense of. parse() falls back to raw byte mode in both cases,
    // which still gives full use of the device through channel learning.
    g_hid_map.parse(desc_report, (desc_len < 2048) ? desc_len : 0);

    // Nothing usable found, or the user asked for raw mode explicitly.
    if (g_cfg.cfg.force_raw_hid || !g_hid_map.valid())
        g_hid_map.forceRaw();

    bool has_axes = (g_hid_map.axisCount() > 0);

    // Adapters that take two controllers - the common PS2-to-USB dongles do
    // exactly this - present two identical HID interfaces, one per port. The
    // wanted one is normally the first, so count the interfaces that carry
    // axes and adopt the one the Port setting selects. Letting each new
    // interface overwrite the last meant always landing on the second port,
    // which is usually empty: detected, but nothing ever moves.
    if (has_axes) {
        if (g_axis_itf == g_cfg.cfg.hid_port && !g_hid_adopted) {
            g_hid_addr    = dev_addr;
            g_hid_inst    = instance;
            g_hid_up      = true;
            g_hid_last    = millis();
            g_hid_adopted = true;
            g_hid_saved   = g_hid_map;

            g_diag.desc_len = desc_len;
            g_diag.map_axes = g_hid_map.axisCount();
            g_diag.map_btns = g_hid_map.buttonCount();
            g_diag.raw_mode = (g_hid_map.mode() == HIDMode::RAW);
            g_diag.ps_type  = (uint8_t)ps;
        } else {
            g_hid_map = g_hid_saved;      // keep the one already chosen
        }
        g_axis_itf++;
    }
    else if (!g_hid_up) {
        // Nothing better seen yet; track it so it still gets re-armed.
        g_hid_addr = dev_addr;
        g_hid_inst = instance;
        g_hid_up   = true;
        g_hid_last = millis();
    }
    g_diag.hid_itf = g_axis_itf;

    g_input.setPresent(true);
    g_diag.hid_mounts++;

    // Arm every interface so none of them stalls the device.
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    if (g_hid_addr == dev_addr) {
        g_hid_up      = false;
        g_hid_adopted = false;
        g_ps_type     = PS_NONE;
        g_axis_itf    = 0;
    }
    g_hid_map.reset();
    g_input.notifyDisconnect();
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                  uint8_t const* report, uint16_t len) {
    // Xbox controllers are handled by the vendor driver, not here
    g_diag.reports++;
    g_diag.last_rpt_len = (uint8_t)len;
    g_hid_last = millis();
    { uint8_t n = (len > 16) ? 16 : (uint8_t)len;
      for (uint8_t i = 0; i < n; i++) g_diag.raw[i] = report[i];
      g_diag.raw_len = n; }
    // Only the adopted interface feeds the mixer. Others are still re-armed
    // below so the device keeps running, but their reports are ignored.
    bool mine = (dev_addr == g_hid_addr && instance == g_hid_inst);

    // Stage only - Core0 decodes it. Keeping this callback short is what stops
    // a fast-reporting joystick from stalling the USB host task.
    if (mine && !XInputHost::active()) {
        uint8_t rt = InputManager::RPT_HID;
        switch (g_ps_type) {
            case PS_DS3: rt = InputManager::RPT_DS3; break;
            case PS_DS4: rt = InputManager::RPT_DS4; break;
            case PS_DS5: rt = InputManager::RPT_DS5; break;
            default: break;
        }
        g_input.submitReport(rt, report, len);
    }

    // Re-arm straight away so the next report is already requested.
    if (!tuh_hid_receive_report(dev_addr, instance))
        tuh_hid_receive_report(dev_addr, instance);
}

// ─── CDC (Radiomaster T8L in CRSF serial mode) ───────────────────────────────
void tuh_cdc_mount_cb(uint8_t idx) {
    g_diag.cdc_mounts++;
    g_input.setPresent(true);
    cdc_line_coding_t lc = { 400000, CDC_LINE_CODING_STOP_BITS_1,
                              CDC_LINE_CODING_PARITY_NONE, 8 };
    tuh_cdc_set_line_coding(idx, &lc, nullptr, 0);
}

void tuh_cdc_umount_cb(uint8_t) {
    g_input.notifyDisconnect();
}

void tuh_cdc_rx_cb(uint8_t idx) {
    uint8_t buf[64];
    uint32_t n = tuh_cdc_read(idx, buf, sizeof(buf));
    g_input.submitCDC(buf, n);      // Core0 parses the CRSF frames
}
