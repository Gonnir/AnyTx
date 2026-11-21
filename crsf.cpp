#include "crsf.h"
#include <hardware/gpio.h>
#include <SerialPIO.h>

static const uint8_t CRC8_TABLE[256] = {
    0x00,0xD5,0x7F,0xAA,0xFE,0x2B,0x81,0x54,0x29,0xFC,0x56,0x83,0xD7,0x02,0xA8,0x7D,
    0x52,0x87,0x2D,0xF8,0xAC,0x79,0xD3,0x06,0x7B,0xAE,0x04,0xD1,0x85,0x50,0xFA,0x2F,
    0xA4,0x71,0xDB,0x0E,0x5A,0x8F,0x25,0xF0,0x8D,0x58,0xF2,0x27,0x73,0xA6,0x0C,0xD9,
    0xF6,0x23,0x89,0x5C,0x08,0xDD,0x77,0xA2,0xDF,0x0A,0xA0,0x75,0x21,0xF4,0x5E,0x8B,
    0x9D,0x48,0xE2,0x37,0x63,0xB6,0x1C,0xC9,0xB4,0x61,0xCB,0x1E,0x4A,0x9F,0x35,0xE0,
    0xCF,0x1A,0xB0,0x65,0x31,0xE4,0x4E,0x9B,0xE6,0x33,0x99,0x4C,0x18,0xCD,0x67,0xB2,
    0x39,0xEC,0x46,0x93,0xC7,0x12,0xB8,0x6D,0x10,0xC5,0x6F,0xBA,0xEE,0x3B,0x91,0x44,
    0x6B,0xBE,0x14,0xC1,0x95,0x40,0xEA,0x3F,0x42,0x97,0x3D,0xE8,0xBC,0x69,0xC3,0x16,
    0xEF,0x3A,0x90,0x45,0x11,0xC4,0x6E,0xBB,0xC6,0x13,0xB9,0x6C,0x38,0xED,0x47,0x92,
    0xBD,0x68,0xC2,0x17,0x43,0x96,0x3C,0xE9,0x94,0x41,0xEB,0x3E,0x6A,0xBF,0x15,0xC0,
    0x4B,0x9E,0x34,0xE1,0xB5,0x60,0xCA,0x1F,0x62,0xB7,0x1D,0xC8,0x9C,0x49,0xE3,0x36,
    0x19,0xCC,0x66,0xB3,0xE7,0x32,0x98,0x4D,0x30,0xE5,0x4F,0x9A,0xCE,0x1B,0xB1,0x64,
    0x72,0xA7,0x0D,0xD8,0x8C,0x59,0xF3,0x26,0x5B,0x8E,0x24,0xF1,0xA5,0x70,0xDA,0x0F,
    0x20,0xF5,0x5F,0x8A,0xDE,0x0B,0xA1,0x74,0x09,0xDC,0x76,0xA3,0xF7,0x22,0x88,0x5D,
    0xD6,0x03,0xA9,0x7C,0x28,0xFD,0x57,0x82,0xFF,0x2A,0x80,0x55,0x01,0xD4,0x7E,0xAB,
    0x84,0x51,0xFB,0x2E,0x7A,0xAF,0x05,0xD0,0xAD,0x78,0xD2,0x07,0x53,0x86,0x2C,0xF9
};

uint8_t crsfCRC8(const uint8_t* d, uint8_t len) {
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++) crc = CRC8_TABLE[crc ^ d[i]];
    return crc;
}
uint8_t CRSFEngine::crsf_crc8(const uint8_t* d, uint8_t l) { return crsfCRC8(d, l); }

// Receive-only PIO UART used in single-pin mode. Constructed here but only
// started when that mode is selected.
static SerialPIO _crsf_rx_pio((pin_size_t)0xFF, PIN_CRSF_TX, 256);

void CRSFEngine::begin(HardwareSerial& serial, uint32_t baud,
                        bool invert, bool half_duplex, bool single_pin) {
    _serial      = &serial;
    _half_duplex = half_duplex || single_pin;   // one pin implies half duplex
    _single_pin  = single_pin;

    _serial->begin(baud);

    if (_single_pin) {
        // Both directions share PIN_CRSF_TX. This works because on the RP2040
        // the GPIO input path feeds every peripheral regardless of which one
        // the function mux has driving the pad - so a PIO receiver can sample
        // the pin while the hardware UART is transmitting on it, and continue
        // sampling once the UART lets go. GP1 is not used at all.
        _crsf_rx_pio.begin(baud);   // this claims the pin for PIO
        _rx = &_crsf_rx_pio;
    } else {
        _rx = _serial;
    }

    setInvert(invert);
    // Half duplex starts with the line free (sendFrame() grabs it per frame).
    // Full duplex must keep GP0 on the UART permanently: releaseTX() hands the
    // pad to SIO as an input, and nothing would ever re-claim it, so a plain
    // two-wire module would receive no frames at all after a reboot.
    if (_half_duplex) releaseTX();
    else              acquireTX();
}

// ─── LINE POLARITY ───────────────────────────────────────────────────────────
// S.Port-style wiring needs inverted TTL; some modules want plain UART levels.
// The RP2040 pad override does this in hardware, no external inverter needed.
// The reply travels on the same wire as the transmission, so it carries the
// same polarity: whatever inversion the output needs, the input needs too.
//
// This has to be re-asserted after every gpio_set_function() call, because
// that writes the pin's whole control register and clears the OUTOVER, OEOVER
// and INOVER fields along with it. Missing that is why single-pin mode saw
// nothing: releasing the line wiped the input inversion, and every reply
// afterwards decoded as noise.
void CRSFEngine::applyOverrides() {
    gpio_set_outover(PIN_CRSF_TX,
                     _invert ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
    // Single-pin mode samples the transmit pin, so the input override belongs
    // there rather than on GP1. Note this follows _rx_invert, not _invert.
    gpio_set_inover(_single_pin ? PIN_CRSF_TX : PIN_CRSF_RX,
                    _rx_invert ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
}

void CRSFEngine::setInvert(bool inv) {
    _invert    = inv;
    _rx_invert = inv;     // sensible starting point; the search may change it
    applyOverrides();
}

void CRSFEngine::setHalfDuplex(bool hd) {
    _half_duplex = hd;
    if (hd) releaseTX();
    else    acquireTX();
}

// ─── HALF-DUPLEX LINE ARBITRATION ────────────────────────────────────────────
// On a single-wire JR module GP0 and GP1 sit on the same wire.  An inverted
// UART idles LOW, meaning the TX pad would sit there actively pulling the
// shared line down and the module could never drive it high to reply.
// So between frames we hand GP0 back to the SIO block as a plain input,
// leaving the line free, and only reconnect it to the UART while sending.
void CRSFEngine::acquireTX() {
    gpio_set_function(PIN_CRSF_TX, GPIO_FUNC_UART);
    applyOverrides();          // funcsel wiped them
}

void CRSFEngine::releaseTX() {
    gpio_set_function(PIN_CRSF_TX, GPIO_FUNC_SIO);
    gpio_set_dir(PIN_CRSF_TX, GPIO_IN);
    applyOverrides();          // funcsel wiped them - this is the important one

    // Hold the line at its idle level while nobody is driving it. An inverted
    // link idles physically low, a normal one high. Without this the pin
    // floats between frames and the receiver decodes noise as start bits.
    // Idle level matches the transmit polarity: a normal link idles high, an
    // inverted one low. The module can still pull against this weak pull.
    if (_invert) gpio_pull_down(PIN_CRSF_TX);
    else         gpio_pull_up(PIN_CRSF_TX);
}

void CRSFEngine::sendFrame(uint8_t dest, uint8_t type,
                            const uint8_t* pl, uint8_t len) {
    if (!_serial || _suspended || (uint16_t)len + 4u > 64u) return;   // a companion session owns the UART
    uint8_t f[64];
    f[0] = dest;
    f[1] = (uint8_t)(len + 2);
    f[2] = type;
    memcpy(&f[3], pl, len);
    f[3 + len] = crsf_crc8(&f[2], len + 1);
    if (_half_duplex) acquireTX();
    _serial->write(f, len + 4);
    _serial->flush();
    // flush() empties the FIFO but the shift register may still hold a byte;
    // one byte at 420 kbaud is ~24 us, so 60 us is a safe margin.
    delayMicroseconds(60);

    // Let go of the line FIRST, before doing anything else. A module can start
    // answering within tens of microseconds of our last bit, and while this
    // pin is still driven it physically cannot pull the wire low - so any time
    // spent here holding the line is time its reply is being suppressed.
    if (_half_duplex) {
        releaseTX();
        _released    = true;
        _released_at = micros();
    }

    // Now clear our own transmission out of the receive FIFO. It is already
    // buffered, so draining after releasing the line costs nothing. Discard
    // exactly as many bytes as we sent: leaving any behind glues them onto the
    // front of the reply and every frame afterwards fails its CRC, while
    // draining more would eat the reply itself.
    // Until the receive path has proved itself, let one transmission through
    // to the parser rather than draining it. Our own frame is a known-good
    // test signal, so if it comes back and passes CRC then the receiver, the
    // baud rate and the polarity are all confirmed working.
    // Full duplex has no echo at all: the module's reply (which arrives on its
    // own wire) must reach the parser untouched - treating it as 'our own frame
    // coming back' would swallow the very first answer and drain real bytes.
    if (!_half_duplex) return;

    if (_auto_pol && !_echo_pass && type == CRSF_FRAMETYPE_DEVICE_PING) {
        _selftest = true;
        _rx_len   = 0;
        return;
    }

    uint16_t to_drop = (uint16_t)len + 4;      // what we just put on the wire
    uint32_t t0      = micros();
    while (to_drop && (micros() - t0) < 600) {
        if (_rx && _rx->available()) { (void)_rx->read(); to_drop--; }
    }

    // Start the frame assembler from a known-clean state.
    _rx_len = 0;
}

void CRSFEngine::setChannels(const uint16_t ch[CRSF_NUM_CHANNELS]) {
    memcpy(_ch, ch, sizeof(_ch));
}

void CRSFEngine::sendRCChannels() {
    uint8_t  pl[22] = {};
    uint32_t bits = 0;
    int bpos = 0, bidx = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t v = constrain(_ch[i], CRSF_CHANNEL_MIN, CRSF_CHANNEL_MAX);
        bits |= (v << bpos);
        bpos += 11;
        while (bpos >= 8 && bidx < 22) {
            pl[bidx++] = bits & 0xFF;
            bits >>= 8; bpos -= 8;
        }
    }
    sendFrame(CRSF_ADDR_TX_MODULE, CRSF_FRAMETYPE_RC_CHANNELS_PACKED, pl, 22);
}

void CRSFEngine::sendOpenTXSync() {
    uint8_t p[8] = {};
    uint32_t hz = 1000u / (_rc_interval ? _rc_interval : RC_INTERVAL_MS);
    memcpy(p, &hz, 4);
    sendFrame(CRSF_ADDR_TX_MODULE, CRSF_FRAMETYPE_OPENTX_SYNC, p, 8);
}

// Ask the module to identify itself, then stay off the wire so it can answer.
void CRSFEngine::pingDevice() {
    // Alternate the ping's destination between broadcast (what EdgeTX sends)
    // and the TX module's own address; some ELRS versions answer only one.
    static bool alt = false;
    alt = !alt;
    uint8_t p[2] = { alt ? (uint8_t)CRSF_ADDR_BROADCAST : (uint8_t)CRSF_ADDR_TX_MODULE, CRSF_ADDR_HANDSET };
    sendFrame(CRSF_ADDR_TX_MODULE, CRSF_FRAMETYPE_DEVICE_PING, p, 2);
    _quiet_until = millis() + QUIET_MS;
}

void CRSFEngine::writeParamCommand(uint8_t idx) {
    uint8_t p[5] = { CRSF_ADDR_TX_MODULE, CRSF_ADDR_HANDSET, idx, 0, 1 };
    sendFrame(CRSF_ADDR_TX_MODULE, CRSF_FRAMETYPE_PARAM_WRITE, p, 5);
    _quiet_until = millis() + QUIET_MS;
}

void CRSFEngine::sendModelId(uint8_t id) {
    uint8_t p[4] = { CRSF_ADDR_TX_MODULE, CRSF_ADDR_HANDSET, 0, id };   // field id 0 - see crsf.h
    sendFrame(CRSF_ADDR_TX_MODULE, CRSF_FRAMETYPE_PARAM_WRITE, p, 4);
    _quiet_until = millis() + QUIET_MS;
}

void CRSFEngine::writeParamValue(const LiveParam& p, int32_t value) {
    uint8_t buf[7] = { CRSF_ADDR_TX_MODULE, CRSF_ADDR_HANDSET, p.field_id, 0, 0, 0, 0 };
    uint8_t n = 3;
    switch (p.type) {
        case ParamType::P_UINT8: case ParamType::P_INT8: case ParamType::P_TEXT_SELECTION:
            buf[n++] = (uint8_t)value;
            break;
        case ParamType::P_UINT16: case ParamType::P_INT16:
            buf[n++] = (uint8_t)(value >> 8);
            buf[n++] = (uint8_t)value;
            break;
        case ParamType::P_UINT32: case ParamType::P_INT32: case ParamType::P_FLOAT:
            buf[n++] = (uint8_t)(value >> 24);
            buf[n++] = (uint8_t)(value >> 16);
            buf[n++] = (uint8_t)(value >> 8);
            buf[n++] = (uint8_t)value;
            break;
        default:
            return;   // folders/commands/strings/info aren't written this way
    }
    sendFrame(CRSF_ADDR_TX_MODULE, CRSF_FRAMETYPE_PARAM_WRITE, buf, n);
    _quiet_until = millis() + QUIET_MS;
}

void CRSFEngine::writeParamString(const LiveParam& p, const char* str) {
    uint8_t buf[35] = { CRSF_ADDR_TX_MODULE, CRSF_ADDR_HANDSET, p.field_id };
    uint8_t n = 3;
    while (*str && n < sizeof(buf) - 1) buf[n++] = (uint8_t)*str++;
    buf[n++] = 0;
    sendFrame(CRSF_ADDR_TX_MODULE, CRSF_FRAMETYPE_PARAM_WRITE, buf, n);
    _quiet_until = millis() + QUIET_MS;
}

void CRSFEngine::sendParamReadChunk(uint8_t field_id, uint8_t chunk) {
    uint8_t p[4] = { CRSF_ADDR_TX_MODULE, CRSF_ADDR_HANDSET, field_id, chunk };
    sendFrame(CRSF_ADDR_TX_MODULE, CRSF_FRAMETYPE_PARAM_READ, p, 4);
    _quiet_until = millis() + QUIET_MS;
}

void CRSFEngine::beginDiscovery() {
    _live_count       = 0;
    _disc_field       = 1;
    _disc_chunk       = 0;
    _disc_sent        = false;
    _disc_got         = false;
    _asm_len          = 0;
    _disc_wait_since  = millis();
    _disc_active      = true;
    // Ask again in case the module was already found earlier and its
    // DEVICE_INFO (and field count) was never captured this session.
    if (_field_count == 0) pingDevice();
}

LiveParam& CRSFEngine::liveSlot(uint8_t field_id) {
    for (uint8_t i = 0; i < _live_count; i++)
        if (_live[i].field_id == field_id) return _live[i];
    if (_live_count < MAX_LIVE_PARAMS) {
        _live[_live_count] = LiveParam{};
        _live[_live_count].field_id = field_id;
        return _live[_live_count++];
    }
    return _live[MAX_LIVE_PARAMS - 1];   // out of room - overwrite rather than overflow
}

const LiveParam* CRSFEngine::liveByField(uint8_t field_id) const {
    for (uint8_t i = 0; i < _live_count; i++)
        if (_live[i].field_id == field_id) return &_live[i];
    return nullptr;
}

// Big-endian byte read, used for the 16/32-bit numeric and float payloads.
static int32_t beRead(const uint8_t* p, uint8_t width) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < width; i++) v = (v << 8) | p[i];
    return (int32_t)v;
}

void CRSFEngine::finishAssembly() {
    LiveParam& lp = liveSlot(_asm_field_id);
    lp.field_id = _asm_field_id;
    lp.parent   = _asm_parent;
    lp.type     = _asm_type;
    lp.hidden   = _asm_hidden;

    // The name is a null-terminated string at the front of the assembly
    // buffer; everything after its terminator is type-specific payload.
    uint16_t ni = 0;
    while (ni < _asm_len && _asm_buf[ni] != 0) ni++;
    uint16_t namelen = ni < sizeof(lp.name) ? ni : sizeof(lp.name) - 1;
    memcpy(lp.name, _asm_buf, namelen);
    lp.name[namelen] = 0;

    const uint8_t* d    = _asm_buf + (ni < _asm_len ? ni + 1 : _asm_len);
    uint16_t       dlen = (ni < _asm_len) ? (uint16_t)(_asm_len - ni - 1) : 0;

    lp.text[0]  = 0;
    lp.value = lp.vmin = lp.vmax = lp.step = 0;
    lp.decimals   = 0;
    lp.cmd_status = 0;

    auto copyTail = [&](const uint8_t* p, uint16_t len) {
        uint16_t n = len < sizeof(lp.text) - 1 ? len : sizeof(lp.text) - 1;
        memcpy(lp.text, p, n);
        lp.text[n] = 0;
    };

    switch (lp.type) {
        case ParamType::P_UINT8:
        case ParamType::P_INT8:
            if (dlen >= 3) {
                if (lp.type == ParamType::P_INT8) {
                    lp.value = (int8_t)d[0]; lp.vmin = (int8_t)d[1]; lp.vmax = (int8_t)d[2];
                } else {
                    lp.value = d[0]; lp.vmin = d[1]; lp.vmax = d[2];
                }
                if (dlen > 3) copyTail(d + 3, dlen - 3);   // optional units suffix
            }
            break;

        case ParamType::P_UINT16:
        case ParamType::P_INT16:
            if (dlen >= 6) {
                int32_t v = beRead(d, 2), mn = beRead(d + 2, 2), mx = beRead(d + 4, 2);
                if (lp.type == ParamType::P_INT16) { v = (int16_t)v; mn = (int16_t)mn; mx = (int16_t)mx; }
                lp.value = v; lp.vmin = mn; lp.vmax = mx;
                if (dlen > 6) copyTail(d + 6, dlen - 6);
            }
            break;

        case ParamType::P_UINT32:
        case ParamType::P_INT32:
            // New in ELRS 4.1 (16/32-bit parameter editing) - same layout as
            // the 8/16-bit cases, just four bytes wide.
            if (dlen >= 12) {
                lp.value = beRead(d, 4); lp.vmin = beRead(d + 4, 4); lp.vmax = beRead(d + 8, 4);
                if (dlen > 12) copyTail(d + 12, dlen - 12);
            }
            break;

        case ParamType::P_FLOAT:
            if (dlen >= 13) {
                lp.value    = beRead(d, 4);
                lp.vmin     = beRead(d + 4, 4);
                lp.vmax     = beRead(d + 8, 4);
                lp.decimals = d[12];
                uint16_t used = 13;
                if (dlen >= 17) { lp.step = beRead(d + 13, 4); used = 17; }
                if (dlen > used) copyTail(d + used, dlen - used);
            }
            break;

        case ParamType::P_TEXT_SELECTION: {
            // Pipe-of-options string first, then value/min/max as one byte
            // each, optionally followed by a units suffix.
            uint16_t oi = 0;
            while (oi < dlen && d[oi] != 0) oi++;
            copyTail(d, oi);
            const uint8_t* rest = d + (oi < dlen ? oi + 1 : dlen);
            uint16_t       rlen = (oi < dlen) ? (uint16_t)(dlen - oi - 1) : 0;
            if (rlen >= 3) { lp.value = rest[0]; lp.vmin = rest[1]; lp.vmax = rest[2]; }
            break;
        }

        case ParamType::P_STRING: {
            uint16_t si = 0;
            while (si < dlen && d[si] != 0) si++;
            copyTail(d, si);
            if (si < dlen) lp.vmax = d[si];   // max length byte, if present
            break;
        }

        case ParamType::P_INFO:
            copyTail(d, dlen);
            break;

        case ParamType::P_COMMAND:
            if (dlen >= 2) {
                lp.cmd_status = d[0];
                lp.step       = d[1];          // timeout, in 100 ms steps
                if (dlen > 2) copyTail(d + 2, dlen - 2);
            }
            break;

        case ParamType::P_FOLDER:
        default:
            break;
    }
}

void CRSFEngine::handleSettingsEntry(const uint8_t* pl, uint8_t pl_len) {
    if (pl_len < 2) return;
    uint8_t field_id         = pl[0];
    uint8_t chunks_remaining = pl[1];

    // A reply matching the field we're actively waiting on during discovery
    // feeds the assembly buffer.
    if (_disc_active && _disc_sent && field_id == _disc_field) {
        if (_disc_chunk == 0) {
            if (pl_len < 4) return;   // too short to even have a type byte - ignore
            _asm_field_id = field_id;
            _asm_parent   = pl[2];
            _asm_hidden   = pl[3] & 0x80;
            _asm_type     = (ParamType)(pl[3] & 0x7F);
            uint16_t n = pl_len - 4;
            if (n > sizeof(_asm_buf)) n = sizeof(_asm_buf);
            memcpy(_asm_buf, &pl[4], n);
            _asm_len = n;
        } else {
            uint16_t n = pl_len - 2;
            if (_asm_len + n > sizeof(_asm_buf)) n = sizeof(_asm_buf) - _asm_len;
            if (n > 0) { memcpy(_asm_buf + _asm_len, &pl[2], n); _asm_len += n; }
        }
        _asm_more_chunks = chunks_remaining > 0;
        _disc_got        = true;
        return;
    }

    // Otherwise, if it's an unsolicited update for a field already known -
    // most commonly a command's status moving from "running" to "confirmed"
    // - refresh that node in place so the command screen can show it.
    if (pl_len >= 4 && liveByField(field_id)) {
        _asm_field_id = field_id;
        _asm_parent   = pl[2];
        _asm_hidden   = pl[3] & 0x80;
        _asm_type     = (ParamType)(pl[3] & 0x7F);
        uint16_t n = pl_len - 4;
        if (n > sizeof(_asm_buf)) n = sizeof(_asm_buf);
        memcpy(_asm_buf, &pl[4], n);
        _asm_len = n;
        if (chunks_remaining == 0) finishAssembly();   // ignore async multi-chunk updates
    }
}

// Drop the leading byte and re-scan the remainder for a frame start, so one
// misplaced byte cannot desynchronise the stream permanently.
void CRSFEngine::resync() {
    if (_rx_len <= 1) { _rx_len = 0; return; }
    uint8_t n = _rx_len - 1;
    memmove(_rx_buf, _rx_buf + 1, n);
    _rx_len = 0;
    for (uint8_t i = 0; i < n; i++) {
        uint8_t b = _rx_buf[i];
        if (_rx_len == 0 &&
            b != CRSF_ADDR_TX_MODULE && b != CRSF_ADDR_HANDSET &&
            b != CRSF_ADDR_FC && b != 0xEF) { if (b) { _skip_last = b; if (_skip_cnt < 255) _skip_cnt++; } continue; }
        _rx_buf[_rx_len++] = b;
    }
}

void CRSFEngine::parseFrame(const uint8_t* f, uint8_t total) {
    if (total < 4) return;
    uint8_t type = f[2];
    const uint8_t* pl = &f[3];
    uint8_t pl_len = f[1] - 2;
    if (type != CRSF_FRAMETYPE_LINK_STATISTICS && type != CRSF_FRAMETYPE_RADIO_ID) _last_type = type;

    switch (type) {
        case CRSF_FRAMETYPE_DEVICE_INFO:
            // Payload is [destination][origin][name...]. The name begins at
            // offset 2 - reading from offset 1 picked up the origin address as
            // the first character of the module name. After the name's null
            // terminator comes: serial(4, always "ELRS") hw_id(4) fw_id(4)
            // field_count(1) protocol_ver(1) - field_count is how many
            // parameters to expect when walking the tree.
            if (_di_count < 99) _di_count++;
            _di_len = pl_len;
            if (pl_len >= 3) {
                _device_found = true;
                strncpy(_device_name, (const char*)&pl[2], sizeof(_device_name) - 1);
                _device_name[sizeof(_device_name) - 1] = 0;

                uint8_t ni = 2;
                while (ni < pl_len && pl[ni] != 0) ni++;
                uint8_t after = ni + 1;
                if (pl_len >= (uint16_t)after + 14) _field_count = pl[after + 12];
            }
            break;
        case CRSF_FRAMETYPE_RADIO_ID:
            // [dest][origin][subtype][interval 4B BE][offset 4B BE], both in
            // tenths of a microsecond. The module is telling us how often it
            // wants RC frames; matching that is what stops us transmitting
            // over the top of its replies.
            if (pl_len >= 11 && pl[0] == CRSF_ADDR_HANDSET &&
                pl[2] == CRSF_SUBTYPE_TIMING_CORRECTION) {
                uint32_t interval = ((uint32_t)pl[3] << 24) | ((uint32_t)pl[4] << 16) |
                                    ((uint32_t)pl[5] << 8)  |  (uint32_t)pl[6];
                uint32_t us = interval / 10;
                uint32_t ms = us / 1000;
                if (ms >= 2 && ms <= 25) { _rc_interval = ms; _synced = true; }
                _device_found = true;
            }
            break;

        case CRSF_FRAMETYPE_ELRS_STATUS:
            _device_found = true;
            break;

        case CRSF_FRAMETYPE_PARAM_SETTINGS_ENTRY:
            if (pl_len >= 4) handleSettingsEntry(pl + 2, pl_len - 2);   // skip [dest][origin] of the extended header
            break;
        case CRSF_FRAMETYPE_LINK_STATISTICS:
            if (pl_len >= 10) {
                _link.rssi1      = -(int8_t)pl[0];
                _link.rssi2      = -(int8_t)pl[1];
                _link.lq         = pl[2];
                _link.snr        = (int8_t)pl[3];
                _link.antenna    = pl[4];
                _link.rf_mode    = pl[5];
                _link.tx_pwr_idx = pl[6];
                _link.dl_rssi    = -(int8_t)pl[7];
                _link.dl_lq      = pl[8];
                _link.dl_snr     = (int8_t)pl[9];
                _link.valid      = true;
            }
            break;
        default: break;
    }
}

void CRSFEngine::beginSession() {
    if (_suspended || !_serial) return;
    _suspended      = true;
    _sess_invert    = _invert;
    _sess_rx_invert = _rx_invert;
    _invert = _rx_invert = false;      // a USB-serial adapter is plain TTL
    acquireTX();                       // drive the line for the whole session
    while (_serial->available()) (void)_serial->read();
}

void CRSFEngine::endSession() {
    if (!_suspended) return;
    _invert    = _sess_invert;
    _rx_invert = _sess_rx_invert;
    _rx_len    = 0;
    _pol_since = 0;
    if (_half_duplex) releaseTX(); else acquireTX();
    _suspended = false;
}

void CRSFEngine::update() {
    if (_suspended) return;
    uint32_t now = millis();

    // The line is left released whenever we are not transmitting. Driving it
    // idle would be wrong: a real radio holds the wire only through a pull-up
    // so the module can pull it low to reply at any moment, and a push-pull
    // high output here would simply swallow that reply.

    // Hunt for the working RECEIVE polarity only. Transmit is left alone, so a
    // module that is already accepting our frames stays connected throughout.
    // One frame passing CRC proves the setting and ends the search for good.
    // (Full duplex has no echo of our own frames, but any CRC-valid frame from
    // the module proves the receive polarity just as well - see below.)
    if (_auto_pol && !_echo_pass) {
        if (_pol_since == 0) _pol_since = now;
        if (now - _pol_since >= POL_TRY_MS) {
            _rx_invert = !_rx_invert;
            applyOverrides();
            _pol_since = now;
            _rx_len    = 0;
            _selftest  = false;
        }
    }

    while (_rx && _rx->available()) {
        uint8_t b = (uint8_t)_rx->read();
        _rx_bytes++;
        if (_tap) _tap(b);
        _rx_last_b = b;
        _last_rx = now;

        // Deliberately NOT accepting 0x00 (broadcast) as a frame start. A line
        // sitting at the wrong polarity reads as an unbroken run of 0x00, and
        // treating those as sync bytes turns pure noise into a stream of
        // "frames" that all fail CRC - which looks like a framing fault when
        // it is really a wiring or polarity fault. Replies from a TX module
        // are addressed to the handset or the module itself anyway.
        if (_rx_len == 0 &&
            b != CRSF_ADDR_TX_MODULE && b != CRSF_ADDR_HANDSET &&
            b != CRSF_ADDR_FC) continue;
        if (_rx_len < sizeof(_rx_buf)) _rx_buf[_rx_len++] = b;
        if (_rx_len < 2) continue;

        uint16_t expected = (uint16_t)_rx_buf[1] + 2u;
        if (expected < 4 || expected > sizeof(_rx_buf)) { resync(); continue; }
        if (_rx_len < expected) continue;

        if (crsf_crc8(&_rx_buf[2], _rx_buf[1] - 1) == _rx_buf[expected - 1]) {
            if (_selftest) {
                // This is our own frame coming back. Receive path proven; do
                // not count it as something the module said.
                _echo_pass = true;
                _selftest  = false;
            } else {
                _rx_frames++;
                _echo_pass = true;     // a good frame from the module: receive polarity is right, stop hunting
                parseFrame(_rx_buf, (uint8_t)expected);
            }
            _rx_len = 0;
        } else {
            // Hold on to what failed so it can be inspected. Without this the
            // interesting frame is immediately overwritten by line noise.
            _bad_snap[0] = _rx_buf[0];                    // address
            _bad_snap[1] = _rx_buf[1];                    // length
            _bad_snap[2] = _rx_buf[2];                    // frame type
            _bad_calc    = crsf_crc8(&_rx_buf[2], _rx_buf[1] - 1);
            _bad_recv    = _rx_buf[expected - 1];
            _have_bad    = true;

            // A stray byte in front of a good frame would otherwise cost us
            // every frame that follows. Drop one byte and rescan what is left
            // instead of discarding the whole buffer.
            _rx_bad++;
            resync();
        }
    }
    if (_rx_len && now - _last_rx > 20) _rx_len = 0;

    // Hold off transmitting while a reply is expected. Skipping a handful of
    // RC frames is harmless - the module holds the last values - and without
    // the gap the answer never gets onto the wire at all.
    bool quiet = (int32_t)(_quiet_until - now) > 0;

    if (!quiet && _rc_enabled && now - _last_rc >= _rc_interval) {
        sendRCChannels();
        _last_rc = now;
    }
    if (!quiet && now - _last_sync >= SYNC_INTERVAL_MS) {
        sendOpenTXSync();
        _last_sync = now;
    }

    uint32_t ping_period = _device_found ? 5000 : PING_INTERVAL_MS;
    if (!quiet && now - _last_ping >= ping_period) {
        pingDevice();
        _last_ping = now;
    }

    // Step the parameter-tree discovery, one field (and, for a field big
    // enough to span more than one frame, one chunk) at a time. Not gated on
    // `quiet` for the whole block since it paces itself via its own timeout,
    // but the actual send still respects it so a read never collides with an
    // RC frame going out the same millisecond. Sending and waiting for a
    // reply are tracked separately so a busy tick can only delay a step,
    // never skip it outright.
    if (_disc_active) {
        if (_field_count == 0) {
            if (now - _disc_wait_since > DISC_GIVE_UP_MS) _disc_active = false;   // no module / no reply at all
        } else if (_disc_field > _field_count) {
            _disc_active = false;   // walked every field the module told us about
        } else if (!_disc_sent) {
            if (!quiet) {
                sendParamReadChunk(_disc_field, _disc_chunk);
                _disc_sent    = true;
                _disc_sent_at = now;
            }
        } else if (_disc_got || now - _disc_sent_at >= DISC_TIMEOUT_MS) {
            if (_disc_got && _asm_more_chunks) {
                // This field isn't finished yet - go fetch its next chunk.
                _disc_chunk++;
                _disc_got  = false;
                _disc_sent = false;
            } else {
                if (_disc_got) finishAssembly();   // got everything - store it
                // else: no reply at all for this field/chunk - move on rather
                // than stall the whole walk over one unresponsive index.
                _disc_field++;
                _disc_chunk = 0;
                _asm_len    = 0;
                _disc_got   = false;
                _disc_sent  = false;
            }
        }
    }
}
