#include "hid_parser.h"

// ─── BIT EXTRACTION ──────────────────────────────────────────────────────────
static uint32_t extractBits(const uint8_t* data, uint16_t byte_len,
                             uint16_t bit_offset, uint8_t bit_count) {
    uint32_t v = 0;
    if ((bit_offset >> 3) >= byte_len) return 0;   // entirely past the report
    if (bit_count == 0 || bit_count > 32) return 0;
    for (uint8_t i = 0; i < bit_count && i < 32; i++) {
        uint16_t b = bit_offset + i;
        if ((b >> 3) >= byte_len) break;
        if (data[b >> 3] & (1u << (b & 7))) v |= (1UL << i);
    }
    return v;
}

// Sign-extend an n-bit value if the field is signed
static int32_t signExtend(uint32_t v, uint8_t bits, bool is_signed) {
    if (!is_signed || bits >= 32) return (int32_t)v;
    uint32_t sign_bit = 1UL << (bits - 1);
    if (v & sign_bit) return (int32_t)(v | (~0UL << bits));
    return (int32_t)v;
}

void HIDReportMap::reset() {
    _mode = HIDMode::DESCRIPTOR;
    _num_axes = 0;
    _num_buttons = 0;
    _uses_report_id = false;
    _rid_fallback   = false;
    memset(_axes, 0, sizeof(_axes));
    memset(_buttons, 0, sizeof(_buttons));
}

// ─── DESCRIPTOR WALK ─────────────────────────────────────────────────────────
void HIDReportMap::parse(const uint8_t* desc, uint16_t len) {
    reset();
    // desc_report is NULL when the descriptor was larger than TinyUSB's
    // enumeration buffer. Raw mode still gives full use of the device.
    if (!desc || len == 0) { _mode = HIDMode::RAW; return; }

    // Global state
    uint16_t usage_page   = 0;
    int32_t  logical_min  = 0;
    int32_t  logical_max  = 0;
    uint8_t  report_size  = 0;
    uint16_t report_count = 0;
    uint8_t  report_id    = 0;

    // Local state (cleared after each Main item)
    uint16_t usages[64]   = {};
    uint8_t  usage_count  = 0;
    uint16_t usage_min    = 0;
    uint16_t usage_max    = 0;
    bool     has_usage_range = false;

    // Per-report-ID bit cursor
    uint16_t bit_pos = 0;
    uint8_t  cur_rid = 0;

    // Hard iteration cap. A malformed descriptor could otherwise leave the
    // cursor stationary and spin here forever, which on Core0 means the
    // watchdog fires and the board reboots the moment that device is plugged
    // in - an unbreakable reset loop.
    uint16_t guard = 0;
    uint16_t i = 0;
    while (i < len) {
        if (++guard > 2048) break;
        uint16_t i_before = i;

        uint8_t item = desc[i++];
        if (item == 0xFE) { // long item - skip
            if (i + 1 >= len) break;
            uint8_t dsize = desc[i];
            i += 2 + dsize;
            if (i <= i_before) break;      // no progress: bail out
            continue;
        }

        uint8_t  tag  = (item >> 4) & 0x0F;
        uint8_t  type = (item >> 2) & 0x03;
        uint8_t  size = item & 0x03;
        if (size == 3) size = 4;

        if (i + size > len) break;

        uint32_t data = 0;
        for (uint8_t k = 0; k < size; k++) data |= ((uint32_t)desc[i + k]) << (8 * k);
        i += size;
        if (i <= i_before) break;          // no progress: bail out

        // Signed interpretation for logical min/max
        int32_t sdata = (int32_t)data;
        if (size == 1 && (data & 0x80))       sdata = (int32_t)(data | 0xFFFFFF00);
        else if (size == 2 && (data & 0x8000)) sdata = (int32_t)(data | 0xFFFF0000);

        switch (type) {
        // ── GLOBAL ──────────────────────────────────────────────────────────
        case 1:
            switch (tag) {
                case 0:  usage_page  = (uint16_t)data; break;  // Usage Page
                case 1:  logical_min = sdata;          break;  // Logical Minimum
                case 2:  logical_max = sdata;          break;  // Logical Maximum
                case 7:  report_size = (uint8_t)data;  break;  // Report Size
                case 8:                                        // Report ID
                    report_id = (uint8_t)data;
                    _uses_report_id = true;
                    if (report_id != cur_rid) { cur_rid = report_id; bit_pos = 0; }
                    break;
                case 9:  report_count = (uint16_t)data; break; // Report Count
                default: break;
            }
            break;

        // ── LOCAL ───────────────────────────────────────────────────────────
        case 2:
            switch (tag) {
                case 0:  // Usage
                    if (usage_count < 64) usages[usage_count++] = (uint16_t)data;
                    break;
                case 1:  usage_min = (uint16_t)data; has_usage_range = true; break;
                case 2:  usage_max = (uint16_t)data; has_usage_range = true; break;
                default: break;
            }
            break;

        // ── MAIN ────────────────────────────────────────────────────────────
        case 0:
            if (tag == 8) {   // Input item
                bool is_constant = (data & 0x01) != 0;
                bool is_variable = (data & 0x02) != 0;

                // report_count is device-supplied; a silly value (some sticks
                // declare thousands of padding bits) would spin for a long
                // time here, so clamp it to what we could ever store.
                uint16_t rc = report_count;
                if (rc > 256) rc = 256;

                if (!is_constant && is_variable) {
                    for (uint16_t n = 0; n < rc; n++) {
                        uint16_t usage;
                        if (has_usage_range)
                            usage = usage_min + n;
                        else if (n < usage_count)
                            usage = usages[n];
                        else
                            usage = usage_count ? usages[usage_count - 1] : 0;

                        uint16_t f_bit = bit_pos + n * report_size;

                        if (usage_page == HID_UP_BUTTON) {
                            if (_num_buttons < MAX_INPUT_BUTTONS) {
                                HIDField& f = _buttons[_num_buttons++];
                                f.bit_offset  = f_bit;
                                f.bit_size    = report_size;
                                f.logical_min = logical_min;
                                f.logical_max = logical_max;
                                f.report_id   = report_id;
                            }
                        }
                        else if (usage_page == HID_UP_GENERIC_DESKTOP &&
                                 usage >= HID_USAGE_X && usage <= HID_USAGE_HATSWITCH) {
                            if (_num_axes < MAX_INPUT_AXES) {
                                HIDField& f = _axes[_num_axes++];
                                f.bit_offset  = f_bit;
                                f.bit_size    = report_size;
                                f.logical_min = logical_min;
                                f.logical_max = logical_max;
                                f.report_id   = report_id;
                            }
                        }
                        else if (usage_page == HID_UP_SIMULATION) {
                            // Throttle/rudder/brake etc. – treat as axes
                            if (_num_axes < MAX_INPUT_AXES) {
                                HIDField& f = _axes[_num_axes++];
                                f.bit_offset  = f_bit;
                                f.bit_size    = report_size;
                                f.logical_min = logical_min;
                                f.logical_max = logical_max;
                                f.report_id   = report_id;
                            }
                        }
                    }
                }
                // Advance by the declared size, saturating rather than wrapping.
                uint32_t adv = (uint32_t)report_size * report_count;
                bit_pos = (uint16_t)((bit_pos + adv > 0xFFFFu) ? 0xFFFFu : bit_pos + adv);
            }
            else if (tag == 9 || tag == 11) {   // Output / Feature – skip bits
                // do not advance input bit_pos
            }
            // Collection (10) / End Collection (12) – nothing to do

            // Main items clear local state
            usage_count = 0;
            has_usage_range = false;
            usage_min = usage_max = 0;
            break;

        default: break;
        }
    }
}

// ─── RUNTIME DECODE ──────────────────────────────────────────────────────────
// Extract every field the descriptor described. `match_rid` controls whether
// fields are filtered by Report ID; see decode() below for why that matters.
bool HIDReportMap::decodeAt(const uint8_t* data, uint16_t data_len, uint8_t rid,
                             bool match_rid,
                             int16_t axes_out[MAX_INPUT_AXES], uint8_t* num_axes_out,
                             uint32_t* buttons_out, uint8_t* num_buttons_out) const {
    uint8_t na = 0;
    for (uint8_t a = 0; a < _num_axes; a++) {
        const HIDField& f = _axes[a];
        if (match_rid && f.report_id != rid) continue;

        uint32_t raw       = extractBits(data, data_len, f.bit_offset, f.bit_size);
        bool     is_signed = (f.logical_min < 0);
        int32_t  v         = signExtend(raw, f.bit_size, is_signed);

        int32_t span = f.logical_max - f.logical_min;
        int16_t norm;
        if (span <= 0) {
            norm = 0;
        } else {
            int64_t scaled = ((int64_t)(v - f.logical_min) * 65535) / span - 32768;
            if (scaled >  32767) scaled =  32767;
            if (scaled < -32768) scaled = -32768;
            norm = (int16_t)scaled;
        }
        if (na < MAX_INPUT_AXES) axes_out[na++] = norm;
    }

    uint32_t btns = 0;
    uint8_t  nb   = 0;
    for (uint8_t b = 0; b < _num_buttons && nb < 32; b++) {
        const HIDField& f = _buttons[b];
        if (match_rid && f.report_id != rid) continue;
        if (extractBits(data, data_len, f.bit_offset, f.bit_size)) btns |= (1UL << nb);
        nb++;
    }

    if (na == 0 && nb == 0) return false;

    *num_axes_out    = na;
    *buttons_out     = btns;
    *num_buttons_out = nb;
    return true;
}

bool HIDReportMap::decode(const uint8_t* report, uint16_t len,
                           int16_t axes_out[MAX_INPUT_AXES], uint8_t* num_axes_out,
                           uint32_t* buttons_out, uint8_t* num_buttons_out) const {
    if (!report || len == 0) return false;

    // ── Raw mode ────────────────────────────────────────────────────────────
    // Byte N -> axis N (treated as unsigned 0-255, centred at 128)
    // Bit  N -> button N
    // Analog axes and button bytes both appear; the learning screen picks
    // whichever the user actually moves, so no interpretation is needed.
    if (_mode == HIDMode::RAW) {
        uint8_t na = (len > MAX_INPUT_AXES) ? MAX_INPUT_AXES : (uint8_t)len;
        for (uint8_t i = 0; i < na; i++)
            axes_out[i] = (int16_t)(((int32_t)report[i] - 128) * 256);

        uint32_t btns = 0;
        uint8_t  nb   = (len > 4) ? 32 : (uint8_t)(len * 8);
        for (uint8_t b = 0; b < nb && b < 32; b++)
            if (report[b >> 3] & (1u << (b & 7))) btns |= (1UL << b);

        *num_axes_out    = na;
        *buttons_out     = btns;
        *num_buttons_out = nb;
        return true;
    }

    if (_uses_report_id && !_rid_fallback) {
        // Normal path: first byte is the Report ID, fields are filtered by it.
        if (decodeAt(report + 1, len - 1, report[0], true,
                     axes_out, num_axes_out, buttons_out, num_buttons_out))
            return true;

        // Nothing matched. Several joysticks (e.g. Saitek ST90) declare a
        // Report ID in the descriptor but then omit the ID byte from the
        // reports they actually send, so byte 0 is real data. Latch into
        // no-ID mode and parse from offset 0 from now on.
        _rid_fallback = true;
    }

    // No Report ID declared, or we fell back to ignoring it.
    return decodeAt(report, len, 0, false,
                    axes_out, num_axes_out, buttons_out, num_buttons_out);
}
