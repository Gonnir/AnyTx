#include "input.h"
#include "crsf.h"
#include "usb_diag.h"
#include "ps_host.h"
#include <hardware/gpio.h>
#include <math.h>

#define DMB() __asm volatile ("dmb" ::: "memory")

// ─── PPM ISR ─────────────────────────────────────────────────────────────────
static volatile uint16_t _ppm_stage[PPM_MAX_CH] = {};
volatile uint16_t g_ppm_raw[PPM_MAX_CH] = {};
volatile uint8_t  g_ppm_count   = 0;
volatile bool     g_ppm_updated = false;
static volatile uint32_t _ppm_last = 0;
static volatile uint8_t  _ppm_idx  = 0;

void ppm_isr() {
    uint32_t now = micros();
    uint32_t dt  = now - _ppm_last;
    _ppm_last = now;

    if (dt > PPM_SYNC_US) {
        if (_ppm_idx >= 4) {
            for (uint8_t i = 0; i < _ppm_idx; i++) g_ppm_raw[i] = _ppm_stage[i];
            g_ppm_count   = _ppm_idx;
            g_ppm_updated = true;
        }
        _ppm_idx = 0;
    } else if (_ppm_idx < PPM_MAX_CH) {
        if (dt >= PPM_MIN_US && dt <= PPM_MAX_US) _ppm_stage[_ppm_idx++] = (uint16_t)dt;
        else _ppm_idx = 0;
    }
}

// ─── AUX SERIAL (only one is begin()-ed, chosen by mode) ─────────────────────
static SerialPIO _sbus_pio  ((pin_size_t)0xFF, PIN_AUX_RX, 256);
static SerialPIO _crsfr_pio ((pin_size_t)0xFF, PIN_AUX_RX, 256);
static SerialPIO _potbtn_pio((pin_size_t)0xFF, PIN_AUX_RX, 256);

void InputManager::begin(DeviceMode mode) {
    _mode = mode;
    _work = InputState{};
    _seq  = 0;
    publish();

    switch (mode) {
        case DeviceMode::SBUS_INPUT:
            _sbus_pio.begin(SBUS_BAUDRATE);
            gpio_set_inover(PIN_AUX_RX, GPIO_OVERRIDE_INVERT);   // SBUS is inverted
            _aux = &_sbus_pio;
            break;
        case DeviceMode::ELRS_REPEATER:
            _crsfr_pio.begin(CRSF_BAUDRATE);
            gpio_set_inover(PIN_AUX_RX, GPIO_OVERRIDE_NORMAL);
            _aux = &_crsfr_pio;
            break;
        case DeviceMode::POT_BTN_UART:
            _potbtn_pio.begin(POT_BTN_BAUDRATE);
            gpio_set_inover(PIN_AUX_RX, GPIO_OVERRIDE_NORMAL);   // not inverted
            _aux = &_potbtn_pio;
            _pb_sync = 0; _pb_len = 0; _pb_need = 0;
            break;
        default:
            _aux = nullptr;
            break;
    }
}

// ─── CALIBRATION ─────────────────────────────────────────────────────────────
// Map the travel the stick actually achieves onto the full output range, and
// ignore jitter inside the deadband.
static int16_t applyCal(int16_t raw, const AxisCal& c) {
    int32_t d  = (int32_t)raw - c.center;
    int32_t db = ((int32_t)c.deadband * 32768) / 100;

    if (d >= -db && d <= db) return 0;          // inside deadband: dead centre
    d = (d > 0) ? (d - db) : (d + db);          // remove the dead zone offset

    if (d > 0) {
        int32_t span = (int32_t)c.max - c.center - db;
        if (span < 1) return 0;
        return (int16_t)constrain((d * 32767) / span, 0, 32767);
    } else {
        int32_t span = (int32_t)c.center - c.min - db;
        if (span < 1) return 0;
        return (int16_t)constrain((d * 32768) / span, -32768, 0);
    }
}

// ─── SEQLOCK ─────────────────────────────────────────────────────────────────
void InputManager::publish() {
    _seq++;                 // now odd: a write is in progress
    DMB();
    _pub = _work;
    if (_cal && !_cal_bypass) {
        for (uint8_t i = 0; i < _work.num_axes && i < MAX_INPUT_AXES; i++)
            _pub.axes[i] = applyCal(_work.axes[i], _cal[i]);
    }
    DMB();
    _seq++;                 // now even: stable again
}

InputState InputManager::snapshot() const {
    InputState out;
    uint32_t s1, s2;
    uint8_t  tries = 0;
    do {
        s1 = _seq;
        DMB();
        out = _pub;
        DMB();
        s2 = _seq;
        // Bail out rather than spin. Worst case we return one slightly torn
        // frame, which for RC channel data is harmless - and far better than
        // stalling the core.
        if (++tries >= 8) break;
    } while ((s1 & 1u) || s1 != s2);
    return out;
}

// ─── TIMEOUTS (Core0) ────────────────────────────────────────────────────────
void InputManager::update() {
    // In USB mode Core1 owns the state; connection is driven by mount/unmount
    // presence, not by report timing, so there is nothing to do here.
    if (_mode == DeviceMode::USB_INPUT) {
        // Either signal is enough to call the device live: the mount callback
        // having reported it present, or reports actually arriving. Requiring
        // both meant a missed presence flag could leave the input marked
        // disconnected while data was flowing, which the mixer then treated
        // as link loss and froze every channel.
        bool live = _present || (_last_recv && (millis() - _last_recv < 1500));
        if (live != _work.connected) { _work.connected = live; publish(); }
        return;
    }

    uint32_t now = millis();
    if (_work.connected && (now - _last_recv > 1500)) {
        _work.connected = false;
        _work.buttons   = 0;
        _work.source    = InputSource::NONE;
        publish();
    }
}

// ═══ Core1: staging only ═════════════════════════════════════════════════════
// Everything here must be short. No decoding, no publishing, no flash.

void InputManager::submitReport(uint8_t type, const uint8_t* r, uint16_t len) {
    if (len > sizeof(_stage)) len = sizeof(_stage);
    _stage_seq++;                       // odd: write in progress
    DMB();
    for (uint16_t i = 0; i < len; i++) _stage[i] = r[i];
    _stage_len  = (uint8_t)len;
    _stage_type = type;
    DMB();
    _stage_seq++;                       // even: complete
}

void InputManager::submitCDC(const uint8_t* b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint16_t nxt = (uint16_t)((_cdc_head + 1) % sizeof(_cdc_ring));
        if (nxt == _cdc_tail) break;    // full: drop rather than block
        _cdc_ring[_cdc_head] = b[i];
        _cdc_head = nxt;
    }
}

void InputManager::setPresent(bool p) {
    _present_req = p ? 1 : 0;
}

// ═══ Core0: do the actual work ═══════════════════════════════════════════════
void InputManager::processUSB() {
    // ── presence changes ────────────────────────────────────────────────────
    int8_t req = _present_req;
    if (req >= 0) {
        _present_req = -1;
        bool now_present = (req == 1);

        if (!now_present && _present) {
            // Going away: park the sticky states in case this is only a blip.
            memcpy(_tgl_saved, _work.toggles, sizeof(_tgl_saved));
            _absent_at    = millis();
            _work         = InputState{};
            _prev_buttons = 0;
            _cdc_head = _cdc_tail = 0;
            _cdc_len  = 0;
            publish();
        } else if (now_present && !_present) {
            // Coming back quickly enough to count as the same session.
            if (_absent_at && (millis() - _absent_at) < STICKY_KEEP_MS)
                memcpy(_work.toggles, _tgl_saved, sizeof(_tgl_saved));
            else
                memset(_work.toggles, 0, sizeof(_work.toggles));
            publish();
        }
        _present = now_present;
    }
    if (_present) _last_present_ms = millis();

    // ── staged report ───────────────────────────────────────────────────────
    uint32_t s1 = _stage_seq;
    if (s1 != _stage_seen && !(s1 & 1u)) {
        uint8_t buf[64];
        uint8_t len  = _stage_len;
        uint8_t type = _stage_type;
        if (len > sizeof(buf)) len = sizeof(buf);
        for (uint8_t i = 0; i < len; i++) buf[i] = _stage[i];

        if (_stage_seq == s1) {         // not torn while we copied
            _stage_seen = s1;
            switch (type) {
                case RPT_HID:    parseHIDReport(buf, len); break;
                case RPT_XINPUT: parseXInput   (buf, len); break;
                case RPT_GIP:    parseGIP      (buf, len); break;
                case RPT_DS3:
                case RPT_DS4:
                case RPT_DS5:    parsePS       (buf, len, type); break;
                default: break;
            }
        }
    }

    // ── CDC byte stream ─────────────────────────────────────────────────────
    uint16_t budget = 128;
    while (_cdc_tail != _cdc_head && budget--) {
        uint8_t b = _cdc_ring[_cdc_tail];
        _cdc_tail = (uint16_t)((_cdc_tail + 1) % sizeof(_cdc_ring));
        parseCDCByte(b);
    }
}

// ─── POLL AUX (Core0) ────────────────────────────────────────────────────────
void InputManager::pollAux() {
    if (_aux) {
        // Bound the work done per call so a noisy line can never monopolise
        // the loop and trip the watchdog.
        uint16_t budget = 128;
        while (_aux->available() && budget--) {
            uint8_t b = (uint8_t)_aux->read();
            if (_mode == DeviceMode::SBUS_INPUT)          parseSBUSByte(b);
            else if (_mode == DeviceMode::ELRS_REPEATER)  parseCRSFRxByte(b);
            else if (_mode == DeviceMode::POT_BTN_UART)   parsePotBtnByte(b);
        }
    }

    if (_mode == DeviceMode::PPM_INPUT && g_ppm_updated) {
        noInterrupts();
        uint8_t  cnt = g_ppm_count;
        uint16_t raw[PPM_MAX_CH];
        for (uint8_t i = 0; i < cnt; i++) raw[i] = g_ppm_raw[i];
        g_ppm_updated = false;
        interrupts();

        uint8_t n = min((int)cnt, MAX_INPUT_AXES);
        for (uint8_t i = 0; i < n; i++) {
            int32_t v = ((int32_t)raw[i] - 1500) * 65;   // 1000-2000us -> full scale
            _work.axes[i] = (int16_t)constrain(v, -32768, 32767);
        }
        _work.num_axes    = n;
        _work.num_buttons = 0;
        _work.connected   = true;
        _work.failsafe    = false;
        _work.source      = InputSource::PPM;
        _last_recv        = millis();
        publish();
    }
}

// Read a report as plain bytes, making no use of the descriptor at all.
// Every byte becomes an axis and every bit a button; the channel-learning
// screen finds whichever one the user actually moves.
static bool rawDecode(const uint8_t* r, uint16_t len,
                       int16_t ax[MAX_INPUT_AXES], uint8_t* na,
                       uint32_t* btns, uint8_t* nb) {
    if (!r || len == 0) return false;
    uint8_t n = (len > MAX_INPUT_AXES) ? MAX_INPUT_AXES : (uint8_t)len;
    for (uint8_t i = 0; i < n; i++)
        ax[i] = (int16_t)(((int32_t)r[i] - 128) * 256);

    uint32_t b = 0;
    uint8_t  cnt = (len > 4) ? 32 : (uint8_t)(len * 8);
    for (uint8_t i = 0; i < cnt && i < 32; i++)
        if (r[i >> 3] & (1u << (i & 7))) b |= (1UL << i);

    *na = n; *btns = b; *nb = cnt;
    return true;
}

// ─── GENERIC HID (gamepads, EdgeTX radios, joysticks) ────────────────────────
void InputManager::parseHIDReport(const uint8_t* r, uint16_t len) {
    int16_t  ax[MAX_INPUT_AXES] = {};
    uint32_t btns = 0;
    uint8_t  na = 0, nb = 0;

    bool ok;
    if (_force_raw || !_hid_map || !_hid_map->valid())
        ok = rawDecode(r, len, ax, &na, &btns, &nb);
    else
        ok = _hid_map->decode(r, len, ax, &na, &btns, &nb);

    // Descriptor decoding produced nothing usable - fall back to raw bytes
    // rather than dropping the report and leaving the axes frozen.
    if (!ok) ok = rawDecode(r, len, ax, &na, &btns, &nb);

    if (!ok) { g_diag.decode_fail++; return; }
    g_diag.decode_ok++;
    if (_hid_map) {
        g_diag.used_rid     = _hid_map->usesReportID();
        g_diag.rid_fallback = _hid_map->ridFallback();
    }
    g_diag.raw_mode = _force_raw || !_hid_map || !_hid_map->valid();

    memcpy(_work.axes, ax, sizeof(ax));
    _work.num_axes    = na;
    _work.num_buttons = nb;
    _work.buttons     = btns;
    edges(btns);
    _work.connected   = true;
    _work.failsafe    = false;
    _work.source      = InputSource::HID_GAMEPAD;
    _last_recv        = millis();
    publish();
}

// ─── XBOX 360 / XINPUT ───────────────────────────────────────────────────────
// [0]=0x00 [1]=0x14 [2-3]=buttons [4]=LT [5]=RT [6..13]=LX LY RX RY (int16)
void InputManager::parseXInput(const uint8_t* r, uint16_t len) {
    if (len < 14) return;
    _work.axes[0] = (int16_t)((uint16_t)r[6]  | ((uint16_t)r[7]  << 8));
    _work.axes[1] = (int16_t)((uint16_t)r[8]  | ((uint16_t)r[9]  << 8));
    _work.axes[2] = (int16_t)((uint16_t)r[10] | ((uint16_t)r[11] << 8));
    _work.axes[3] = (int16_t)((uint16_t)r[12] | ((uint16_t)r[13] << 8));
    _work.axes[4] = (int16_t)(((int32_t)r[4] * 65535 / 255) - 32768);
    _work.axes[5] = (int16_t)(((int32_t)r[5] * 65535 / 255) - 32768);
    _work.num_axes    = 6;
    _work.buttons     = (uint32_t)r[2] | ((uint32_t)r[3] << 8);
    _work.num_buttons = 16;
    edges(_work.buttons);
    _work.connected = true;
    _work.failsafe  = false;
    _work.source    = InputSource::XINPUT;
    _last_recv      = millis();
    publish();
}

// ─── XBOX ONE / GIP ──────────────────────────────────────────────────────────
// [0]=0x20 [4-5]=buttons [6]=LT [7]=RT [8..15]=LX LY RX RY (int16)
void InputManager::parseGIP(const uint8_t* r, uint16_t len) {
    if (len < 16 || r[0] != 0x20) return;
    _work.axes[0] = (int16_t)((uint16_t)r[8]  | ((uint16_t)r[9]  << 8));
    _work.axes[1] = (int16_t)((uint16_t)r[10] | ((uint16_t)r[11] << 8));
    _work.axes[2] = (int16_t)((uint16_t)r[12] | ((uint16_t)r[13] << 8));
    _work.axes[3] = (int16_t)((uint16_t)r[14] | ((uint16_t)r[15] << 8));
    _work.axes[4] = (int16_t)(((int32_t)r[6] * 65535 / 255) - 32768);
    _work.axes[5] = (int16_t)(((int32_t)r[7] * 65535 / 255) - 32768);
    _work.num_axes    = 6;
    _work.buttons     = (uint32_t)r[4] | ((uint32_t)r[5] << 8);
    _work.num_buttons = 16;
    edges(_work.buttons);
    _work.connected = true;
    _work.failsafe  = false;
    _work.source    = InputSource::XBOX_GIP;
    _last_recv      = millis();
    publish();
}

// ─── PLAYSTATION CONTROLLERS ─────────────────────────────────────────────────
// Three different layouts, one common result: six axes and a shared button
// numbering, so a channel mapping learned on one pad still makes sense on
// another.
//
//   DS4  [1]LX [2]LY [3]RX [4]RY [5]face+hat [6]shoulder [7]ps [8]L2 [9]R2
//   DS5  [1]LX [2]LY [3]RX [4]RY [5]L2  [6]R2  [8]face+hat [9]shoulder [10]ps
//   DS3  [6]LX [7]LY [8]RX [9]RY [2]dpad+stick [3]face+shoulder [4]ps
//                                             [18]L2 [19]R2
void InputManager::parsePS(const uint8_t* r, uint16_t len, uint8_t type) {
    uint8_t  lx, ly, rx, ry, l2, r2;
    uint32_t btn = 0;

    if (type == RPT_DS4) {
        if (len < 10) return;
        lx = r[1]; ly = r[2]; rx = r[3]; ry = r[4];
        l2 = r[8]; r2 = r[9];
        btn |= (uint32_t)(r[5] >> 4) & 0x0F;          // square cross circle tri
        btn |= (uint32_t)r[6] << 4;                    // shoulders, sticks, menu
        btn |= (uint32_t)(r[7] & 0x03) << 12;          // PS, touchpad
        btn |= psHatToButtons(r[5] & 0x0F);
    }
    else if (type == RPT_DS5) {
        if (len < 11) return;
        lx = r[1]; ly = r[2]; rx = r[3]; ry = r[4];
        l2 = r[5]; r2 = r[6];
        btn |= (uint32_t)(r[8] >> 4) & 0x0F;
        btn |= (uint32_t)r[9] << 4;
        btn |= (uint32_t)(r[10] & 0x03) << 12;
        btn |= psHatToButtons(r[8] & 0x0F);
    }
    else {                                             // DS3
        if (len < 20) return;
        lx = r[6]; ly = r[7]; rx = r[8]; ry = r[9];
        l2 = r[18]; r2 = r[19];
        // DS3 orders its bits differently; remap onto the shared numbering.
        uint8_t b2 = r[2], b3 = r[3];
        if (b3 & 0x80) btn |= 1u << 0;                 // square
        if (b3 & 0x40) btn |= 1u << 1;                 // cross
        if (b3 & 0x20) btn |= 1u << 2;                 // circle
        if (b3 & 0x10) btn |= 1u << 3;                 // triangle
        if (b3 & 0x04) btn |= 1u << 4;                 // L1
        if (b3 & 0x08) btn |= 1u << 5;                 // R1
        if (b3 & 0x01) btn |= 1u << 6;                 // L2
        if (b3 & 0x02) btn |= 1u << 7;                 // R2
        if (b2 & 0x01) btn |= 1u << 8;                 // select
        if (b2 & 0x08) btn |= 1u << 9;                 // start
        if (b2 & 0x02) btn |= 1u << 10;                // L3
        if (b2 & 0x04) btn |= 1u << 11;                // R3
        if (r[4] & 0x01) btn |= 1u << 12;              // PS
        if (b2 & 0x10) btn |= 1u << 14;                // D-up
        if (b2 & 0x20) btn |= 1u << 15;                // D-right
        if (b2 & 0x40) btn |= 1u << 16;                // D-down
        if (b2 & 0x80) btn |= 1u << 17;                // D-left
    }

    // Sticks are unsigned bytes centred on 128; triggers rest at 0.
    _work.axes[0] = (int16_t)(((int32_t)lx - 128) * 256);
    _work.axes[1] = (int16_t)(((int32_t)ly - 128) * 256);
    _work.axes[2] = (int16_t)(((int32_t)rx - 128) * 256);
    _work.axes[3] = (int16_t)(((int32_t)ry - 128) * 256);
    _work.axes[4] = (int16_t)(((int32_t)l2 * 257) - 32768);
    _work.axes[5] = (int16_t)(((int32_t)r2 * 257) - 32768);
    _work.num_axes    = 6;
    _work.buttons     = btn;
    _work.num_buttons = PS_BTN_COUNT;
    edges(btn);
    _work.connected = true;
    _work.failsafe  = false;
    _work.source    = InputSource::HID_GAMEPAD;
    _last_recv      = millis();
    publish();
}

// ─── SHARED: unpack 16x11-bit CRSF channels into axes ────────────────────────
void InputManager::storeCRSFChannels(const uint8_t* p, InputSource src) {
    uint32_t bits = 0;
    int      bpos = 0;
    for (int i = 0; i < 16 && i < MAX_INPUT_AXES; i++) {
        while (bpos < 11) { bits |= ((uint32_t)(*p++) << bpos); bpos += 8; }
        uint16_t ch = bits & 0x7FF;
        bits >>= 11; bpos -= 11;
        int32_t v = ((int32_t)ch - CRSF_CHANNEL_MID) * 40;
        _work.axes[i] = (int16_t)constrain(v, -32768, 32767);
    }
    _work.num_axes    = 16;
    _work.num_buttons = 0;
    _work.connected   = true;
    _work.failsafe    = false;
    _work.source      = src;
    _last_recv        = millis();
    publish();
}

// ─── T8L / CDC CRSF ──────────────────────────────────────────────────────────
void InputManager::parseCDCByte(uint8_t b) {
    if (_cdc_len == 0 &&
        b != CRSF_ADDR_FC && b != CRSF_ADDR_HANDSET &&
        b != CRSF_ADDR_BROADCAST && b != CRSF_ADDR_TX_MODULE) return;
    if (_cdc_len < sizeof(_cdc_buf)) _cdc_buf[_cdc_len++] = b;

    if (_cdc_len >= 2) {
        uint16_t expected = (uint16_t)_cdc_buf[1] + 2u;
        if (expected < 4 || expected > sizeof(_cdc_buf)) { _cdc_len = 0; return; }
        if (_cdc_len >= expected) {
            if (crsfCRC8(&_cdc_buf[2], _cdc_buf[1] - 1) == _cdc_buf[expected - 1] &&
                _cdc_buf[2] == CRSF_FRAMETYPE_RC_CHANNELS_PACKED)
                storeCRSFChannels(&_cdc_buf[3], InputSource::CDC_CRSF);
            _cdc_len = 0;
        }
    }
}

// ─── SBUS ────────────────────────────────────────────────────────────────────
void InputManager::parseSBUSByte(uint8_t b) {
    if (_sbus_len == 0 && b != SBUS_HEADER) return;
    _sbus_buf[_sbus_len++] = b;

    if (_sbus_len >= SBUS_FRAME_LEN) {
        if (_sbus_buf[0] == SBUS_HEADER && _sbus_buf[24] == SBUS_FOOTER) {
            const uint8_t* d = &_sbus_buf[1];
            uint32_t bits = 0; int bpos = 0;
            for (int i = 0; i < 16 && i < MAX_INPUT_AXES; i++) {
                while (bpos < 11) { bits |= ((uint32_t)(*d++) << bpos); bpos += 8; }
                uint16_t raw = bits & 0x7FF;      // 0..2047, centre 1024
                bits >>= 11; bpos -= 11;
                int32_t v = ((int32_t)raw - 1024) * 32;
                _work.axes[i] = (int16_t)constrain(v, -32768, 32767);
            }
            _work.num_axes    = 16;
            _work.num_buttons = 0;
            _work.failsafe    = (_sbus_buf[23] & SBUS_FLAG_FAILSAFE) != 0;
            _work.connected   = true;
            _work.source      = InputSource::SBUS;
            _last_recv        = millis();
            publish();
        }
        _sbus_len = 0;
    }
}

// ─── ELRS RECEIVER CRSF ──────────────────────────────────────────────────────
void InputManager::parseCRSFRxByte(uint8_t b) {
    if (_crsf_len == 0 && b != CRSF_ADDR_FC && b != CRSF_ADDR_BROADCAST) return;
    if (_crsf_len < sizeof(_crsf_buf)) _crsf_buf[_crsf_len++] = b;

    if (_crsf_len >= 2) {
        uint16_t expected = (uint16_t)_crsf_buf[1] + 2u;
        if (expected < 4 || expected > sizeof(_crsf_buf)) { _crsf_len = 0; return; }
        if (_crsf_len >= expected) {
            if (crsfCRC8(&_crsf_buf[2], _crsf_buf[1] - 1) == _crsf_buf[expected - 1] &&
                _crsf_buf[2] == CRSF_FRAMETYPE_RC_CHANNELS_PACKED)
                storeCRSFChannels(&_crsf_buf[3], InputSource::ELRS_RX);
            _crsf_len = 0;
        }
    }
}

// ─── POT/BUTTON UART ──────────────────────────────────────────────────────────
// SYNC1 SYNC2 NUM_POTS NUM_BUTTONS, then NUM_POTS x uint16 LE pot values
// (0..65535, mapped straight onto the -32768..32767 axis range), then
// ceil(NUM_BUTTONS/8) packed button bytes, then an 8-bit sum checksum over
// every byte from NUM_POTS through the end of the button bitfield. See
// README.txt for the full frame layout and the matching sender sketch.
void InputManager::parsePotBtnByte(uint8_t b) {
    if (_pb_sync == 0) {
        if (b == POT_BTN_SYNC1) _pb_sync = 1;
        return;
    }
    if (_pb_sync == 1) {
        if (b == POT_BTN_SYNC2) { _pb_sync = 2; _pb_len = 0; _pb_need = 0; }
        else if (b != POT_BTN_SYNC1) _pb_sync = 0;   // else: stay put, could be SYNC1 again
        return;
    }

    // _pb_sync == 2: collecting the header, then the body it describes.
    if (_pb_len < sizeof(_pb_buf)) _pb_buf[_pb_len++] = b;
    else { _pb_sync = 0; _pb_len = 0; return; }        // longer than any legal frame

    if (_pb_len == 2 && _pb_need == 0) {
        uint8_t num_pots = _pb_buf[0];
        uint8_t num_btns = _pb_buf[1];
        if (num_pots > MAX_INPUT_AXES || num_btns > MAX_INPUT_BUTTONS) {
            _pb_sync = 0; _pb_len = 0;                 // packet claims more than we support
            return;
        }
        uint8_t btn_bytes = (uint8_t)((num_btns + 7) / 8);
        _pb_need = (uint16_t)2 + (uint16_t)num_pots * 2 + btn_bytes + 1;  // + checksum byte
        if (_pb_need > sizeof(_pb_buf)) { _pb_sync = 0; _pb_len = 0; _pb_need = 0; return; }
    }

    if (_pb_need && _pb_len >= _pb_need) {
        uint8_t  num_pots  = _pb_buf[0];
        uint8_t  num_btns  = _pb_buf[1];
        uint16_t body_len  = _pb_need - 1;   // everything but the trailing checksum
        uint8_t  sum = 0;
        for (uint16_t i = 0; i < body_len; i++) sum += _pb_buf[i];

        if (sum == _pb_buf[_pb_need - 1]) {
            const uint8_t* pots = &_pb_buf[2];
            uint8_t na = (num_pots > MAX_INPUT_AXES) ? MAX_INPUT_AXES : num_pots;
            for (uint8_t i = 0; i < na; i++) {
                uint16_t raw = (uint16_t)pots[i * 2] | ((uint16_t)pots[i * 2 + 1] << 8);
                _work.axes[i] = (int16_t)((int32_t)raw - 32768);
            }

            const uint8_t* btnbits = &pots[(uint16_t)num_pots * 2];
            uint8_t  nb  = (num_btns > MAX_INPUT_BUTTONS) ? MAX_INPUT_BUTTONS : num_btns;
            uint32_t btn = 0;
            for (uint8_t i = 0; i < nb; i++)
                if (btnbits[i >> 3] & (1u << (i & 7))) btn |= (1UL << i);

            _work.num_axes    = na;
            _work.buttons     = btn;
            _work.num_buttons = nb;
            edges(btn);
            _work.connected = true;
            _work.failsafe  = false;
            _work.source    = InputSource::POT_BTN;
            _last_recv      = millis();
            publish();
        }
        // Whether or not the checksum matched, this frame is done - look for
        // the next SYNC1/SYNC2 pair rather than trying to resync mid-body.
        _pb_sync = 0; _pb_len = 0; _pb_need = 0;
    }
}

// ─── STICKY BUTTON LATCHING ──────────────────────────────────────────────────
void InputManager::edges(uint32_t nb) {
    uint32_t rising = nb & ~_prev_buttons;
    for (uint8_t i = 0; i < MAX_INPUT_BUTTONS; i++)
        if (rising & (1UL << i)) _work.toggles[i] = !_work.toggles[i];
    _prev_buttons = nb;
}

// ─── TRIM BUTTONS ────────────────────────────────────────────────────────────
bool ChannelMixer::applyTrimButtons(ChannelConfig cfg[CRSF_NUM_CHANNELS],
                                     const InputState& in) {
    // Nothing to act on without a live input. Tracking the state anyway means
    // a button still held when the controller comes back does not read as a
    // fresh press and silently step the trim.
    if (!in.connected) { _trim_prev = in.buttons; _trim_stopped = 0; return false; }

    // A press is one step; holding repeats like EdgeTX's trim keys: first
    // repeat after 400 ms, then every 100 ms, every 50 ms after ten repeats.
    // Crossing centre snaps to exactly 0 and parks the repeat there until the
    // button is released and pressed again.
    const uint32_t now     = millis();
    uint32_t       rising  = in.buttons & ~_trim_prev;
    uint32_t       held    = in.buttons & _trim_prev;
    _trim_prev = in.buttons;
    _trim_stopped &= in.buttons;                       // a release clears the park
    uint32_t fire = rising;
    for (uint8_t b = 0; b < MAX_INPUT_BUTTONS; b++) {
        uint32_t bit = 1UL << b;
        if (rising & bit) { _trim_next[b] = now + 400; _trim_reps[b] = 0; }
        else if ((held & bit) && !(_trim_stopped & bit) && (int32_t)(now - _trim_next[b]) >= 0) {
            fire |= bit;
            _trim_next[b] = now + (_trim_reps[b] >= 10 ? 50 : 100);
            if (_trim_reps[b] < 255) _trim_reps[b]++;
        }
    }
    if (!fire) return false;

    bool changed = false;
    for (uint8_t i = 0; i < CRSF_NUM_CHANNELS; i++) {
        ChannelConfig& c = cfg[i];
        int16_t step = (c.trim_step ? c.trim_step : 1);
        int16_t t    = c.trim;

        int16_t before = t;
        if (c.trim_up != TRIM_BTN_NONE && c.trim_up < in.num_buttons &&
            (fire & (1UL << c.trim_up)))  t += step;
        if (c.trim_dn != TRIM_BTN_NONE && c.trim_dn < in.num_buttons &&
            (fire & (1UL << c.trim_dn)))  t -= step;

        // Crossing centre: land exactly on 0 and park a repeating button there.
        if ((before < 0 && t > 0) || (before > 0 && t < 0)) {
            t = 0;
            if (c.trim_up != TRIM_BTN_NONE && c.trim_up < MAX_INPUT_BUTTONS) _trim_stopped |= (1UL << c.trim_up);
            if (c.trim_dn != TRIM_BTN_NONE && c.trim_dn < MAX_INPUT_BUTTONS) _trim_stopped |= (1UL << c.trim_dn);
        }

        t = constrain(t, -100, 100);
        if (t != c.trim) { c.trim = (int8_t)t; changed = true; }
    }
    return changed;
}

// ─── EXPO CURVE ──────────────────────────────────────────────────────────────
// Positive expo softens the response around centre (fine control near the
// middle, full travel still reachable at the ends). Negative expo does the
// opposite, making the centre more sensitive.
static int32_t applyExpo(int32_t v, int8_t expo) {
    if (expo == 0) return v;

    float x = (float)v / 32768.0f;
    if (x >  1.0f) x =  1.0f;
    if (x < -1.0f) x = -1.0f;

    float e = (float)expo / 100.0f;
    float y;
    if (e > 0.0f) {
        // blend linear with a cubic
        y = x * (1.0f - e) + (x * x * x) * e;
    } else {
        // blend linear with a square-root curve
        float a = -e;
        float s = (x < 0.0f) ? -1.0f : 1.0f;
        y = x * (1.0f - a) + s * sqrtf(fabsf(x)) * a;
    }
    return (int32_t)(y * 32768.0f);
}

// ─── 5-POINT CURVE ───────────────────────────────────────────────────────────
// Points are spaced evenly across the input range; interpolate between the two
// that bracket the current value.
static int32_t applyCurve(int32_t v, const int8_t* pts) {
    float x = (float)v / 32768.0f;                 // -1 .. +1
    if (x >  1.0f) x =  1.0f;
    if (x < -1.0f) x = -1.0f;

    float pos = (x + 1.0f) * 2.0f;                 // 0 .. 4
    int   i   = (int)pos;
    if (i > 3) i = 3;
    float f   = pos - (float)i;

    float y0 = (float)pts[i]     / 100.0f;
    float y1 = (float)pts[i + 1] / 100.0f;
    float y  = y0 + (y1 - y0) * f;

    return (int32_t)(y * 32768.0f);
}

// ─── MIXER ───────────────────────────────────────────────────────────────────
// Order: base + mix slots -> curve or expo -> weight -> trim -> CRSF units.
void ChannelMixer::mix(const ChannelConfig cfg[CRSF_NUM_CHANNELS],
                        const InputState&  in,
                        uint16_t           out[CRSF_NUM_CHANNELS]) {
    // Only treat this as failsafe once a working link has actually existed and
    // then gone away. Before the first good data - at boot, or with a device
    // that never asserts the flag - mix normally instead of pinning every
    // channel to its failsafe value, which looks like a dead board.
    if (in.connected) _seen_link = true;
    _fs = _seen_link && ((!in.connected) || in.failsafe);

    for (int i = 0; i < CRSF_NUM_CHANNELS; i++) {
        const ChannelConfig& c = cfg[i];

        // ── Failsafe ────────────────────────────────────────────────────────
        if (_fs) {
            if (c.fs_mode == FailsafeMode::VALUE) {
                int32_t v = map((int32_t)c.fs_value, -100, 100,
                                CRSF_CHANNEL_MIN, CRSF_CHANNEL_MAX);
                out[i] = (uint16_t)constrain(v, CRSF_CHANNEL_MIN, CRSF_CHANNEL_MAX);
            } else {
                out[i] = _have ? _last[i] : CRSF_CHANNEL_MID;
            }
            continue;
        }

        // ── Mix ─────────────────────────────────────────────────────────────
        int32_t base = mixTargetValue(c.base);
        int32_t v    = base;

        for (uint8_t k = 0; k < MAX_MIX_SOURCES; k++) {
            const MixSource& m = c.src[k];
            if (m.type == SourceType::NONE || m.weight == 0) continue;

            if (m.type == SourceType::AXIS) {
                if (m.index >= in.num_axes) continue;
                v += ((int32_t)in.axes[m.index] * m.weight) / 100;
            } else {
                if (m.index >= in.num_buttons) continue;
                bool on = (m.btn_mode == ButtonMode::STICKY)
                            ? in.toggles[m.index]
                            : (bool)((in.buttons >> m.index) & 1u);
                if (!on) continue;
                v += ((mixTargetValue(m.target) - base) * m.weight) / 100;
            }
        }

        v = constrain(v, -32768, 32767);
        v = c.curve_on ? applyCurve(v, c.curve) : applyExpo(v, c.expo);
        v = (v * c.weight) / 100;              // channel travel, sign reverses
        v += ((int32_t)c.trim * 32768) / 100;  // centre offset, not scaled by weight
        v = constrain(v, -32768, 32767);

        int32_t o = map(v, -32768, 32767, CRSF_CHANNEL_MIN, CRSF_CHANNEL_MAX);
        out[i] = (uint16_t)constrain(o, CRSF_CHANNEL_MIN, CRSF_CHANNEL_MAX);
    }

    if (!_fs) {
        memcpy(_last, out, sizeof(_last));
        _have = true;
    }
}
