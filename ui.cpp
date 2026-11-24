#include "ui.h"
#include "usb_diag.h"
#include "ppm_out.h"
#include "ps_host.h"
#include "splash.h"

extern PPMOut g_ppm_out;

// Layout constants for a 128x64 SSD1306, font size 1 (6x8 px)
static const uint8_t HDR_H     = 10;
static const uint8_t ROW_Y0    = 12;
static const uint8_t ROW_H     = 11;
static const uint8_t VIS_ROWS  = 4;
static const uint8_t FOOTER_Y  = 56;

// How long the boot splash stays up. Long enough to cover the DFPlayer's
// ~2.6 s boot+reset sequence (sound.h) plus a short margin, so the welcome
// sound is already audible before the splash clears rather than starting
// just after it.
static const uint32_t SPLASH_HOLD_MS = 2800;

// Adafruit_SSD1306::begin() does not reliably report a missing display: it
// mainly reflects whether the frame-buffer malloc succeeded, and largely
// ignores whether the device actually ACKed on the bus. Probing the address
// ourselves first is the only way to know whether anything is really there
// before handing it to the display library.
static bool i2cPing(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// ─── INIT ────────────────────────────────────────────────────────────────────
bool UIEngine::begin(ConfigManager* cfg, CRSFEngine* crsf, InputManager* input,
                      ProfileStore* profiles, ChannelMixer* mixer, DFPlayerVoice* voice) {
    _cfg = cfg; _crsf = crsf; _input = input; _prof = profiles; _mixer = mixer; _voice = voice;

    // Cheap SSD1306 boards are commonly strapped to 0x3C or 0x3D depending on
    // the module, so a fixed address is a frequent no-screen support ticket.
    // Ping the configured address a few times, then the other common one a
    // few times, before giving up on the bus entirely.
    const uint8_t addrs[2] = { OLED_ADDR, (OLED_ADDR == 0x3C) ? 0x3D : 0x3C };
    bool found = false;
    uint8_t found_addr = 0;
    for (int a = 0; a < 2 && !found; a++) {
        for (int i = 0; i < 5 && !found; i++) {
            found = i2cPing(addrs[a]);
            if (!found) delay(40);
        }
        if (found) found_addr = addrs[a];
    }
    _oled_ok   = found;
    _oled_addr = found_addr;

    // Buttons need to work whether or not the screen came up - Special
    // Functions, safe mode, etc. all still run headless - so this is
    // unconditional rather than gated on the display.
    _joy.begin();
    if (!found) {
        // Nothing answered on the bus at all, at either address, after
        // several tries: not an addressing mismatch, this is the display
        // (or the I2C wiring/power to it) not present on the line as far
        // as the RP2040 can tell. Calling disp.begin() anyway would just
        // paper over that with a return value that means little.
        return false;
    }

    // A device is really there, so it's worth letting the library try to
    // bring it up (its own retry is cheap insurance against a slow-waking
    // panel), but the failure signal from here on is our own ACK above,
    // not disp.begin()'s return.
    for (int i = 0; i < 3; i++) {
        if (disp.begin(SSD1306_SWITCHCAPVCC, found_addr)) break;
        delay(60);
    }

    disp.setAddr(found_addr);
    disp.setTextSize(1);
    disp.setTextColor(SSD1306_WHITE);
    disp.clearDisplay();
    // Full-screen splash. The wordmark and icon are baked into the
    // artwork, so nothing needs to be drawn over it - the active mode
    // appears on the status screen a moment later anyway.
    disp.drawBitmap(0, 0, SPLASH_BMP, SPLASH_W, SPLASH_H, SSD1306_WHITE);
    disp.display();

    // The welcome sound was queued into g_voice just before this call, but
    // the DFPlayer needs ~2.6 s after its own begin() to mount the SD card
    // and accept commands (see sound.h's BOOT_DELAY_MS/RESET_DELAY_MS) - and
    // that state machine only advances on update() calls. Pump it here
    // instead of a blind delay(), so the startup track actually starts
    // playing while the splash is still on screen, rather than firing a
    // moment after the menu has already replaced it.
    uint32_t splash_start = millis();
    while (millis() - splash_start < SPLASH_HOLD_MS) {
        if (_voice) _voice->update();
        delay(10);
    }
    disp.clearDisplay(); disp.display();
    return true;
}

// ─── HELPERS ─────────────────────────────────────────────────────────────────
void UIEngine::header(const char* t) {
    disp.fillRect(0, 0, 128, HDR_H, SSD1306_WHITE);
    disp.setTextColor(SSD1306_BLACK);
    disp.setCursor(2, 1);
    disp.print(t);
    disp.setTextColor(SSD1306_WHITE);
}

void UIEngine::footer(const char* ok_fn, const char* back_fn, const char* hold_fn) {
    // y=56 with an 8 px font ends at y=63, exactly filling the last row.
    int16_t lw = (ok_fn && *ok_fn) ? (int16_t)strlen(ok_fn) * 6 : 0;
    int16_t rw = (back_fn && *back_fn) ? (int16_t)strlen(back_fn) * 6 : 0;

    if (lw) { disp.setCursor(0, FOOTER_Y);        disp.print(ok_fn); }
    if (rw) { disp.setCursor(128 - rw, FOOTER_Y); disp.print(back_fn); }

    // The middle zone is a hint for a long-press or a secondary action.
    // Centred in whatever gap the other two leave, and dropped entirely if
    // it will not fit rather than being allowed to overlap them.
    if (hold_fn && *hold_fn) {
        int16_t aw   = (int16_t)strlen(hold_fn) * 6;
        int16_t free = 128 - lw - rw;
        if (aw + 12 <= free) {
            // Prefer the middle of the screen; fall back to the middle of the
            // remaining gap only if that would run into one of the other two.
            int16_t x = (128 - aw) / 2;
            if (x < lw + 6)        x = lw + (free - aw) / 2;
            if (x + aw > 128 - rw) x = lw + (free - aw) / 2;
            disp.setCursor(x, FOOTER_Y);
            disp.print(hold_fn);
        }
    }
}

// A 60 px live bar sitting on the right of the header strip. The header is a
// filled white band, so the bar has to be drawn in black to be visible. Putting
// it here keeps it clear of the rows and the footer, which is where it used to
// collide.
void UIEngine::headerBar(float v) {
    const int16_t BX = 64, BY = 2, BW = 60, BH = 6;
    v = constrain(v, 0.0f, 1.0f);
    disp.drawRect(BX, BY, BW, BH, SSD1306_BLACK);
    int16_t fw = (int16_t)(v * (BW - 2));
    if (fw > 0) disp.fillRect(BX + 1, BY + 1, fw, BH - 2, SSD1306_BLACK);
}

void UIEngine::row(uint8_t r, bool sel, const char* text) {
    int16_t y = ROW_Y0 + r * ROW_H;
    if (sel) {
        disp.fillRect(0, y - 2, 122, ROW_H, SSD1306_WHITE);
        disp.setTextColor(SSD1306_BLACK);
    }
    disp.setCursor(3, y);
    disp.print(text);
    if (sel) disp.setTextColor(SSD1306_WHITE);
}

void UIEngine::bar(int16_t x, int16_t y, int16_t w, int16_t h, float v, uint16_t col) {
    v = constrain(v, 0.0f, 1.0f);
    disp.drawRect(x, y, w, h, col);
    int16_t fill = (int16_t)(v * (w - 2));
    if (fill > 0) disp.fillRect(x + 1, y + 1, fill, h - 2, col);
}

void UIEngine::yesNo(uint8_t sel) {
    const int16_t y = 40;
    if (sel == 0) {
        disp.fillRect(12, y - 2, 34, 13, SSD1306_WHITE);
        disp.setTextColor(SSD1306_BLACK);
        disp.setCursor(16, y); disp.print(F("YES"));
        disp.setTextColor(SSD1306_WHITE);
        disp.setCursor(74, y); disp.print(F("NO"));
    } else {
        disp.setCursor(16, y); disp.print(F("YES"));
        disp.fillRect(70, y - 2, 28, 13, SSD1306_WHITE);
        disp.setTextColor(SSD1306_BLACK);
        disp.setCursor(74, y); disp.print(F("NO"));
        disp.setTextColor(SSD1306_WHITE);
    }
}

void UIEngine::scrollbar(uint8_t total, uint8_t vis, uint8_t scroll) {
    if (total <= vis) return;
    const int16_t top = HDR_H + 1, h = FOOTER_Y - top - 2;
    int16_t bh = (int16_t)((float)h * vis / total);
    int16_t by = top + (int16_t)((float)h * scroll / total);
    disp.drawRect(124, top, 4, h, SSD1306_WHITE);
    disp.fillRect(124, by, 4, bh, SSD1306_WHITE);
}

// Live children of a folder (or the root, for folder==0), in the order they
// were discovered, skipping hidden fields (the module hides fields that
// don't apply in the current mode - e.g. MAVLink hides several RF fields).
uint8_t UIEngine::elrsChildren(uint8_t folder, uint8_t* out, uint8_t max) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < _crsf->liveCount() && n < max; i++) {
        const LiveParam* p = _crsf->liveAt(i);
        if (p && p->parent == folder && !p->hidden && p->field_id != 0) out[n++] = p->field_id;
    }
    return n;
}

void UIEngine::checkSoundTriggers(const InputState& in) {
    for (uint8_t i = 0; i < MAX_SOUND_TRIGGERS; i++) {
        const SoundTrigger& t = _cfg->cfg.sound.triggers[i];
        bool active = false;
        if (t.type == SourceType::BUTTON && t.index < in.num_buttons) {
            active = (in.buttons >> t.index) & 1u;
        } else if (t.type == SourceType::AXIS && t.index < in.num_axes) {
            int16_t v = in.axes[t.index];
            active = t.rising ? (v >= t.threshold) : (v <= t.threshold);
        }
        if (active && !_trig_was[i] && t.track) _voice->play(t.track);
        _trig_was[i] = active;
    }
}

void UIEngine::checkTrimEvents() {
    if (!_trim_init) {
        for (uint8_t i = 0; i < CRSF_NUM_CHANNELS; i++) _last_trim[i] = _cfg->cfg.channels[i].trim;
        _trim_init = true;
        return;
    }
    for (uint8_t i = 0; i < CRSF_NUM_CHANNELS; i++) {
        int8_t t = _cfg->cfg.channels[i].trim;
        if (t == _last_trim[i]) continue;
        // Whichever channel it is, and however it got there - the trim
        // buttons or hand-editing Trim in the menu - the cue is the same
        // one a physical radio gives: which channel isn't announced, only
        // that a limit or centre was just reached.
        if (!((_cfg->cfg.trim_sound_off >> i) & 1u)) {   // Trim > Sound ON for this channel
            if      (t == 100)  playEvent(SE_TRIM_MAX);
            else if (t == -100) playEvent(SE_TRIM_MIN);
            else if (t == 0)    playEvent(SE_TRIM_CENTER);
        }
        _last_trim[i] = t;
    }
}

void UIEngine::checkInactivity(const InputState& in, uint32_t now) {
    if (!_activity_init) {
        _last_buttons_seen = in.buttons;
        for (uint8_t i = 0; i < MAX_INPUT_AXES; i++) _last_axes_seen[i] = in.axes[i];
        _last_activity_ms = _last_inactivity_alarm_ms = now;
        _activity_init = true;
        return;
    }

    bool moved = (in.buttons != _last_buttons_seen);
    _last_buttons_seen = in.buttons;
    for (uint8_t i = 0; i < in.num_axes && i < MAX_INPUT_AXES; i++) {
        // A little slack so calibration noise on an idle stick doesn't
        // count as activity and keep the alarm from ever firing.
        if (abs((int32_t)in.axes[i] - _last_axes_seen[i]) > 1500) moved = true;
        _last_axes_seen[i] = in.axes[i];
    }

    if (moved) {
        _last_activity_ms = _last_inactivity_alarm_ms = now;
        return;
    }

    if (!_cfg->cfg.inactivity.enabled) return;
    uint32_t threshold_ms = (uint32_t)_cfg->cfg.inactivity.minutes * 60000UL;
    if (threshold_ms == 0) return;
    if (now - _last_activity_ms >= threshold_ms &&
        now - _last_inactivity_alarm_ms >= threshold_ms) {
        playEvent(SE_INACTIVITY);
        _last_inactivity_alarm_ms = now;
    }
}

bool UIEngine::sfTriggerActive(const SpecialFunction& f, const InputState& in, const uint16_t* chv) const {
    // Channel trigger: the channel's output value as a percent, compared with
    // the level. chv is the mixer output when the caller has it (override
    // evaluation, before overrides apply), otherwise last frame's output.
    if (f.trig_type == SourceType::CHANNEL && f.trig_index < CRSF_NUM_CHANNELS) {
        const uint16_t* src = chv ? chv : _live;
        if (!src) return false;
        int32_t pct = ((int32_t)src[f.trig_index] - CRSF_CHANNEL_MID) * 200 /
                      (CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN);
        return f.trig_rising ? (pct >= f.trig_threshold) : (pct <= f.trig_threshold);
    }
    if (f.trig_type == SourceType::BUTTON && f.trig_index < in.num_buttons)
        return (in.buttons >> f.trig_index) & 1u;
    if (f.trig_type == SourceType::AXIS && f.trig_index < in.num_axes) {
        int16_t v = in.axes[f.trig_index];
        return f.trig_rising ? (v >= f.trig_threshold) : (v <= f.trig_threshold);
    }
    return false;
}

bool UIEngine::sfConditionMet(const SpecialFunction& f, const InputState& in, const uint16_t* chv) const {
    return sfTriggerActive(f, in, chv) == f.fire_active;
}

// Every function row except Override Channel fires once, on the edge into
// its configured state (Active or Inactive) - a press-and-hold doesn't
// repeat it. Override Channel is level-based and handled separately, in
// applyOverrides(), since it has to run inline with mixing.
void UIEngine::tickSpecialFunctions(const InputState& in) {
    for (uint8_t i = 0; i < MAX_SPECIAL_FUNCS; i++) {
        const SpecialFunction& f = _cfg->cfg.sf[i];

        // Volume is continuous, like Override Channel: it tracks the trigger
        // axis's position every frame rather than firing once on a
        // transition, so it has no "Active/Inactive" edge to detect at all.
        if (f.type == SFType::VOLUME) {
            _sf_was[i] = false;
            if (_voice && f.trig_type == SourceType::CHANNEL && _live && f.trig_index < CRSF_NUM_CHANNELS) {
                int32_t span = (int32_t)f.vol_max - (int32_t)f.vol_min;
                int32_t vol  = f.vol_min + ((int32_t)(_live[f.trig_index] - CRSF_CHANNEL_MIN) * span) /
                                           (CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN);
                _voice->setVolume((uint8_t)constrain(vol, 0L, 30L));
            } else if (_voice && f.trig_type == SourceType::AXIS && f.trig_index < in.num_axes) {
                int32_t span = (int32_t)f.vol_max - (int32_t)f.vol_min;
                int32_t vol  = f.vol_min + ((int32_t)(in.axes[f.trig_index] + 32768) * span) / 65535;
                _voice->setVolume((uint8_t)constrain(vol, 0L, 30L));
            }
            continue;
        }

        // Run Timer is level-based: the timer counts for as long as the
        // condition holds (e.g. a channel is high) and stops when it doesn't.
        // A countdown that has reached zero stays finished - it isn't
        // restarted (and "timer over" re-announced) every frame.
        if (f.type == SFType::TIMER_RUN) {
            _sf_was[i] = false;
            if (f.timer_idx < MAX_TIMERS) {
                TimerRT& tr = _timer_rt[f.timer_idx];
                bool done = _cfg->cfg.timers[f.timer_idx].kind == TimerKind::COUNTDOWN && tr.value_s <= 0;
                tr.running = sfConditionMet(f, in) && !done;
            }
            continue;
        }

        if (f.type == SFType::NONE || f.type == SFType::OVERRIDE_CHANNEL) { _sf_was[i] = false; continue; }

        bool met  = sfConditionMet(f, in);
        bool edge = met && !_sf_was[i];
        _sf_was[i] = met;
        if (!edge) continue;

        switch (f.type) {
            case SFType::PLAY_SOUND:
                if (_voice && f.sound_track) _voice->play(f.sound_track);
                break;
            case SFType::TIMER_SET:
                if (f.timer_idx < MAX_TIMERS) {
                    TimerRT& tr = _timer_rt[f.timer_idx];
                    tr.value_s  = f.timer_set_s;
                    tr.accum_ms = 0;
                    tr.last_min = 0;
                }
                break;
            case SFType::TIMER_START:
                if (f.timer_idx < MAX_TIMERS) _timer_rt[f.timer_idx].running = true;
                break;
            case SFType::TIMER_STOP:
                if (f.timer_idx < MAX_TIMERS) _timer_rt[f.timer_idx].running = false;
                break;
            case SFType::TIMER_RESET:
                if (f.timer_idx < MAX_TIMERS) {
                    const TimerDef& td = _cfg->cfg.timers[f.timer_idx];
                    TimerRT&        tr = _timer_rt[f.timer_idx];
                    tr.value_s  = (td.kind == TimerKind::COUNTDOWN) ? td.start_s : 0;
                    tr.accum_ms = 0;
                    tr.last_min = 0;
                }
                break;
            default: break;
        }
    }
}

// Which channels (bit i = channel i) currently have an active Override
// Channel row forcing them, right now - what the Status screen's "o"
// marker under a bar reflects. Mirrors applyOverrides()'s own condition
// check so the marker can never disagree with what's actually happening
// to the output.
uint16_t UIEngine::overriddenChannelMask() const {
    if (_mixer && _mixer->inFailsafe()) return 0;   // overrides don't apply in failsafe either
    uint16_t mask = 0;
    for (uint8_t i = 0; i < MAX_SPECIAL_FUNCS; i++) {
        const SpecialFunction& f = _cfg->cfg.sf[i];
        if (f.type != SFType::OVERRIDE_CHANNEL) continue;
        if (f.override_ch >= CRSF_NUM_CHANNELS) continue;
        if (sfConditionMet(f, _in)) mask |= (uint16_t)(1u << f.override_ch);
    }
    return mask;
}

void UIEngine::applyOverrides(uint16_t ch[CRSF_NUM_CHANNELS], const InputState& in) {
    // A configured failsafe value exists specifically so a channel lands
    // somewhere deliberate when the link is gone - an override active at
    // that same moment must never be allowed to replace it, or the one
    // safety mechanism a channel actually has becomes silently optional.
    if (_mixer && _mixer->inFailsafe()) return;

    for (uint8_t i = 0; i < MAX_SPECIAL_FUNCS; i++) {
        const SpecialFunction& f = _cfg->cfg.sf[i];
        if (f.type != SFType::OVERRIDE_CHANNEL) continue;
        if (f.override_ch >= CRSF_NUM_CHANNELS) continue;
        if (!sfConditionMet(f, in, ch)) continue;
        int32_t v = CRSF_CHANNEL_MID +
                    ((int32_t)f.override_pct * (CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN)) / 200;
        ch[f.override_ch] = (uint16_t)constrain(v, (long)CRSF_CHANNEL_MIN, (long)CRSF_CHANNEL_MAX);
    }
}

// Timers tick every frame regardless of what screen is open - a countdown
// left running behind another menu still finishes on time.
void UIEngine::tickTimers(uint32_t now) {
    uint32_t dt = now - _timer_last_ms;
    _timer_last_ms = now;
    if (dt > 500) dt = 500;   // first call, or a long stall - don't fast-forward

    for (uint8_t i = 0; i < MAX_TIMERS; i++) {
        const TimerDef& td = _cfg->cfg.timers[i];
        if (!td.enabled) continue;
        TimerRT& tr = _timer_rt[i];
        if (!tr.running) continue;
        bool minute_call = _cfg->cfg.timer_minute_call[i];

        tr.accum_ms += dt;
        while (tr.accum_ms >= 1000) {
            tr.accum_ms -= 1000;
            if (td.kind == TimerKind::COUNTDOWN) {
                if (tr.value_s > 0) tr.value_s--;
                if (tr.value_s <= 0) {
                    tr.value_s = 0;
                    tr.running = false;
                    if (_voice) {
                        uint16_t t = _cfg->cfg.sound.event_track[SE_TIMER_OVER];
                        if (t) _voice->play(t);
                    }
                } else if (minute_call && tr.value_s % 60 == 0) {
                    // Minutes remaining, same number+word tracks as below.
                    uint16_t mins = (uint16_t)(tr.value_s / 60);
                    if (mins != tr.last_min) {
                        tr.last_min = mins;
                        if (_voice && mins <= 99) {
                            _voice->play(SPOKEN_NUMBER_BASE + mins);   // spoken number
                            uint16_t t = _cfg->cfg.sound.event_track[mins == 1 ? SE_MIN_CALL : SE_MINS_CALL];
                            if (t) _voice->play(t);   // "minute" / "minutes"
                        }
                    }
                }
            } else {
                tr.value_s++;
                uint16_t mins = (uint16_t)(tr.value_s / 60);
                if (minute_call && tr.value_s % 60 == 0 && mins > 0 && mins != tr.last_min) {
                    tr.last_min = mins;
                    if (_voice && mins <= 99) {
                        _voice->play(SPOKEN_NUMBER_BASE + mins);   // spoken number
                        uint16_t t = _cfg->cfg.sound.event_track[mins == 1 ? SE_MIN_CALL : SE_MINS_CALL];
                        if (t) _voice->play(t);   // "minute" / "minutes"
                    }
                }
            }
        }
    }
}

uint8_t UIEngine::timerSlots(uint8_t* out, uint8_t max) const {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_TIMERS && n < max; i++)
        if (_cfg->cfg.timers[i].enabled) out[n++] = i;
    return n;
}

uint8_t UIEngine::diagPageCount() const {
    return 2 + (_in.num_axes + 3) / 4;   // page 0: USB info, page 1: raw report, then axes
}

const char* UIEngine::optionAt(const char* list, uint8_t idx, char* buf, uint8_t sz) {
    buf[0] = 0;
    if (!list) return buf;
    const char* p = list;
    for (uint8_t k = 0; k < idx && *p; k++) {
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
    }
    uint8_t n = 0;
    while (*p && *p != ';' && n < sz - 1) buf[n++] = *p++;
    buf[n] = 0;
    return buf;
}

// How many ';'-separated entries are in a TEXT_SELECTION options string.
static uint8_t countOptions(const char* list) {
    if (!list || !*list) return 0;
    uint8_t n = 1;
    for (const char* p = list; *p; p++) if (*p == ';') n++;
    return n;
}

// CRSF FLOAT fields carry the value scaled by 10^decimals as a plain
// integer (e.g. 425 with decimals=2 means 4.25) - turn that back into text.
static void formatFloat(int32_t raw, uint8_t decimals, char* buf, uint8_t sz) {
    if (decimals == 0) { snprintf(buf, sz, "%ld", (long)raw); return; }
    long div = 1;
    for (uint8_t i = 0; i < decimals && i < 8; i++) div *= 10;
    long whole = raw / div;
    long frac  = raw % div;
    if (frac < 0) frac = -frac;
    snprintf(buf, sz, "%ld.%0*ld", whole, decimals, frac);
}

// "Ax3" / "Bt2" / "--"
void UIEngine::slotLabel(const MixSource& m, char* buf, uint8_t sz) {
    switch (m.type) {
        case SourceType::AXIS:   snprintf(buf, sz, "Ax%d", m.index + 1); break;
        case SourceType::BUTTON: snprintf(buf, sz, "Bt%d", m.index + 1); break;
        case SourceType::CHANNEL: snprintf(buf, sz, "CH%d", m.index + 1); break;
        default:                 snprintf(buf, sz, "--");                break;
    }
}

// Compact one-line summary of a whole channel for the channel list
void UIEngine::chanLabel(const ChannelConfig& c, char* buf, uint8_t sz) {
    uint8_t n = 0;
    for (uint8_t k = 0; k < MAX_MIX_SOURCES; k++)
        if (c.src[k].type != SourceType::NONE) n++;

    if (n == 0) { snprintf(buf, sz, "--"); return; }

    char first[6];
    slotLabel(c.src[0], first, sizeof(first));
    if (n == 1) snprintf(buf, sz, "%s", first);
    else        snprintf(buf, sz, "%s+%d", first, n - 1);
}

// Freeze the channels' current live output as their failsafe positions.
void UIEngine::captureFailsafe(bool all) {
    if (!_live) return;
    uint8_t from = all ? 0 : _sel_ch;
    uint8_t to   = all ? CRSF_NUM_CHANNELS : (uint8_t)(_sel_ch + 1);
    for (uint8_t i = from; i < to; i++) {
        int32_t pct = map((int32_t)_live[i], CRSF_CHANNEL_MIN, CRSF_CHANNEL_MAX, -100, 100);
        _cfg->cfg.channels[i].fs_value = (int8_t)constrain(pct, -100, 100);
        _cfg->cfg.channels[i].fs_mode  = FailsafeMode::VALUE;
    }
    _cfg->markDirty();
}

void UIEngine::refreshProfiles() {
    _pcount = _prof ? _prof->list(_plist, MAX_PROFILES) : 0;
}

void UIEngine::saveAsNewSlot() {
    if (!_prof || _pcount >= MAX_PROFILES) return;   // store is full either way

    bool used[16] = {};
    for (uint8_t i = 0; i < _pcount; i++)
        if (_plist[i].vid == SLOT_PROFILE_VID && _plist[i].pid < 16) used[_plist[i].pid] = true;

    uint8_t slot = 0;
    while (slot < 16 && used[slot]) slot++;
    if (slot >= 16) return;   // all 16 slot numbers taken

    char name[PROFILE_NAME_LEN];
    snprintf(name, sizeof(name), "Slot %d", slot + 1);
    _prof->save(SLOT_PROFILE_VID, slot, name, _cfg->cfg.channels, _cfg->cfg.cal, _cfg->cfg.model_id,
                _cfg->cfg.sf, _cfg->cfg.timers, _cfg->cfg.timer_minute_call,
                _cfg->cfg.trim_sound_off);
    refreshProfiles();
    setActiveProfile(SLOT_PROFILE_VID, slot, name);   // you are on this slot now
}

// Make a just-saved profile "the one you're on" (name looked up from the list).
void UIEngine::adoptProfile(uint16_t vid, uint16_t pid) {
    for (uint8_t i = 0; i < _pcount; i++)
        if (_plist[i].vid == vid && _plist[i].pid == pid) { setActiveProfile(vid, pid, _plist[i].name); return; }
}

// OK + BACK held together: write the live channels, calibration, Model ID
// and special functions back into the active profile, and show a popup.
void UIEngine::saveActiveProfile() {
    _toast_until = millis() + 1400;
    if (_prof && _prof->available() && _active_valid) {
        bool ok = _prof->save(_active_vid, _active_pid, _model_name, _cfg->cfg.channels,
                              _cfg->cfg.cal, _cfg->cfg.model_id, _cfg->cfg.sf,
                              _cfg->cfg.timers, _cfg->cfg.timer_minute_call, _cfg->cfg.trim_sound_off);
        strcpy(_toast_l1, ok ? "Changes saved in" : "Save FAILED:");
        strncpy(_toast_l2, _model_name, sizeof(_toast_l2) - 1);
        _toast_l2[sizeof(_toast_l2) - 1] = 0;
        if (ok) refreshProfiles();
    } else {
        strcpy(_toast_l1, "No profile loaded");
        strcpy(_toast_l2, "Menu>Profiles>Save");
    }
}

void UIEngine::drawToast() {
    disp.clearDisplay();
    disp.drawRect(2, 8, 124, 48, SSD1306_WHITE);
    int16_t w1 = (int16_t)strlen(_toast_l1) * 6;
    disp.setTextSize(1);
    disp.setCursor((128 - w1) / 2, 16);
    disp.print(_toast_l1);
    size_t n2 = strlen(_toast_l2);
    bool big = _active_valid && n2 <= 9;
    disp.setTextSize(big ? 2 : 1);
    int16_t w2 = (int16_t)n2 * (big ? 12 : 6);
    disp.setCursor((128 - w2) / 2, big ? 32 : 36);
    disp.print(_toast_l2);
    disp.setTextSize(1);
    disp.display();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  DRAW
// ═══════════════════════════════════════════════════════════════════════════════

// ── EdgeTX-style overview widgets (pixel layout copied from the reference
//    screenshot: 128x64, everything below is absolute coordinates) ──────────

// A trim marker: 7x7 rounded box with two grip lines, centred on (cx, cy).
// `horiz` picks the grip orientation (vertical grips on a horizontal scale,
// horizontal grips on a vertical one).
static void drawTrimMarker(Adafruit_SSD1306& d, int16_t cx, int16_t cy, bool horiz) {
    d.fillRect(cx - 3, cy - 3, 7, 7, SSD1306_BLACK);
    d.drawFastHLine(cx - 2, cy - 3, 5, SSD1306_WHITE);
    d.drawFastHLine(cx - 2, cy + 3, 5, SSD1306_WHITE);
    d.drawFastVLine(cx - 3, cy - 2, 5, SSD1306_WHITE);
    d.drawFastVLine(cx + 3, cy - 2, 5, SSD1306_WHITE);
    if (horiz) {
        d.drawFastVLine(cx - 1, cy - 1, 3, SSD1306_WHITE);
        d.drawFastVLine(cx + 1, cy - 1, 3, SSD1306_WHITE);
    } else {
        d.drawFastHLine(cx - 1, cy - 1, 3, SSD1306_WHITE);
        d.drawFastHLine(cx - 1, cy + 1, 3, SSD1306_WHITE);
    }
}

// Horizontal trim scale: a 43 px line from x0 along y, marker slides +-18 px
// around the centre for Trim -100..+100.
void UIEngine::drawTrimH(int16_t x0, int16_t y, int8_t trim) {
    disp.drawFastHLine(x0, y, 43, SSD1306_WHITE);
    drawTrimMarker(disp, x0 + 21 + (int16_t)trim * 18 / 100, y, true);
}

// Vertical trim scale: a 43 px line from y0 along x, higher trim = higher up.
void UIEngine::drawTrimV(int16_t x, int16_t y0, int8_t trim) {
    disp.drawFastVLine(x, y0, 43, SSD1306_WHITE);
    drawTrimMarker(disp, x, y0 + 21 - (int16_t)trim * 18 / 100, false);
}

// One joystick box: 23x23 outline with a 5x5 ring marking the stick
// position (ch_x horizontal, ch_y vertical; higher = right / up).
void UIEngine::drawStickBox(int16_t x, int16_t y, uint8_t ch_x, uint8_t ch_y) {
    disp.drawRect(x, y, 23, 23, SSD1306_WHITE);

    float vx = 0.5f, vy = 0.5f;
    if (_live) {
        vx = (float)(_live[ch_x] - CRSF_CHANNEL_MIN) / (CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN);
        vy = (float)(_live[ch_y] - CRSF_CHANNEL_MIN) / (CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN);
    }
    vx = constrain(vx, 0.0f, 1.0f);
    vy = constrain(vy, 0.0f, 1.0f);
    int16_t cx = x + 3 + (int16_t)(vx * 16.0f + 0.5f);          // centre travels x+3 .. x+19
    int16_t cy = y + 3 + (int16_t)((1.0f - vy) * 16.0f + 0.5f);

    static const uint8_t RING[5] = { 0x0E, 0x15, 0x1B, 0x15, 0x0E };   // .###. #.#.# ##.## #.#.# .###.
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 5; c++)
            if (RING[r] & (0x10 >> c)) disp.drawPixel(cx - 2 + c, cy - 2 + r, SSD1306_WHITE);
}

// STATUS carousel page 0: EdgeTX-style main screen.
//   row 1       model name, double size
//   row 2       first timer: small "TM1" + double-size M:SS (colon blinks while running)
//   sides       CH3 / CH2 vertical trims; CH4 / CH1 horizontal trims under the boxes
//   middle      two stick boxes, Mode 2: left = CH4 horizontal + CH3 vertical,
//               right = CH1 horizontal + CH2 vertical
void UIEngine::drawStatusMain() {
    ChannelConfig* ch = _cfg->cfg.channels;

    // Active profile's name, always: double size when it fits (9 chars),
    // otherwise normal size so the whole name (up to 13) still shows.
    const char* nm = _active_valid ? _model_name : "No profile";
    if (strlen(nm) <= 9) { disp.setTextSize(2); disp.setCursor(10, 1); }
    else                 { disp.setTextSize(1); disp.setCursor(10, 4); }
    disp.print(nm);

    // First timer, if any.
    uint8_t tslots[MAX_TIMERS];
    if (timerSlots(tslots, MAX_TIMERS) > 0) {
        uint8_t        slot = tslots[0];
        const TimerRT& tr   = _timer_rt[slot];
        uint16_t t   = (uint16_t)constrain((long)tr.value_s, 0L, 5999L);
        uint8_t  mm  = t / 60, ss = t % 60;
        int16_t  mx  = (mm >= 10) ? 73 : 85;      // seconds always start at x = 101
        disp.setTextSize(2);
        if (mm >= 10) { disp.setCursor(mx, 17); disp.print(mm / 10); }
        disp.setCursor(85, 17);  disp.print(mm % 10);
        disp.setCursor(101, 17); disp.print(ss / 10);
        disp.setCursor(113, 17); disp.print(ss % 10);
        if (!tr.running || (millis() / 500) % 2 == 0) {   // colon
            disp.fillRect(98, 21, 2, 2, SSD1306_WHITE);
            disp.fillRect(98, 27, 2, 2, SSD1306_WHITE);
        }
        disp.setTextSize(1);
        char lbl[5];
        snprintf(lbl, sizeof(lbl), "TM%d", slot + 1);
        disp.setCursor(mx - 27, 24);
        disp.print(lbl);
    }
    disp.setTextSize(1);

    drawStickBox(33, 33, 3, 2);    // left:  CH4 horizontal, CH3 vertical
    drawStickBox(72, 33, 0, 1);    // right: CH1 horizontal, CH2 vertical

    drawTrimV(3,   10, ch[2].trim);   // left edge:  CH3
    drawTrimV(124, 10, ch[1].trim);   // right edge: CH2
    drawTrimH(4,   60, ch[3].trim);   // bottom left:  CH4
    drawTrimH(81,  60, ch[0].trim);   // bottom right: CH1
}

void UIEngine::drawStatus() {
    disp.clearDisplay();

    uint8_t tslots[MAX_TIMERS];
    uint8_t nt = timerSlots(tslots, MAX_TIMERS);
    uint8_t nd = diagPageCount();
    uint8_t total = 4 + nt + nd;   // overview + 2 bar pages + ELRS stats + timers + diagnostics
    if (_status_view >= total) _status_view = 0;

    if (_status_view == 0) {
        // EdgeTX-style overview: its own layout, no shared header/ELRS line.
        drawStatusMain();
        disp.display();
        return;
    }

    if (_status_view >= 4 + nt) {
        // Diagnostics: full-screen content that draws its own header.
        drawDiagContent(_status_view - 4 - nt);
        disp.display();
        return;
    }

    // Header: mode + input source (bars, stats & timer views share this)
    char h[24];
    if (_safe_mode)
        snprintf(h, sizeof(h), "SAFE MODE - no USB");
    else
        snprintf(h, sizeof(h), "%s  %s", deviceModeName(_cfg->cfg.mode),
                 _in.connected ? inputSourceName(_in.source) : "--");
    header(h);

    // ELRS link line
    LinkStats lk = _crsf->linkStats();
    disp.setCursor(0, 12);
    if (_cfg->cfg.out_mode == OutputMode::PPM) {
        char b[24];
        snprintf(b, sizeof(b), "PPM %dch %dus",
                 _cfg->cfg.ppm.channels, _cfg->cfg.ppm.frame_us);
        disp.print(b);
    } else if (_crsf->deviceFound()) {
        if (lk.valid) {
            char b[24];
            snprintf(b, sizeof(b), "ELRS LQ%3d%% %ddBm", lk.lq, lk.rssi1);
            disp.print(b);
        } else disp.print(F("ELRS connected"));
    } else if (_crsf->txOnly()) {
        // Transmitting fine but nothing coming back. Normal on a single-wire
        // module with no telemetry return path.
        disp.print(F("ELRS TX only"));
    } else {
        disp.print(F("ELRS: linking..."));
    }
    if (_tx_cut)                             { disp.setCursor(98, 12);  disp.print(F("CUT")); }
    else if (_mixer && _mixer->inFailsafe()) { disp.setCursor(110, 12); disp.print(F("FS")); }

    if (_status_view < 3) {
        // Eight channel bars; UP/DOWN step between CH1-8 and CH9-16. Bars
        // reflect the actual output - after mixing and after any Override
        // Channel row has had its say - so an "o" under a bar means this
        // channel isn't coming from the normal mix right now, not just that
        // a row exists somewhere that could affect it.
        const int16_t BW = 13, BG = 3, BH = 16, BT = 24;
        uint8_t  first = (_status_view == 1) ? 0 : 8;
        uint16_t ovr   = overriddenChannelMask();
        for (int i = 0; i < 8; i++) {
            uint8_t ci = first + i;
            int16_t x  = 3 + i * (BW + BG);
            float v = _live
                ? (float)(_live[ci] - CRSF_CHANNEL_MIN) / (CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN)
                : 0.5f;
            v = constrain(v, 0.0f, 1.0f);
            disp.drawRect(x, BT, BW, BH, SSD1306_WHITE);
            int16_t fh = (int16_t)(v * (BH - 2));
            if (fh > 0) disp.fillRect(x + 1, BT + BH - 1 - fh, BW - 2, fh, SSD1306_WHITE);
            // Right-align the label so two-digit numbers stay under their bar
            disp.setCursor(x + (ci >= 9 ? 1 : 4), BT + BH + 2);
            disp.print(ci + 1);
            if (ovr & (1u << ci)) { disp.setCursor(x + 4, BT + BH + 11); disp.print('o'); }
        }
    } else if (_status_view == 3) {
        // ELRS link statistics - everything the LINK_STATISTICS frame
        // carries that the one-line summary above doesn't already show.
        char b[24];
        if (_cfg->cfg.out_mode != OutputMode::CRSF) {
            disp.setCursor(0, 28); disp.print(F("PPM mode - no ELRS"));
            disp.setCursor(0, 38); disp.print(F("link to report"));
        } else if (!_crsf->deviceFound()) {
            disp.setCursor(0, 28); disp.print(F("Waiting for"));
            disp.setCursor(0, 38); disp.print(F("ELRS module..."));
        } else if (!lk.valid) {
            disp.setCursor(0, 28); disp.print(F("Connected - no link"));
            disp.setCursor(0, 38); disp.print(F("statistics yet"));
        } else {
            snprintf(b, sizeof(b), "RSSI2 %4ddBm  Ant %u", lk.rssi2, lk.antenna + 1);
            disp.setCursor(0, 20); disp.print(b);
            snprintf(b, sizeof(b), "SNR %4ddB PwrIdx%3u", lk.snr, lk.tx_pwr_idx);
            disp.setCursor(0, 29); disp.print(b);
            snprintf(b, sizeof(b), "DL LQ%3u%% %4ddBm", lk.dl_lq, lk.dl_rssi);
            disp.setCursor(0, 38); disp.print(b);
            snprintf(b, sizeof(b), "DL SNR %4ddB", lk.dl_snr);
            disp.setCursor(0, 47); disp.print(b);
        }
    } else {
        // A selected timer, shown big in the same area the bars would use.
        uint8_t          slot = tslots[_status_view - 4];
        const TimerRT&   tr   = _timer_rt[slot];
        const TimerDef&  td   = _cfg->cfg.timers[slot];

        uint16_t total_s = (uint16_t)constrain((long)tr.value_s, 0L, 5999L);
        char     buf[8];
        snprintf(buf, sizeof(buf), "%02u:%02u", total_s / 60, total_s % 60);
        disp.setTextSize(2);
        disp.setCursor(28, 28);
        disp.print(buf);
        disp.setTextSize(1);

        char lbl[24];
        snprintf(lbl, sizeof(lbl), "Timer %d  %s  %s", slot + 1,
                 td.kind == TimerKind::COUNTDOWN ? "down" : "up",
                 tr.running ? "running" : "paused");
        disp.setCursor(0, 50);
        disp.print(lbl);

    }
    disp.display();
}

static const char* MAIN_ITEMS[] = {
    "Profiles", "Control Setup", "ELRS Setup", "Sound Setup",
    "Settings", "Save Config"
};
static const uint8_t MAIN_N = 6;

static const char* CONTROL_ITEMS[] = { "Channels", "Special Func", "Calibration" };

void UIEngine::drawControlMenu() {
    disp.clearDisplay();
    header("Control Setup");
    for (uint8_t i = 0; i < 3; i++) row(i, _cursor == i, CONTROL_ITEMS[i]);
    footer("Enter", "Back");
    disp.display();
}

void UIEngine::hControlMenu(const JoyEvent& e) {
    wrapCursor(_cursor, 3, e.up, e.down);
    if (e.ok) {
        switch (_cursor) {
            case 0: _page = Page::CHANNEL_LIST; _cursor = 0; break;
            case 1: _page = Page::SF_HOME;      _cursor = 0; break;
            case 2: _page = Page::CAL_MENU;     _cursor = 0; break;
        }
    }
    if (e.back) { _page = Page::MAIN_MENU; _cursor = 1; }
}

void UIEngine::drawMainMenu() {
    disp.clearDisplay();
    header("Menu");
    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < MAIN_N; i++)
        row(i, _cursor == (i + scroll), MAIN_ITEMS[i + scroll]);
    scrollbar(MAIN_N, VIS_ROWS, scroll);
    footer("Enter", "Back");
    disp.display();
}

void UIEngine::drawModeSelect() {
    disp.clearDisplay();
    header("Device Mode");
    const uint8_t N = (uint8_t)DeviceMode::_COUNT;
    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < N; i++) {
        uint8_t m = i + scroll;
        char b[22];
        snprintf(b, sizeof(b), "%s%s", deviceModeName((DeviceMode)m),
                 (DeviceMode)m == _cfg->cfg.mode ? "  *" : "");
        row(i, _cursor == m, b);
    }
    scrollbar(N, VIS_ROWS, scroll);
    footer("Select", "Back");
    disp.display();
}

void UIEngine::drawModeConfirm() {
    disp.clearDisplay();
    header("Switch Mode?");
    disp.setCursor(0, 14); disp.print(deviceModeName((DeviceMode)_cursor));
    disp.setCursor(0, 26); disp.print(F("Resets channels"));
    disp.setCursor(0, 34); disp.print(F("and reboots."));
    yesNo(_yesno);
    disp.display();
}

void UIEngine::drawChannelList() {
    disp.clearDisplay();
    header("Channels");
    const uint8_t N = CRSF_NUM_CHANNELS + 1;   // + the capture row
    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;

    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < N; i++) {
        uint8_t ci  = i + scroll;
        bool    sel = (_cursor == ci);

        if (ci >= CRSF_NUM_CHANNELS) {          // action row
            row(i, sel, "Capture failsafe");
            continue;
        }
        char    src[10];
        chanLabel(_cfg->cfg.channels[ci], src, sizeof(src));

        char line[18];
        snprintf(line, sizeof(line), "%2d %-6s", ci + 1, src);
        row(i, sel, line);

        int16_t  y   = ROW_Y0 + i * ROW_H;
        uint16_t col = sel ? SSD1306_BLACK : SSD1306_WHITE;

        // Left bar: the raw input feeding slot 1 of this channel.
        // Right bar: the finished channel output.
        // Seeing them side by side makes it obvious whether a dead channel is
        // a mapping problem or a missing input.
        const MixSource& m0 = _cfg->cfg.channels[ci].src[0];
        if (m0.type == SourceType::AXIS && m0.index < _in.num_axes) {
            float iv = ((float)_in.axes[m0.index] + 32768.0f) / 65535.0f;
            bar(62, y - 1, 26, 8, constrain(iv, 0.0f, 1.0f), col);
        } else if (m0.type == SourceType::BUTTON && m0.index < _in.num_buttons) {
            bool on = (_in.buttons >> m0.index) & 1u;
            if (on) disp.fillRect(62, y - 1, 26, 8, col);
            else    disp.drawRect(62, y - 1, 26, 8, col);
        }

        float v = _live
            ? (float)(_live[ci] - CRSF_CHANNEL_MIN) / (CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN)
            : 0.5f;
        bar(92, y - 1, 28, 8, v, col);
    }
    scrollbar(N, VIS_ROWS, scroll);
    footer(_in.connected ? "Edit" : "No input", "Back");
    disp.display();
}

// ── Channel editor: rest position, 3 mix slots, expo, weight, failsafe ──────
#define CE_ROWS 10
void UIEngine::drawChannelEdit() {
    const ChannelConfig& c = _cfg->cfg.channels[_sel_ch];
    disp.clearDisplay();
    char h[20]; snprintf(h, sizeof(h), "CH%d", _sel_ch + 1);
    header(h);
    headerBar(_live
        ? (float)(_live[_sel_ch] - CRSF_CHANNEL_MIN) / (CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN)
        : 0.5f);

    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    char line[22];

    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < CE_ROWS; i++) {
        uint8_t r = i + scroll;
        if (r == 0) {
            snprintf(line, sizeof(line), "Rest at   %s", mixTargetName(c.base));
        } else if (r <= MAX_MIX_SOURCES) {
            const MixSource& m = c.src[r - 1];
            char sl[6]; slotLabel(m, sl, sizeof(sl));
            if (m.type == SourceType::NONE)
                snprintf(line, sizeof(line), "%d  --", r);
            else if (m.type == SourceType::BUTTON)
                snprintf(line, sizeof(line), "%d %s>%-4s%d%%",
                         r, sl, mixTargetName(m.target), m.weight);
            else
                snprintf(line, sizeof(line), "%d %s   %+d%%", r, sl, m.weight);
        } else if (r == 4) {
            snprintf(line, sizeof(line), "Shape     %s",
                     c.curve_on ? "Curve" : "Expo");
        } else if (r == 5) {
            if (c.curve_on) snprintf(line, sizeof(line), "Edit curve...");
            else            snprintf(line, sizeof(line), "Expo      %+d%%", c.expo);
        } else if (r == 6) {
            snprintf(line, sizeof(line), "Weight    %d%%", c.weight);
        } else if (r == 7) {
            snprintf(line, sizeof(line), "Trim      %+d%%", c.trim);
        } else if (r == 8) {
            if (c.fs_mode == FailsafeMode::HOLD)
                snprintf(line, sizeof(line), "Failsafe  Hold");
            else
                snprintf(line, sizeof(line), "Failsafe  %+d%%", c.fs_value);
        } else {
            snprintf(line, sizeof(line), "Clear channel");
        }
        row(i, _cursor == r, line);
    }
    scrollbar(CE_ROWS, VIS_ROWS, scroll);
    if      (_editing)                     footer("Done", "Done", "Up/Dn adjust");
    else if (_cursor >= 1 && _cursor <= 3) footer("Slot",   "Back");
    else if (_cursor == 0 || _cursor == 4) footer("Change", "Back");
    else if (_cursor == 5 && c.curve_on)   footer("Curve",  "Back");
    else if (_cursor == 7)                 footer("Open",   "Back");
    else if (_cursor == 8 || _cursor == 9) footer("Open",   "Back");
    else                                    footer("Edit",  "Back");
    disp.display();
}

// ── Slot editor ─────────────────────────────────────────────────────────────
void UIEngine::drawSlotEdit() {
    MixSource& m = _cfg->cfg.channels[_sel_ch].src[_slot];
    disp.clearDisplay();
    char h[22]; snprintf(h, sizeof(h), "CH%d Slot %d", _sel_ch + 1, _slot + 1);
    header(h);

    char sl[6];
    slotLabel(m, sl, sizeof(sl));

    // Built up front as a plain array of lines, then rendered through the
    // same scroll window every other list screen uses - so a future row
    // added here (a new MixSource type's extra field, say) just scrolls
    // instead of silently running off the bottom of the display.
    char lines[5][22];
    uint8_t n = 0;
    snprintf(lines[n++], 22, "Source   %s", sl);
    snprintf(lines[n++], 22, "Weight   %+d%%", m.weight);
    snprintf(lines[n++], 22, "Reverse  %s", m.weight < 0 ? "Yes" : "No");
    if (m.type == SourceType::BUTTON) {
        snprintf(lines[n++], 22, "Push to  %s", mixTargetName(m.target));
        snprintf(lines[n++], 22, "Button   %s",
                 m.btn_mode == ButtonMode::STICKY ? "Sticky" : "Moment");
    } else {
        snprintf(lines[n++], 22, "Clear slot");
    }

    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < n; i++) {
        uint8_t r = i + scroll;
        row(i, _cursor == r, lines[r]);
    }
    scrollbar(n, VIS_ROWS, scroll);

    if      (_editing)     footer("Done", "Done", "Up/Dn adjust");
    else if (_cursor == 0) footer("Learn", "Back", "Hold: clear");
    else if (_cursor == 1) footer("Edit", "Back");
    else if (_cursor == 2) footer("Flip", "Back");
    else                   footer("Change", "Back");
    disp.display();
}

void UIEngine::drawLearnWait() {
    disp.clearDisplay();
    char h[22];
    if (_learn_what == LEARN_SLOT)
        snprintf(h, sizeof(h), "CH%d.%d Learn", _sel_ch + 1, _slot + 1);
    else if (_learn_what == LEARN_SOUND_TRIG)
        snprintf(h, sizeof(h), "Trigger %d Learn", _sound_trig_sel + 1);
    else if (_learn_what == LEARN_SF_TRIG)
        snprintf(h, sizeof(h), "Func %d Learn", _sf_sel + 1);
    else
        snprintf(h, sizeof(h), "CH%d Trim %s", _sel_ch + 1,
                 _learn_what == LEARN_TRIM_UP ? "+" : "-");
    header(h);

    if (_learn_what == LEARN_SLOT || _learn_what == LEARN_SOUND_TRIG || _learn_what == LEARN_SF_TRIG) {
        disp.setCursor(0, 14); disp.print(F("Move a stick or"));
        disp.setCursor(0, 24); disp.print(F("press a button"));
    } else {
        disp.setCursor(0, 14); disp.print(F("Press the button to"));
        disp.setCursor(0, 24); disp.print(F("use for this step"));
    }

    if (!_in.connected) {
        disp.setCursor(0, 38); disp.print(F("No input detected!"));
    } else {
        bar(4, 38, 120, 8, _teach.progress(), SSD1306_WHITE);
    }
    footer("", "Cancel");
    disp.display();
}

void UIEngine::drawLearnSaved() {
    disp.clearDisplay();
    header("Assigned");
    const MixSource& m = _cfg->cfg.channels[_sel_ch].src[_slot];
    char sl[6]; slotLabel(m, sl, sizeof(sl));
    disp.setTextSize(2);
    disp.setCursor(12, 22);
    disp.print(F("CH")); disp.print(_sel_ch + 1);
    disp.print(F(" ")); disp.print(sl);
    disp.setTextSize(1);
    disp.display();
}

// ── Per-channel failsafe ────────────────────────────────────────────────────
void UIEngine::drawFsEdit() {
    const ChannelConfig& c = _cfg->cfg.channels[_sel_ch];
    disp.clearDisplay();
    char h[22]; snprintf(h, sizeof(h), "CH%d FS", _sel_ch + 1);
    header(h);
    // Preview of where the channel sits when the link is lost
    headerBar(c.fs_mode == FailsafeMode::VALUE
                ? ((float)c.fs_value + 100.0f) / 200.0f
                : 0.5f);

    char lines[3][22];
    uint8_t n = 0;
    snprintf(lines[n++], 22, "Mode     %s", failsafeModeName(c.fs_mode));
    if (c.fs_mode == FailsafeMode::VALUE)
        snprintf(lines[n++], 22, "Position %+d%%", c.fs_value);
    else
        snprintf(lines[n++], 22, "Position  --");
    snprintf(lines[n++], 22, "Capture now");

    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < n; i++) {
        uint8_t r = i + scroll;
        row(i, _cursor == r, lines[r]);
    }
    scrollbar(n, VIS_ROWS, scroll);

    if      (_editing)     footer("Done", "Done", "Up/Dn adjust");
    else if (_cursor == 2) footer("Capture", "Back");
    else if (_cursor == 0) footer("Toggle", "Back");
    else                   footer(c.fs_mode == FailsafeMode::VALUE ? "Edit" : "", "Back");
    disp.display();
}

void UIEngine::drawFsCaptureAll() {
    disp.clearDisplay();
    header("Capture Failsafe");
    disp.setCursor(0, 15); disp.print(F("Store the current"));
    disp.setCursor(0, 25); disp.print(F("stick positions for"));
    disp.setCursor(0, 35); disp.print(F("all 16 channels?"));
    yesNo(_yesno);
    disp.display();
}

// ── Startup throttle warning ────────────────────────────────────────────────
void UIEngine::drawThrottleWarn() {
    disp.clearDisplay();
    disp.fillRect(0, 0, 128, 12, SSD1306_WHITE);
    disp.setTextColor(SSD1306_BLACK);
    disp.setCursor(20, 2); disp.print(F("!! THROTTLE !!"));
    disp.setTextColor(SSD1306_WHITE);

    disp.setCursor(0, 17);
    disp.print(F("Move CH"));
    disp.print(_cfg->cfg.thr_ch + 1);
    disp.print(F(" to minimum"));
    disp.setCursor(0, 27); disp.print(F("to enable output."));

    // live throttle position so you can see it drop
    if (_live) {
        uint8_t t = _cfg->cfg.thr_ch;
        float v = (float)(_live[t] - CRSF_CHANNEL_MIN) /
                  (CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN);
        bar(4, 38, 120, 10, constrain(v, 0.0f, 1.0f), SSD1306_WHITE);
    }
    footer("Menu");
    disp.display();
}

// ── 5-point curve editor ────────────────────────────────────────────────────
// Graph on the left, point values on the right. A vertical marker shows where
// the live input currently sits on the curve.
void UIEngine::drawCurveEdit() {
    const ChannelConfig& c = _cfg->cfg.channels[_sel_ch];
    disp.clearDisplay();
    char h[22]; snprintf(h, sizeof(h), "CH%d Curve", _sel_ch + 1);
    header(h);

    // ── graph box ───────────────────────────────────────────────────────────
    const int16_t GX = 0, GY = 13, GW = 58, GH = 40;
    disp.drawRect(GX, GY, GW, GH, SSD1306_WHITE);
    // centre cross-hairs
    for (int16_t x = GX + 2; x < GX + GW - 1; x += 3)
        disp.drawPixel(x, GY + GH / 2, SSD1306_WHITE);
    for (int16_t y = GY + 2; y < GY + GH - 1; y += 3)
        disp.drawPixel(GX + GW / 2, y, SSD1306_WHITE);

    // plot the four segments
    for (uint8_t i = 0; i < 4; i++) {
        int16_t x0 = GX + 1 + (int16_t)((GW - 3) * i / 4);
        int16_t x1 = GX + 1 + (int16_t)((GW - 3) * (i + 1) / 4);
        int16_t y0 = GY + GH - 2 -
                     (int16_t)((int32_t)(c.curve[i]     + 100) * (GH - 4) / 200);
        int16_t y1 = GY + GH - 2 -
                     (int16_t)((int32_t)(c.curve[i + 1] + 100) * (GH - 4) / 200);
        disp.drawLine(x0, y0, x1, y1, SSD1306_WHITE);
    }

    // mark the selected point
    {
        int16_t px = GX + 1 + (int16_t)((GW - 3) * _cpt / 4);
        int16_t py = GY + GH - 2 -
                     (int16_t)((int32_t)(c.curve[_cpt] + 100) * (GH - 4) / 200);
        disp.fillRect(px - 1, py - 1, 3, 3, SSD1306_WHITE);
        disp.drawRect(px - 3, py - 3, 7, 7, SSD1306_WHITE);
    }

    // live input position
    if (_live) {
        float v = (float)(_live[_sel_ch] - CRSF_CHANNEL_MIN) /
                  (CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN);
        int16_t lx = GX + 1 + (int16_t)(constrain(v, 0.0f, 1.0f) * (GW - 3));
        for (int16_t y = GY + 1; y < GY + GH - 1; y += 2)
            disp.drawPixel(lx, y, SSD1306_WHITE);
    }

    // ── point list ──────────────────────────────────────────────────────────
    static const char* PLBL[5] = { "-100", " -50", "   0", " +50", "+100" };
    for (uint8_t i = 0; i < 5; i++) {
        int16_t y = 13 + i * 8;
        bool sel = (i == _cpt);
        if (sel) {
            disp.fillRect(GW + 2, y - 1, 128 - GW - 2, 9, SSD1306_WHITE);
            disp.setTextColor(SSD1306_BLACK);
        }
        char b[14];
        snprintf(b, sizeof(b), "%s %+4d", PLBL[i], c.curve[i]);
        disp.setCursor(GW + 4, y);
        disp.print(b);
        if (sel) disp.setTextColor(SSD1306_WHITE);
    }

    if (_editing) footer("Done", "Done", "Up/Dn adjust");
    else           footer("Edit", "Back", "Hold: straighten");
    disp.display();
}

// ── Calibration ─────────────────────────────────────────────────────────────
// Trim submenu: the trim value itself, the two buttons that nudge it, and how
// far each press moves it. Reached with SET on the channel editor's Trim row.
void UIEngine::drawTrimEdit() {
    const ChannelConfig& c = _cfg->cfg.channels[_sel_ch];
    disp.clearDisplay();
    char h[20]; snprintf(h, sizeof(h), "CH%d Trim", _sel_ch + 1);
    header(h);
    headerBar(_live
        ? (float)(_live[_sel_ch] - CRSF_CHANNEL_MIN) / (CRSF_CHANNEL_MAX - CRSF_CHANNEL_MIN)
        : 0.5f);

    char line[22], b[8];
    const uint8_t N = 6;
    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;

    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < N; i++) {
        uint8_t r = i + scroll;
        switch (r) {
            case 0:
                snprintf(line, sizeof(line), "Trim      %+d%%", c.trim);
                break;
            case 1:
                if (c.trim_up == TRIM_BTN_NONE) snprintf(b, sizeof(b), "--");
                else                            snprintf(b, sizeof(b), "Bt%d", c.trim_up + 1);
                snprintf(line, sizeof(line), "Add btn   %s", b);
                break;
            case 2:
                if (c.trim_dn == TRIM_BTN_NONE) snprintf(b, sizeof(b), "--");
                else                            snprintf(b, sizeof(b), "Bt%d", c.trim_dn + 1);
                snprintf(line, sizeof(line), "Sub btn   %s", b);
                break;
            case 3:
                snprintf(line, sizeof(line), "Step      %d%%", c.trim_step);
                break;
            case 4:
                snprintf(line, sizeof(line), "Sound     %s",
                         ((_cfg->cfg.trim_sound_off >> _sel_ch) & 1u) ? "OFF" : "ON");
                break;
            default:
                snprintf(line, sizeof(line), "Centre now");
                break;
        }
        row(i, _cursor == r, line);
    }
    scrollbar(N, VIS_ROWS, scroll);

    if (_editing) {
        footer("Done", "Done", "Up/Dn adjust");
    } else if (_cursor == 1 || _cursor == 2) {
        footer("Learn", "Back", "Hold: clear");
    } else if (_cursor == 4) {
        footer("Toggle", "Back");
    } else if (_cursor == 5) {
        footer("Zero", "Back");
    } else {
        footer("Edit", "Back");
    }
    disp.display();
}

void UIEngine::drawCalMenu() {
    disp.clearDisplay();
    // The axis count used to be printed on its own line in row 3's slot,
    // which only worked because this screen never had a 4th row - folded
    // into the header instead so all VIS_ROWS slots stay free for the
    // scrollable list below (and a future row doesn't silently collide
    // with it).
    char h[22]; snprintf(h, sizeof(h), "Calibration Ax:%d", _in.num_axes);
    header(h);

    char lines[3][22];
    uint8_t n = 0;
    snprintf(lines[n++], 22, "Calibrate sticks");
    snprintf(lines[n++], 22, "Deadband    %d%%", _cfg->cfg.cal[0].deadband);
    snprintf(lines[n++], 22, "Reset calibration");

    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < n; i++) {
        uint8_t r = i + scroll;
        row(i, _cursor == r, lines[r]);
    }
    scrollbar(n, VIS_ROWS, scroll);

    if (_editing) footer("Done", "Done", "Up/Dn adjust");
    else footer(_cursor == 0 ? "Start" : _cursor == 1 ? "Edit" : "Reset", "Back");
    disp.display();
}

void UIEngine::drawCalSweep() {
    disp.clearDisplay();
    header("Step 1 of 2");
    disp.setCursor(0, 14); disp.print(F("Move every stick"));
    disp.setCursor(0, 24); disp.print(F("to all extremes."));

    // count axes that have shown meaningful travel
    uint8_t seen = 0;
    for (uint8_t i = 0; i < _in.num_axes && i < MAX_INPUT_AXES; i++)
        if ((int32_t)_cmax[i] - _cmin[i] > 8000) seen++;

    char b[22];
    snprintf(b, sizeof(b), "Detected %d of %d", seen, _in.num_axes);
    disp.setCursor(0, 38); disp.print(b);

    bar(0, 48, 128, 6,
        _in.num_axes ? (float)seen / _in.num_axes : 0.0f, SSD1306_WHITE);
    footer("Next", "Cancel");
    disp.display();
}

void UIEngine::drawCalCenter() {
    disp.clearDisplay();
    header("Step 2 of 2");
    disp.setCursor(0, 16); disp.print(F("Release everything"));
    disp.setCursor(0, 26); disp.print(F("and let it centre."));
    disp.setCursor(0, 40); disp.print(F("Then press OK."));
    footer("Save", "Cancel");
    disp.display();
}

// ── Safety ──────────────────────────────────────────────────────────────────
void UIEngine::drawSafety() {
    disp.clearDisplay();
    header("Settings");
    char line[22];

    const uint8_t N = 10;
    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;

    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < N; i++) {
        uint8_t r = i + scroll;
        switch (r) {
            case 0: snprintf(line, sizeof(line), "Safety       >"); break;
            case 1: snprintf(line, sizeof(line), "Link setup   >"); break;
            case 2:
                if (_cfg->cfg.model_id) snprintf(line, sizeof(line), "Model ID    %u", _cfg->cfg.model_id);
                else                     snprintf(line, sizeof(line), "Model ID    off");
                break;
            case 3: snprintf(line, sizeof(line), "Inactivity  %s",
                             _cfg->cfg.inactivity.enabled ? "ON" : "OFF"); break;
            case 4: snprintf(line, sizeof(line), "Idle mins   %u",
                             _cfg->cfg.inactivity.minutes); break;
            case 5: snprintf(line, sizeof(line), "Pad port    %d",
                             _cfg->cfg.hid_port + 1); break;
            case 6: snprintf(line, sizeof(line), "Device mode  >"); break;
            case 7: snprintf(line, sizeof(line), "Input info   >"); break;
            case 8: snprintf(line, sizeof(line), "Raw HID     %s",
                             _cfg->cfg.force_raw_hid ? "ON" : "AUTO"); break;
            default: snprintf(line, sizeof(line), "Restart device"); break;
        }
        row(i, _cursor == r, line);
    }
    scrollbar(N, VIS_ROWS, scroll);
    if      (_editing)      footer("Done", "Done", "Up/Dn adjust");
    else if (_cursor == 9)  footer("Restart", "Back");
    else if (_cursor == 2 || _cursor == 4) footer("Edit", "Back");
    else if (_cursor <= 1 || _cursor == 6 || _cursor == 7) footer("Open", "Back");
    else                    footer("Toggle", "Back");
    disp.display();
}

void UIEngine::drawSafetySub() {
    disp.clearDisplay();
    header("Safety");
    char line[22];
    const uint8_t N = 5;
    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < N; i++) {
        uint8_t r = i + scroll;
        switch (r) {
            case 0: snprintf(line, sizeof(line), "Thr check   %s",
                             _cfg->cfg.thr_check ? "ON" : "OFF"); break;
            case 1: snprintf(line, sizeof(line), "Thr channel CH%d",
                             _cfg->cfg.thr_ch + 1); break;
            case 2: snprintf(line, sizeof(line), "Link loss   %s",
                             linkLossName(_cfg->cfg.loss_action)); break;
            case 3: snprintf(line, sizeof(line), "Grace       %dms",
                             _cfg->cfg.loss_grace_ms); break;
            default: snprintf(line, sizeof(line), "Stall recov %s",
                             _cfg->cfg.stall_recovery ? "ON" : "OFF"); break;
        }
        row(i, _cursor == r, line);
    }
    scrollbar(N, VIS_ROWS, scroll);
    if      (_editing)                         footer("Done", "Done", "Up/Dn adjust");
    else if (_cursor == 1 || _cursor == 3)     footer("Edit", "Back");
    else                                       footer("Toggle", "Back");
    disp.display();
}

// ── Sound Setup ─────────────────────────────────────────────────────────────
void UIEngine::drawSoundList() {
    disp.clearDisplay();
    header("Sound Setup");
    char l[22];
    for (uint8_t i = 0; i < 4; i++) {
        switch (i) {
            case 0: snprintf(l, sizeof(l), "Voice       %s", _cfg->cfg.sound.enabled ? "ON" : "OFF"); break;
            case 1: snprintf(l, sizeof(l), "Volume      %d", _cfg->cfg.sound.volume); break;
            case 2: snprintf(l, sizeof(l), "Events"); break;
            default: snprintf(l, sizeof(l), "Triggers"); break;
        }
        row(i, _cursor == i, l);
    }
    if (_editing)           footer("Done", "Done", "Up/Dn adjust");
    else if (_cursor == 0)  footer("Toggle", "Back");
    else if (_cursor == 1)  footer("Edit", "Back");
    else                    footer("Open", "Back");
    disp.display();
}

void UIEngine::drawSoundEvents() {
    disp.clearDisplay();
    header("Sound Events");
    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < SOUND_EVENT_COUNT; i++) {
        uint8_t  r = i + scroll;
        uint16_t t = _cfg->cfg.sound.event_track[r];
        char line[22];
        if (t) snprintf(line, sizeof(line), "%-11s %4u", SOUND_EVENT_LABEL[r], t);
        else   snprintf(line, sizeof(line), "%-11s off",  SOUND_EVENT_LABEL[r]);
        row(i, _cursor == r, line);
    }
    scrollbar(SOUND_EVENT_COUNT, VIS_ROWS, scroll);
    footer("Edit", "Back");
    disp.display();
}

void UIEngine::drawSoundEventEdit() {
    disp.clearDisplay();
    header(SOUND_EVENT_LABEL[_sound_event_sel]);
    uint16_t t = _cfg->cfg.sound.event_track[_sound_event_sel];

    disp.setTextSize(2);
    disp.setCursor(20, 24);
    if (t) disp.print(t); else disp.print(F("off"));
    disp.setTextSize(1);
    disp.setCursor(0, 50);
    disp.print(F("Track number, 0 = off"));

    footer("Done", "Done", "Up/Dn +-1");
    disp.display();
}

void UIEngine::drawSoundTrigList() {
    disp.clearDisplay();
    header("Sound Triggers");
    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < MAX_SOUND_TRIGGERS; i++) {
        uint8_t r = i + scroll;
        const SoundTrigger& t = _cfg->cfg.sound.triggers[r];
        char line[22];
        if (t.type == SourceType::NONE) {
            snprintf(line, sizeof(line), "%d: --", r + 1);
        } else {
            MixSource tmp; tmp.type = t.type; tmp.index = t.index;
            char sl[6]; slotLabel(tmp, sl, sizeof(sl));
            if (t.track) snprintf(line, sizeof(line), "%d: %-4s -> %u", r + 1, sl, t.track);
            else         snprintf(line, sizeof(line), "%d: %-4s -> off", r + 1, sl);
        }
        row(i, _cursor == r, line);
    }
    scrollbar(MAX_SOUND_TRIGGERS, VIS_ROWS, scroll);
    footer("Edit", "Back");
    disp.display();
}

void UIEngine::drawSoundTrigEdit() {
    const SoundTrigger& t = _cfg->cfg.sound.triggers[_sound_trig_sel];
    bool axis = (t.type == SourceType::AXIS);

    disp.clearDisplay();
    char title[16]; snprintf(title, sizeof(title), "Trigger %d", _sound_trig_sel + 1);
    header(title);

    char sl[6];
    if (t.type == SourceType::NONE) snprintf(sl, sizeof(sl), "--");
    else { MixSource tmp; tmp.type = t.type; tmp.index = t.index; slotLabel(tmp, sl, sizeof(sl)); }

    char lines[4][22];
    uint8_t n = 0;
    snprintf(lines[n++], 22, "Source     %s", sl);
    if (axis) {
        snprintf(lines[n++], 22, "Threshold  %d", t.threshold);
        snprintf(lines[n++], 22, "Direction  %s", t.rising ? "Rising" : "Falling");
    }
    if (t.track) snprintf(lines[n++], 22, "Sound      %u", t.track);
    else         snprintf(lines[n++], 22, "Sound      off");

    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < n; i++) {
        uint8_t r = i + scroll;
        row(i, _cursor == r, lines[r]);
    }
    scrollbar(n, VIS_ROWS, scroll);

    if      (_editing)     footer("Done", "Done", "Up/Dn adjust");
    else if (_cursor == 0) footer("Learn", "Back", "Hold: clear");
    else                    footer("Edit", "Back");
    disp.display();
}

// ── Special Functions ───────────────────────────────────────────────────────
void UIEngine::drawSFHome() {
    disp.clearDisplay();
    header("Special Functions");
    static const char* ITEMS[] = { "Timers", "Functions" };
    for (uint8_t i = 0; i < 2; i++) row(i, _cursor == i, ITEMS[i]);
    footer("Open", "Back");
    disp.display();
}

void UIEngine::hSFHome(const JoyEvent& e) {
    wrapCursor(_cursor, 2, e.up, e.down);
    if (e.ok) {
        uint8_t sel = _cursor;
        _cursor = 0;
        _page   = (sel == 0) ? Page::TIMER_LIST : Page::SF_LIST;
    }
    if (e.back) { _page = Page::CONTROL_MENU; _cursor = 1; }
}

// ── Timers ───────────────────────────────────────────────────────────────────
void UIEngine::drawTimerList() {
    disp.clearDisplay();
    header("Timers");
    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < MAX_TIMERS; i++) {
        uint8_t r = i + scroll;
        const TimerDef& td = _cfg->cfg.timers[r];
        char line[22];
        if (!td.enabled) {
            snprintf(line, sizeof(line), "%d: (empty)", r + 1);
        } else if (td.kind == TimerKind::COUNTDOWN) {
            snprintf(line, sizeof(line), "%d: Countdown %02u:%02u", r + 1, td.start_s / 60, td.start_s % 60);
        } else {
            snprintf(line, sizeof(line), "%d: Count-up", r + 1);
        }
        row(i, _cursor == r, line);
    }
    scrollbar(MAX_TIMERS, VIS_ROWS, scroll);
    bool empty = !_cfg->cfg.timers[_cursor].enabled;
    footer(empty ? "New" : "Edit", "Back");
    disp.display();
}

void UIEngine::hTimerList(const JoyEvent& e) {
    wrapCursor(_cursor, MAX_TIMERS, e.up, e.down);
    TimerDef& td = _cfg->cfg.timers[_cursor];
    if (e.ok) {
        _timer_sel = _cursor;
        if (!td.enabled) { td.enabled = true; _timer_rt[_cursor] = TimerRT{}; _cfg->markDirty(); }
        _cursor = 0;
        _page   = Page::TIMER_EDIT;
    }
    if (e.back) { _page = Page::SF_HOME; _cursor = 0; }
}

void UIEngine::drawTimerEdit() {
    TimerDef& td = _cfg->cfg.timers[_timer_sel];
    disp.clearDisplay();
    char title[16]; snprintf(title, sizeof(title), "Timer %d", _timer_sel + 1);
    header(title);

    char lines[4][22];
    uint8_t n = 0;
    snprintf(lines[n++], 22, "Kind       %s", td.kind == TimerKind::COUNTDOWN ? "Countdown" : "Count-up");
    snprintf(lines[n++], 22, "Minute call %s",
             _cfg->cfg.timer_minute_call[_timer_sel] ? "ON" : "OFF");
    if (td.kind == TimerKind::COUNTDOWN) {
        snprintf(lines[n++], 22, "Start      %02u:%02u", td.start_s / 60, td.start_s % 60);
    }
    snprintf(lines[n++], 22, "Delete this timer");

    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < n; i++) {
        uint8_t r = i + scroll;
        row(i, _cursor == r, lines[r]);
    }
    scrollbar(n, VIS_ROWS, scroll);

    if      (_editing)             footer("Done", "Done", "Up/Dn adjust");
    else if (_cursor == n - 1)     footer("Delete", "Back");
    else                            footer("Edit", "Back");
    disp.display();
}

void UIEngine::hTimerEdit(const JoyEvent& e) {
    TimerDef& td = _cfg->cfg.timers[_timer_sel];
    uint8_t   n  = (td.kind == TimerKind::COUNTDOWN) ? 4 : 3;
    uint8_t   start_row  = 2;   // COUNTDOWN only
    uint8_t   delete_row = n - 1;

    if (_editing) {
        if (_cursor == start_row && td.kind == TimerKind::COUNTDOWN) {
            if (e.up)   { td.start_s = (uint16_t)constrain((int)td.start_s + 5, 0, 5999); _cfg->markDirty(); }
            if (e.down) { td.start_s = (uint16_t)constrain((int)td.start_s - 5, 0, 5999); _cfg->markDirty(); }
            if ((e.up || e.down) && !_timer_rt[_timer_sel].running)
                _timer_rt[_timer_sel].value_s = td.start_s;
        }
        if (e.ok || e.back) _editing = false;
        return;
    }

    wrapCursor(_cursor, n, e.up, e.down);

    if (_cursor == 0) {
        if (e.ok) {
            td.kind = (td.kind == TimerKind::COUNTDOWN) ? TimerKind::COUNTUP : TimerKind::COUNTDOWN;
            _cfg->markDirty();
            _timer_rt[_timer_sel] = TimerRT{};
            if (td.kind == TimerKind::COUNTDOWN) _timer_rt[_timer_sel].value_s = td.start_s;
            if (_cursor >= ((td.kind == TimerKind::COUNTDOWN) ? 4 : 3)) _cursor = 0;
        }
    } else if (_cursor == 1) {
        if (e.ok) {
            _cfg->cfg.timer_minute_call[_timer_sel] = !_cfg->cfg.timer_minute_call[_timer_sel];
            _cfg->markDirty();
        }
    } else if (_cursor == start_row && td.kind == TimerKind::COUNTDOWN) {
        if (e.ok) _editing = true;
    } else if (_cursor == delete_row) {
        if (e.ok) {
            _yesno = 1;                    // default to No - a delete should be opt-in
            _page  = Page::TIMER_DELETE_CONFIRM;
            return;
        }
    }
    if (e.back) { _cursor = _timer_sel; _page = Page::TIMER_LIST; }
}

void UIEngine::drawTimerDeleteConfirm() {
    disp.clearDisplay();
    char h[20]; snprintf(h, sizeof(h), "Delete Timer %d?", _timer_sel + 1);
    header(h);
    disp.setCursor(0, 18); disp.print(F("Also stops it if"));
    disp.setCursor(0, 28); disp.print(F("running"));
    yesNo(_yesno);
    footer(_yesno == 0 ? "Hold: delete" : "Back: cancel", nullptr);
    disp.display();
}

void UIEngine::hTimerDeleteConfirm(const JoyEvent& e) {
    if (e.up || e.down) _yesno ^= 1;
    // Same rule as every other delete confirm: OK only ever acts on a long
    // press, whichever option is highlighted, so nothing is ever deleted by
    // a single accidental tap. BACK cancels immediately either way.
    if (e.ok_long) {
        if (_yesno == 0) {
            _cfg->cfg.timers[_timer_sel] = TimerDef{};
            _timer_rt[_timer_sel] = TimerRT{};
            _cfg->markDirty();
        }
        _cursor = _timer_sel;
        _page   = Page::TIMER_LIST;
    }
    if (e.back) { _cursor = 0; _page = Page::TIMER_EDIT; }
}

// ── Functions ────────────────────────────────────────────────────────────────
enum SFRowKind : uint8_t {
    SFR_TRIG, SFR_STATE, SFR_FUNC, SFR_CH, SFR_PCT, SFR_SOUND,
    SFR_TIMERIDX, SFR_SET_MMSS, SFR_VOL_MIN, SFR_VOL_MAX, SFR_DELETE,
    SFR_SRC,      // Source: Input (button/axis) or Channel
    SFR_TCH,      // trigger channel (Source = Channel)
    SFR_TDIR,     // fires Above / Below the level (Source = Channel)
    SFR_TLEVEL    // the level, percent (Source = Channel)
};

// Single source of truth for which rows a function row's edit screen has,
// in order - Trigger, Active/Inactive, Function, then whatever values that
// function needs. Both drawSFEdit() and hSFEdit() walk this so they can
// never disagree about what a given row number means.
//
// Volume is the exception to the Active/Inactive pattern: it doesn't fire on
// a transition, it continuously maps the trigger axis's position to a
// volume, so there's no "state" to pick - the State row is left out for it.
static uint8_t sfRows(const SpecialFunction& f, SFRowKind* out) {
    uint8_t n = 0;
    out[n++] = SFR_SRC;
    if (f.trig_type == SourceType::CHANNEL) {
        out[n++] = SFR_TCH;
        if (f.type != SFType::VOLUME) { out[n++] = SFR_TDIR; out[n++] = SFR_TLEVEL; }
    } else {
        out[n++] = SFR_TRIG;
    }
    if (f.type != SFType::VOLUME) out[n++] = SFR_STATE;
    out[n++] = SFR_FUNC;
    switch (f.type) {
        case SFType::OVERRIDE_CHANNEL: out[n++] = SFR_CH; out[n++] = SFR_PCT; break;
        case SFType::PLAY_SOUND:       out[n++] = SFR_SOUND; break;
        case SFType::TIMER_SET:        out[n++] = SFR_TIMERIDX; out[n++] = SFR_SET_MMSS; break;
        case SFType::TIMER_START:
        case SFType::TIMER_STOP:
        case SFType::TIMER_RESET:
        case SFType::TIMER_RUN:        out[n++] = SFR_TIMERIDX; break;
        case SFType::VOLUME:           out[n++] = SFR_VOL_MIN; out[n++] = SFR_VOL_MAX; break;
        default: break;   // NONE - nothing to configure until a function is picked
    }
    out[n++] = SFR_DELETE;
    return n;
}

// Order the functions are listed in the Function picker. Separate from the
// SFType numbering, which is saved in flash and so can never be reordered.
static const SFType SF_PICK_ORDER[] = {
    SFType::OVERRIDE_CHANNEL, SFType::PLAY_SOUND, SFType::TIMER_SET, SFType::TIMER_RUN,
    SFType::TIMER_START, SFType::TIMER_STOP, SFType::TIMER_RESET, SFType::VOLUME
};
static const uint8_t SF_PICK_N = sizeof(SF_PICK_ORDER) / sizeof(SF_PICK_ORDER[0]);

// Index of the Function row on a function's edit screen (it moves with the
// trigger source and function type).
static uint8_t sfFuncRow(const SpecialFunction& f) {
    SFRowKind rows[10];
    uint8_t n = sfRows(f, rows);
    for (uint8_t i = 0; i < n; i++) if (rows[i] == SFR_FUNC) return i;
    return 0;
}

void UIEngine::drawSFList() {
    disp.clearDisplay();
    header("Functions");
    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < MAX_SPECIAL_FUNCS; i++) {
        uint8_t r = i + scroll;
        const SpecialFunction& f = _cfg->cfg.sf[r];
        char line[22];
        if (f.type == SFType::NONE) {
            snprintf(line, sizeof(line), "%d: (empty)", r + 1);
        } else {
            char sl[6];
            if (f.trig_type == SourceType::NONE) snprintf(sl, sizeof(sl), "--");
            else { MixSource tmp; tmp.type = f.trig_type; tmp.index = f.trig_index; slotLabel(tmp, sl, sizeof(sl)); }
            snprintf(line, sizeof(line), "%s %s %s", sl, f.fire_active ? "on " : "off", sfTypeName(f.type));
        }
        row(i, _cursor == r, line);
    }
    scrollbar(MAX_SPECIAL_FUNCS, VIS_ROWS, scroll);
    bool empty = (_cfg->cfg.sf[_cursor].type == SFType::NONE);
    footer(empty ? "New" : "Edit", "Back");
    disp.display();
}

void UIEngine::hSFList(const JoyEvent& e) {
    wrapCursor(_cursor, MAX_SPECIAL_FUNCS, e.up, e.down);
    if (e.ok) {
        _sf_sel = _cursor;
        _cursor = 0;
        _page   = Page::SF_EDIT;
    }
    if (e.back) { _page = Page::SF_HOME; _cursor = 1; }
}

void UIEngine::drawSFEdit() {
    SpecialFunction& f = _cfg->cfg.sf[_sf_sel];
    disp.clearDisplay();
    char title[16]; snprintf(title, sizeof(title), "Func %d", _sf_sel + 1);
    header(title);

    SFRowKind rows[10];
    uint8_t   n      = sfRows(f, rows);
    uint8_t   scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;

    char sl[6];
    if (f.trig_type == SourceType::NONE) snprintf(sl, sizeof(sl), "--");
    else { MixSource tmp; tmp.type = f.trig_type; tmp.index = f.trig_index; slotLabel(tmp, sl, sizeof(sl)); }

    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < n; i++) {
        uint8_t ri = i + scroll;
        char line[22];
        switch (rows[ri]) {
            case SFR_SRC:   snprintf(line, sizeof(line), "Source     %s",
                                     f.trig_type == SourceType::CHANNEL ? "Channel" : "Input"); break;
            case SFR_TCH:   snprintf(line, sizeof(line), "Channel    CH%d", f.trig_index + 1); break;
            case SFR_TDIR:  snprintf(line, sizeof(line), "When       %s", f.trig_rising ? "Above" : "Below"); break;
            case SFR_TLEVEL:snprintf(line, sizeof(line), "Level      %d%%", f.trig_threshold); break;
            case SFR_TRIG:
                if (f.type == SFType::VOLUME && f.trig_type != SourceType::NONE && f.trig_type != SourceType::AXIS)
                    snprintf(line, sizeof(line), "Trigger    %s !axis", sl);
                else
                    snprintf(line, sizeof(line), "Trigger    %s", sl);
                break;
            case SFR_STATE: snprintf(line, sizeof(line), "State      %s", f.fire_active ? "Active" : "Inactive"); break;
            case SFR_FUNC:  snprintf(line, sizeof(line), "Function   %s", sfTypeName(f.type)); break;
            case SFR_CH:    snprintf(line, sizeof(line), "Channel    CH%d", f.override_ch + 1); break;
            case SFR_PCT:   snprintf(line, sizeof(line), "Value      %d%%", f.override_pct); break;
            case SFR_SOUND:
                if (f.sound_track) snprintf(line, sizeof(line), "Sound      %u", f.sound_track);
                else                snprintf(line, sizeof(line), "Sound      off");
                break;
            case SFR_TIMERIDX: snprintf(line, sizeof(line), "Timer      %d", f.timer_idx + 1); break;
            case SFR_SET_MMSS: snprintf(line, sizeof(line), "Set to     %02u:%02u",
                                         f.timer_set_s / 60, f.timer_set_s % 60); break;
            case SFR_VOL_MIN: snprintf(line, sizeof(line), "Min vol    %u", f.vol_min); break;
            case SFR_VOL_MAX: snprintf(line, sizeof(line), "Max vol    %u", f.vol_max); break;
            case SFR_DELETE: snprintf(line, sizeof(line), "Delete this row"); break;
        }
        row(i, _cursor == ri, line);
    }
    scrollbar(n, VIS_ROWS, scroll);

    SFRowKind cur = (_cursor < n) ? rows[_cursor] : SFR_DELETE;
    if      (_editing)          footer("Done", "Done", "Up/Dn adjust");
    else if (cur == SFR_TRIG)   footer("Learn", "Back", "Hold: clear");
    else if (cur == SFR_DELETE) footer("Delete", "Back");
    else if (cur == SFR_STATE || cur == SFR_SRC || cur == SFR_TDIR) footer("Toggle", "Back");
    else if (cur == SFR_FUNC)   footer("Select", "Back");
    else                         footer("Edit", "Back");
    disp.display();
}

void UIEngine::hSFEdit(const JoyEvent& e) {
    SpecialFunction& f = _cfg->cfg.sf[_sf_sel];
    SFRowKind rows[10];
    uint8_t   n = sfRows(f, rows);
    if (_cursor >= n) _cursor = n - 1;
    SFRowKind cur = rows[_cursor];

    if (_editing) {
        switch (cur) {
            case SFR_CH:
                if (e.up)   { f.override_ch = (uint8_t)((f.override_ch + 1) % CRSF_NUM_CHANNELS); _cfg->markDirty(); }
                if (e.down) { f.override_ch = (uint8_t)((f.override_ch + CRSF_NUM_CHANNELS - 1) % CRSF_NUM_CHANNELS); _cfg->markDirty(); }
                break;
            case SFR_TCH:
                if (e.up)   { f.trig_index = (uint8_t)((f.trig_index + 1) % CRSF_NUM_CHANNELS); _cfg->markDirty(); }
                if (e.down) { f.trig_index = (uint8_t)((f.trig_index + CRSF_NUM_CHANNELS - 1) % CRSF_NUM_CHANNELS); _cfg->markDirty(); }
                break;
            case SFR_TLEVEL:
                if (e.up)   { f.trig_threshold = (int16_t)constrain((int)f.trig_threshold + 5, -100, 100); _cfg->markDirty(); }
                if (e.down) { f.trig_threshold = (int16_t)constrain((int)f.trig_threshold - 5, -100, 100); _cfg->markDirty(); }
                break;
            case SFR_PCT:
                if (e.up)   { f.override_pct = (int8_t)constrain((int)f.override_pct + 5, -100, 100); _cfg->markDirty(); }
                if (e.down) { f.override_pct = (int8_t)constrain((int)f.override_pct - 5, -100, 100); _cfg->markDirty(); }
                break;
            case SFR_SOUND:
                if (e.up)   { f.sound_track = (uint16_t)constrain((int)f.sound_track + 1, 0, 999); _cfg->markDirty(); if (_voice && f.sound_track) _voice->play(f.sound_track); }
                if (e.down) { f.sound_track = (uint16_t)constrain((int)f.sound_track - 1, 0, 999); _cfg->markDirty(); if (_voice && f.sound_track) _voice->play(f.sound_track); }
                break;
            case SFR_TIMERIDX:
                if (e.up)   { f.timer_idx = (uint8_t)((f.timer_idx + 1) % MAX_TIMERS); _cfg->markDirty(); }
                if (e.down) { f.timer_idx = (uint8_t)((f.timer_idx + MAX_TIMERS - 1) % MAX_TIMERS); _cfg->markDirty(); }
                break;
            case SFR_SET_MMSS:
                if (e.up)   { f.timer_set_s = (uint16_t)constrain((int)f.timer_set_s + 5, 0, 5999); _cfg->markDirty(); }
                if (e.down) { f.timer_set_s = (uint16_t)constrain((int)f.timer_set_s - 5, 0, 5999); _cfg->markDirty(); }
                break;
            case SFR_VOL_MIN:
                if (e.up)   { f.vol_min = (uint8_t)constrain((int)f.vol_min + 1, 0, 30); _cfg->markDirty(); }
                if (e.down) { f.vol_min = (uint8_t)constrain((int)f.vol_min - 1, 0, 30); _cfg->markDirty(); }
                break;
            case SFR_VOL_MAX:
                if (e.up)   { f.vol_max = (uint8_t)constrain((int)f.vol_max + 1, 0, 30); _cfg->markDirty(); }
                if (e.down) { f.vol_max = (uint8_t)constrain((int)f.vol_max - 1, 0, 30); _cfg->markDirty(); }
                break;
            default: break;
        }
        if (e.ok || e.back) _editing = false;
        return;
    }

    wrapCursor(_cursor, n, e.up, e.down);
    cur = rows[_cursor];

    switch (cur) {
        case SFR_TRIG:
            if (e.ok) {
                _learn_what = LEARN_SF_TRIG;
                _teach.begin(_in);
                _page = Page::LEARN_WAIT;
                return;
            }
            if (e.ok_long) { f.trig_type = SourceType::NONE; f.trig_index = 0; _cfg->markDirty(); }
            break;
        case SFR_STATE:
            if (e.ok) { f.fire_active = !f.fire_active; _cfg->markDirty(); }
            break;
        case SFR_SRC:
            if (e.ok) {
                if (f.trig_type == SourceType::CHANNEL) {      // back to a learned button / axis
                    f.trig_type = SourceType::NONE; f.trig_index = 0; f.trig_threshold = 0; f.trig_rising = true;
                } else {                                       // watch a channel's output instead
                    f.trig_type = SourceType::CHANNEL; f.trig_index = 0; f.trig_threshold = 0; f.trig_rising = true;
                }
                _cfg->markDirty();
            }
            break;
        case SFR_TDIR:
            if (e.ok) { f.trig_rising = !f.trig_rising; _cfg->markDirty(); }
            break;
        case SFR_TCH: case SFR_TLEVEL:
            if (e.ok) _editing = true;
            break;
        case SFR_FUNC:
            if (e.ok) {
                // Opens a list panel instead of cycling one step per press:
                // UP/DOWN pick, OK sets it, BACK leaves the function
                // unchanged. NONE isn't offered - "Delete this row" is the
                // way back to an unconfigured row.
                _cursor = 0;
                for (uint8_t k = 0; k < SF_PICK_N; k++) if (SF_PICK_ORDER[k] == f.type) _cursor = k;
                _page   = Page::SF_FUNC_SELECT;
                return;
            }
            break;
        case SFR_CH: case SFR_PCT: case SFR_SOUND: case SFR_TIMERIDX: case SFR_SET_MMSS:
        case SFR_VOL_MIN: case SFR_VOL_MAX:
            if (e.ok) _editing = true;
            break;
        case SFR_DELETE:
            if (e.ok) {
                _yesno = 1;                    // default to No - a delete should be opt-in
                _page  = Page::SF_DELETE_CONFIRM;
                return;
            }
            break;
    }
    if (e.back) { _cursor = _sf_sel; _page = Page::SF_LIST; }
}

// A list panel for the Function row: UP/DOWN move the cursor, OK sets the
// function and returns to SF_EDIT, BACK returns without changing anything.
// NONE is left out - "Delete this row" on SF_EDIT is the way to clear a row.
void UIEngine::drawSFFuncSelect() {
    disp.clearDisplay();
    header("Function");
    const SpecialFunction& f = _cfg->cfg.sf[_sf_sel];
    const uint8_t N = SF_PICK_N;
    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < N; i++) {
        SFType t = SF_PICK_ORDER[i + scroll];
        char b[22];
        snprintf(b, sizeof(b), "%s%s", sfTypeName(t), t == f.type ? "  *" : "");
        row(i, _cursor == (i + scroll), b);
    }
    scrollbar(N, VIS_ROWS, scroll);
    footer("Select", "Back");
    disp.display();
}

void UIEngine::hSFFuncSelect(const JoyEvent& e) {
    const uint8_t N = SF_PICK_N;
    wrapCursor(_cursor, N, e.up, e.down);

    SpecialFunction& f = _cfg->cfg.sf[_sf_sel];
    if (e.ok) {
        f.type = SF_PICK_ORDER[_cursor];
        _cfg->markDirty();
        _cursor = sfFuncRow(f);   // land back on the Function row
        _page   = Page::SF_EDIT;
    }
    if (e.back) {
        _cursor = sfFuncRow(f);
        _page   = Page::SF_EDIT;
    }
}

void UIEngine::drawSFDeleteConfirm() {
    disp.clearDisplay();
    char h[20]; snprintf(h, sizeof(h), "Delete Func %d?", _sf_sel + 1);
    header(h);
    disp.setCursor(0, 18); disp.print(F("This row only"));
    yesNo(_yesno);
    footer(_yesno == 0 ? "Hold: delete" : "Back: cancel", nullptr);
    disp.display();
}

void UIEngine::hSFDeleteConfirm(const JoyEvent& e) {
    if (e.up || e.down) _yesno ^= 1;
    if (e.ok_long) {
        if (_yesno == 0) {
            _cfg->cfg.sf[_sf_sel] = SpecialFunction{};
            _cfg->markDirty();
        }
        _cursor = _sf_sel;
        _page   = Page::SF_LIST;
    }
    if (e.back) { _cursor = 0; _page = Page::SF_EDIT; }
}

// ── Profiles ────────────────────────────────────────────────────────────────
void UIEngine::drawProfiles() {
    disp.clearDisplay();
    header("Profiles");

    if (!_prof || !_prof->available()) {
        disp.setCursor(0, 16); disp.print(F("No filesystem."));
        disp.setCursor(0, 28); disp.print(F("Set Tools>Flash Size"));
        disp.setCursor(0, 38); disp.print(F("to one with an FS."));
        footer("", "Back");
        disp.display();
        return;
    }

    uint8_t total  = _pcount + 2;   // row 0 = save current device, row 1 = save as new slot
    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;

    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < total; i++) {
        uint8_t r = i + scroll;
        char line[22];
        if (r == 0) {
            if (_vid) snprintf(line, sizeof(line), "Save %04X:%04X", _vid, _pid);
            else      snprintf(line, sizeof(line), "No device");
        } else if (r == 1) {
            snprintf(line, sizeof(line), "Save as new slot");
        } else {
            const ProfileInfo& pi = _plist[r - 2];
            bool cur = _vid && pi.vid == _vid && pi.pid == _pid;
            snprintf(line, sizeof(line), "%s%s", cur ? "*" : " ", pi.name);
        }
        row(i, _cursor == r, line);
    }
    scrollbar(total, VIS_ROWS, scroll);
    footer("Select", "Back");
    disp.display();
}

// What to do with the profile picked from the list. RST simply goes back,
// so deleting is never a side effect of the back button.
void UIEngine::drawProfileAction() {
    disp.clearDisplay();
    header("Profile");
    if (_psel < _pcount) {
        disp.setCursor(3, 12); disp.print(_plist[_psel].name);
    }
    row(1, _cursor == 0, "Load into current");
    row(2, _cursor == 1, "Rename");
    row(3, _cursor == 2, "Delete profile");
    footer("Do", "Back");
    disp.display();
}

// Name editor: whole name in normal size with an underline under the
// character being edited, and that character enlarged below it.
void UIEngine::drawProfileRename() {
    disp.clearDisplay();
    header("Rename Profile");
    disp.setTextSize(1);
    disp.setCursor(4, 16);
    disp.print(_str_buf);
    disp.drawFastHLine(4 + _str_cursor * 6, 25, 5, SSD1306_WHITE);
    char c = _str_buf[_str_cursor] ? _str_buf[_str_cursor] : ' ';
    disp.setTextSize(3);
    disp.setCursor(52, 29);
    disp.print(c == ' ' ? '_' : c);
    disp.setTextSize(1);
    footer("Next", "Back", "Hold OK: save");
    disp.display();
}

void UIEngine::drawProfileDelete() {
    disp.clearDisplay();
    header("Delete Profile?");
    if (_psel < _pcount) {
        disp.setCursor(0, 18); disp.print(_plist[_psel].name);
    }
    yesNo(_yesno);
    footer(_yesno == 0 ? "Hold: delete" : "Back: cancel", nullptr);
    disp.display();
}

void UIEngine::drawChannelClear() {
    disp.clearDisplay();
    char h[20]; snprintf(h, sizeof(h), "Clear CH%d?", _sel_ch + 1);
    header(h);
    disp.setCursor(0, 18); disp.print(F("Set source to none"));
    yesNo(_yesno);
    footer(_yesno == 0 ? "Hold: clear" : "Back: cancel", nullptr);
    disp.display();
}

void UIEngine::drawElrsList() {
    disp.clearDisplay();

    uint8_t kids[16];
    uint8_t n = elrsChildren(_elrs_folder, kids, 16);

    const LiveParam* folder = _elrs_folder ? _crsf->liveByField(_elrs_folder) : nullptr;
    header(folder ? folder->name : "ELRS Setup");

    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < n; i++) {
        const LiveParam* p = _crsf->liveByField(kids[i + scroll]);
        char line[22];
        if (!p) {
            snprintf(line, sizeof(line), "?");
        } else switch (p->type) {
            case ParamType::P_FOLDER:
                snprintf(line, sizeof(line), "%-17s>", p->name);
                break;
            case ParamType::P_COMMAND:
                snprintf(line, sizeof(line), "%-14s run", p->name);
                break;
            case ParamType::P_TEXT_SELECTION: {
                char vbuf[13];
                optionAt(p->text, (uint8_t)p->value, vbuf, sizeof(vbuf));
                snprintf(line, sizeof(line), "%-8s %s", p->name, vbuf);
                break;
            }
            case ParamType::P_FLOAT: {
                char vbuf[13];
                formatFloat(p->value, p->decimals, vbuf, sizeof(vbuf));
                snprintf(line, sizeof(line), "%-8s %s", p->name, vbuf);
                break;
            }
            case ParamType::P_STRING:
            case ParamType::P_INFO:
                snprintf(line, sizeof(line), "%-8s %s", p->name, p->text);
                break;
            default:   // integer types
                snprintf(line, sizeof(line), "%-8s %ld", p->name, (long)p->value);
                break;
        }
        row(i, _cursor == (i + scroll), line);
    }
    scrollbar(n, VIS_ROWS, scroll);

    if (n == 0 && _crsf->haveBad()) {
        const uint8_t* B = _crsf->badSnap();
        char bd[24];
        disp.setTextColor(SSD1306_WHITE);
        snprintf(bd, sizeof(bd), "bad %02X %02X %02X", B[0], B[1], B[2]);
        disp.setCursor(0, 20); disp.print(bd);
        snprintf(bd, sizeof(bd), "crc calc %02X got %02X", _crsf->badCrcCalc(), _crsf->badCrcRecv());
        disp.setCursor(0, 30); disp.print(bd);
        snprintf(bd, sizeof(bd), "rxfr %lu bytes %lu", (unsigned long)_crsf->rxFrames(), (unsigned long)_crsf->rxBytes());
        disp.setCursor(0, 40); disp.print(bd);
    }
    if (_crsf->discoveryBusy()) {
        char buf[12];
        if (_crsf->discoveryTotal() == 0)
            snprintf(buf, sizeof(buf), "i%u k%02X b%lu", _crsf->diInfoCount(), _crsf->skipLast(), (unsigned long)_crsf->rxBad());
        else
            snprintf(buf, sizeof(buf), "%u/%u", _crsf->discoveryDone(), _crsf->discoveryTotal());
        footer(_crsf->deviceFound() ? "Open" : "No module", "Back", buf);
    } else if (n == 0) {
        {
            char dg[14];
            snprintf(dg, sizeof(dg), "i%u k%02X b%lu", _crsf->diInfoCount(), _crsf->skipLast(), (unsigned long)_crsf->rxBad());
            footer(_crsf->deviceFound() ? "Empty" : "No module", "Back", dg);
        }
    } else {
        footer("Open", "Back");
    }
    disp.display();
}

void UIEngine::drawElrsValue() {
    const LiveParam* p = _crsf->liveByField(_elrs_field);
    disp.clearDisplay();
    header(p ? p->name : "?");
    if (!p) { footer("", "Back"); disp.display(); return; }

    uint8_t count  = countOptions(p->text);
    uint8_t scroll = (_val >= VIS_ROWS) ? _val - VIS_ROWS + 1 : 0;
    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < count; i++) {
        char b[16];
        optionAt(p->text, i + scroll, b, sizeof(b));
        row(i, _val == (i + scroll), b);
    }
    scrollbar(count, VIS_ROWS, scroll);
    footer("Send", "Back");
    disp.display();
}

void UIEngine::drawElrsNumEdit() {
    const LiveParam* p = _crsf->liveByField(_elrs_field);
    disp.clearDisplay();
    header(p ? p->name : "?");
    if (!p) { footer("", "Back"); disp.display(); return; }

    char vbuf[16];
    if (p->type == ParamType::P_FLOAT) formatFloat(_num_val, p->decimals, vbuf, sizeof(vbuf));
    else                                snprintf(vbuf, sizeof(vbuf), "%ld", (long)_num_val);

    disp.setTextSize(2);
    disp.setCursor(8, 22);
    disp.print(vbuf);
    disp.setTextSize(1);
    if (p->text[0]) { disp.setCursor(8, 40); disp.print(p->text); }   // units, if the module sent any

    char minb[12], maxb[12], range[26];
    if (p->type == ParamType::P_FLOAT) {
        formatFloat(p->vmin, p->decimals, minb, sizeof(minb));
        formatFloat(p->vmax, p->decimals, maxb, sizeof(maxb));
    } else {
        snprintf(minb, sizeof(minb), "%ld", (long)p->vmin);
        snprintf(maxb, sizeof(maxb), "%ld", (long)p->vmax);
    }
    snprintf(range, sizeof(range), "%s..%s", minb, maxb);
    disp.setCursor(0, 50);
    disp.print(range);

    footer("Send", "Back", "Up/Dn adjust");
    disp.display();
}

void UIEngine::drawElrsStrEdit() {
    const LiveParam* p = _crsf->liveByField(_elrs_field);
    disp.clearDisplay();
    header(p ? p->name : "?");

    disp.setTextSize(2);
    disp.setCursor(4, 22);
    disp.print(_str_buf);
    disp.setTextSize(1);
    int16_t x = 4 + _str_cursor * 12;
    disp.drawFastHLine(x, 40, 10, SSD1306_WHITE);   // underline the char being edited

    footer("Next", "Back", "Hold OK: send");
    disp.display();
}

void UIEngine::drawElrsInfo() {
    const LiveParam* p = _crsf->liveByField(_elrs_field);
    disp.clearDisplay();
    header(p ? p->name : "?");
    disp.setCursor(0, 24);
    disp.print(p ? p->text : "");
    footer("", "Back");
    disp.display();
}

void UIEngine::drawElrsCommand() {
    const LiveParam* p = _crsf->liveByField(_elrs_field);
    disp.clearDisplay();
    header(p ? p->name : "?");
    if (!_cmd_running) {
        disp.setCursor(0, 18); disp.print(F("Send command to"));
        disp.setCursor(0, 28); disp.print(F("the ELRS module?"));
        yesNo(_yesno);
    } else {
        const char* label = "Running...";
        if (p) {
            switch (p->cmd_status) {
                case 0: label = "Ready";       break;
                case 1: label = "Starting...";  break;
                case 2: label = "In progress"; break;
                case 3: label = "Confirmed";   break;
                case 4: label = "Cancelled";   break;
                default: label = "Timed out";  break;
            }
        }
        disp.setCursor(0, 22); disp.print(label);
        if (p && p->text[0]) { disp.setCursor(0, 36); disp.print(p->text); }
        footer("", "Back");
    }
    disp.display();
}

void UIEngine::drawLinkSetup() {
    disp.clearDisplay();
    header("Output Setup");

    bool ppm = (_cfg->cfg.out_mode == OutputMode::PPM);
    const uint8_t N = ppm ? 5 : 3;
    uint8_t scroll = (_cursor >= VIS_ROWS) ? _cursor - VIS_ROWS + 1 : 0;
    uint8_t shown  = 0;
    char line[24];

    for (uint8_t i = 0; i < VIS_ROWS && (i + scroll) < N; i++) {
        uint8_t r = i + scroll;
        if (r == 0) {
            snprintf(line, sizeof(line), "Output    %s",
                     outputModeName(_cfg->cfg.out_mode));
        } else if (!ppm) {
            if (r == 1) {
                // Just the line polarity. The receive side is matched
                // automatically; the status line below reports how that is
                // going, so it does not need to clutter this row.
                snprintf(line, sizeof(line), "Inverted  %s",
                         _cfg->cfg.crsf_invert ? "YES" : "NO");
            } else {
                // One label covering how the module is actually wired.
                const char* w = _cfg->cfg.crsf_single_pin ? "1 pin GP0"
                              : _cfg->cfg.crsf_half_duplex ? "2 pin 1wire"
                                                           : "2 pin TX/RX";
                snprintf(line, sizeof(line), "Wiring  %s", w);
            }
        } else {
            switch (r) {
                case 1: snprintf(line, sizeof(line), "Channels  %d",
                                 _cfg->cfg.ppm.channels); break;
                case 2: snprintf(line, sizeof(line), "Frame     %dus",
                                 _cfg->cfg.ppm.frame_us); break;
                case 3: snprintf(line, sizeof(line), "Pulse     %dus",
                                 _cfg->cfg.ppm.pulse_us); break;
                default: snprintf(line, sizeof(line), "Polarity  %s",
                                 _cfg->cfg.ppm.invert ? "Neg" : "Pos"); break;
            }
        }
        row(i, _cursor == r, line);
        shown++;
    }
    scrollbar(N, VIS_ROWS, scroll);

    // Put the status line directly below the last row rather than at a fixed
    // height. With five rows (PPM) four are visible and the bottom one reaches
    // y=53, so anything pinned lower lands on top of it.
    int16_t sy = ROW_Y0 + shown * ROW_H;
    if (sy + 8 > FOOTER_Y) {          // no room left; the rows win
        footer("", "Back");
        disp.display();
        return;
    }

    disp.setCursor(0, sy);
    if (ppm) {
        disp.print(F("GP0 -> trainer port"));
    } else if (_crsf->deviceFound()) {
        char b[24];
        // Bound the module name: it can be up to 23 characters, which would
        // push "sync" off the end of the line.
        char nm[12];
        snprintf(nm, sizeof(nm), "%s",
                 _crsf->deviceName()[0] ? _crsf->deviceName() : "module");
        snprintf(b, sizeof(b), "OK %s%s", nm, _crsf->synced() ? " sync" : "");
        disp.print(b);
    } else {
        // The last frame that failed, held on screen: address, length, type,
        // then the CRC we calculated against the one that arrived.
        char b[26];
        if (!_crsf->echoVerified()) {
            // Still proving the receive path with our own transmission.
            snprintf(b, sizeof(b), "rx%lu %02X testing",
                     (unsigned long)_crsf->rxBytes(), _crsf->rxLastByte());
            disp.print(b);
        } else if (_crsf->haveBad()) {
            const uint8_t* B = _crsf->badSnap();
            snprintf(b, sizeof(b), "%02X %02X %02X c%02X/%02X",
                     B[0], B[1], B[2],
                     _crsf->badCrcCalc(), _crsf->badCrcRecv());
            disp.print(b);          // was filled but never drawn
        } else {
            // Our own frames read back fine, so the receiver is proven. Any
            // silence now is the module not answering, not our problem.
            snprintf(b, sizeof(b), "RX OK - no reply");
            disp.print(b);
        }
    }
    if (_editing)                     footer("Done", "Done", "Up/Dn adjust");
    else if (_cursor == 0)            footer("Switch", "Back");
    else if (!ppm && _cursor == 1)    footer("Toggle", "Back");
    else if (!ppm && _cursor == 2)    footer("Select", "Back");
    else if (ppm && _cursor == 4)     footer("Toggle", "Back");
    else if (ppm)                     footer("Edit", "Back");
    else                               footer("", "Back");
    disp.display();
}

void UIEngine::drawSaveConfirm() {
    disp.clearDisplay();
    header("Save Config?");
    if (_cfg->usingFilesystem()) {
        disp.setCursor(0, 16); disp.print(F("Write to /settings"));
        disp.setCursor(0, 26); disp.print(F("Survives reflashing"));
    } else {
        // No FS partition: settings go to EEPROM, which a firmware update or
        // a change of Flash Size can erase.
        disp.setCursor(0, 16); disp.print(F("No filesystem!"));
        disp.setCursor(0, 26); disp.print(F("EEPROM only - may be"));
        disp.setCursor(0, 34); disp.print(F("lost on reflash"));
    }
    yesNo(_yesno);
    disp.display();
}

void UIEngine::drawDiagContent(uint8_t page) {
    // ── Page 0: USB / input diagnostics ─────────────────────────────────────
    // Six lines on a 9 px grid: 11, 20, 29, 38, 47, 56. No footer, so the
    // bottom line can use the last row without clashing.
    if (page == 0) {
        header("USB Diagnostics");
        char b[26];

        snprintf(b, sizeof(b), "mnt%u hid%u p%u/%u",
                 g_diag.mounts, g_diag.hid_mounts,
                 (unsigned)(g_diag.hid_itf), (unsigned)(g_diag.cdc_mounts));
        disp.setCursor(0, 11); disp.print(b);

        if (g_diag.mounts == 0) {
            disp.setCursor(0, 22); disp.print(F("No USB device seen."));
            disp.setCursor(0, 33); disp.print(F("Tools>USB Stack must"));
            disp.setCursor(0, 44); disp.print(F("be TinyUSB Host"));
        } else {
            snprintf(b, sizeof(b), "VID%04X PID%04X",
                     g_diag.last_vid, g_diag.last_pid);
            disp.setCursor(0, 20); disp.print(b);

            snprintf(b, sizeof(b), "d%u ax%u bt%u %s",
                     g_diag.desc_len, g_diag.map_axes, g_diag.map_btns,
                     g_diag.ps_type ? psName((PSType)g_diag.ps_type)
                                    : (g_diag.raw_mode ? "RAW" : ""));
            disp.setCursor(0, 29); disp.print(b);

            snprintf(b, sizeof(b), "rpt%lu len%u",
                     (unsigned long)g_diag.reports, g_diag.last_rpt_len);
            disp.setCursor(0, 38); disp.print(b);

            snprintf(b, sizeof(b), "xi%lu a%lu/%lu %c%c",
                     (unsigned long)g_diag.xinput_rpts,
                     (unsigned long)g_diag.arm_ok,
                     (unsigned long)g_diag.arm_fail,
                     g_diag.xinput_open ? 'I' : '-',
                     g_diag.xinput_out  ? 'O' : '-');
            disp.setCursor(0, 47); disp.print(b);

            // Age of the newest report: the number that matters when input
            // freezes. It should reset to 0 on any stick movement.
            snprintf(b, sizeof(b), "age%lu abt%lu rec%lu",
                     (unsigned long)g_diag.report_age_ms,
                     (unsigned long)g_diag.aborts,
                     (unsigned long)g_diag.hid_stuck);
            disp.setCursor(0, 56); disp.print(b);
        }
        return;
    }

    // ── Page 1: raw report bytes ────────────────────────────────────────────
    // If a device isn't understood, this shows exactly what it sends so the
    // layout can be worked out by hand.
    if (page == 1) {
        header("Raw HID Report");
        char b[26];

        snprintf(b, sizeof(b), "len:%u ax:%u bt:%u",
                 g_diag.raw_len, g_diag.map_axes, g_diag.map_btns);
        disp.setCursor(0, 12); disp.print(b);

        snprintf(b, sizeof(b), "ok%lu bad%lu %s",
                 (unsigned long)g_diag.decode_ok,
                 (unsigned long)g_diag.decode_fail,
                 g_diag.raw_mode ? "RAW" : "desc");
        disp.setCursor(0, 44); disp.print(b);

        if (g_diag.raw_len == 0) {
            disp.setCursor(0, 26); disp.print(F("No report received"));
        } else {
            // 8 bytes per line, two lines
            for (uint8_t line = 0; line < 2; line++) {
                char hexbuf[26]; uint8_t pos = 0;
                for (uint8_t i = 0; i < 8; i++) {
                    uint8_t idx = line * 8 + i;
                    if (idx >= g_diag.raw_len) break;
                    pos += snprintf(hexbuf + pos, sizeof(hexbuf) - pos,
                                    "%02X ", g_diag.raw[idx]);
                }
                hexbuf[pos] = 0;
                disp.setCursor(0, 24 + line * 10); disp.print(hexbuf);
            }
        }
        return;
    }

    // ── Pages 2+: live axis values ──────────────────────────────────────────
    char h[24];
    snprintf(h, sizeof(h), "%s A%d B%d",
             _in.connected ? inputSourceName(_in.source) : "No input",
             _in.num_axes, _in.num_buttons);
    header(h);

    // Track how far each axis has moved since this page was opened. A '*'
    // marks any axis that has actually changed, which answers the only
    // question that matters here: is this axis alive at all?
    static int16_t seen_min[MAX_INPUT_AXES], seen_max[MAX_INPUT_AXES];
    static bool    seen_init = false;
    if (!seen_init) {
        for (uint8_t i = 0; i < MAX_INPUT_AXES; i++) {
            seen_min[i] =  32767;
            seen_max[i] = -32768;
        }
        seen_init = true;
    }
    for (uint8_t i = 0; i < _in.num_axes && i < MAX_INPUT_AXES; i++) {
        if (_in.axes[i] < seen_min[i]) seen_min[i] = _in.axes[i];
        if (_in.axes[i] > seen_max[i]) seen_max[i] = _in.axes[i];
    }

    uint8_t base = (page - 2) * 4;
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t a = base + i;
        if (a >= _in.num_axes) break;
        int16_t y = ROW_Y0 + i * ROW_H;
        bool moved = ((int32_t)seen_max[a] - seen_min[a]) > 2000;

        char lbl[8]; snprintf(lbl, sizeof(lbl), "A%-2d%c", a + 1, moved ? '*' : ' ');
        disp.setCursor(0, y); disp.print(lbl);
        float v = ((float)_in.axes[a] + 32768.0f) / 65535.0f;
        bar(28, y - 1, 62, 8, v, SSD1306_WHITE);
        disp.setCursor(94, y);
        disp.print((int)(_in.axes[a] / 328));
    }

    disp.setCursor(0, 46);
    for (uint8_t b2 = 0; b2 < 16 && b2 < _in.num_buttons; b2++)
        disp.print(((_in.buttons >> b2) & 1u) ? '1' : '.');
}

void UIEngine::drawInputInfo() {
    disp.clearDisplay();
    drawDiagContent(_cursor);
    footer("", "Back");
    disp.display();
}

// ═══════════════════════════════════════════════════════════════════════════════
//  HANDLERS
// ═══════════════════════════════════════════════════════════════════════════════

void UIEngine::hStatus(const JoyEvent& e) {
    if (e.ok) { _page = Page::MAIN_MENU; _cursor = 0; }

    // Long-press BACK jumps straight to the profile list - a quick way to
    // swap setups without going through the Main Menu first. Short BACK
    // still does nothing on this screen; every view here is already
    // reachable with UP/DOWN, and a timer's start/stop/reset only ever
    // happens through whatever switch a Special Function row was set up
    // with, not a button on this screen.
    if (e.back_long) {
        if (_prof && _prof->available()) {
            refreshProfiles();
            _cursor = (_pcount > 0) ? 2 : 0;   // straight to the list, past the Save rows
            _page   = Page::PROFILES;
        }
        return;
    }

    uint8_t tslots[MAX_TIMERS];
    uint8_t nt    = timerSlots(tslots, MAX_TIMERS);
    uint8_t nd    = diagPageCount();
    uint8_t total = 4 + nt + nd;   // overview + 2 bar pages + ELRS stats + timers + diagnostics
    if (_status_view >= total) _status_view = 0;

    // UP/DOWN cycle the whole carousel: the overview, CH1-8, CH9-16, ELRS
    // stats, each timer, then each diagnostics page - the same screens
    // Menu > Input Info shows, just reachable directly without leaving Status.
    if (e.up)   _status_view = (uint8_t)((_status_view + 1) % total);
    if (e.down) _status_view = (uint8_t)((_status_view + total - 1) % total);
}

void UIEngine::hMainMenu(const JoyEvent& e) {
    wrapCursor(_cursor, MAIN_N, e.up, e.down);
    if (e.ok) {
        switch (_cursor) {
            case 0: refreshProfiles();
                    _page = Page::PROFILES;      _cursor = 0; break;
            case 1: _page = Page::CONTROL_MENU;  _cursor = 0; break;
            case 2: _page = Page::ELRS_LIST;     _cursor = 0; break;
            case 3: _page = Page::SOUND_LIST;    _cursor = 0; break;
            case 4: _page = Page::SAFETY;        _cursor = 0; break;
            case 5: _page = Page::SAVE_CONFIRM;  _yesno  = 0; break;
        }
    }
    if (e.back) { _page = Page::STATUS; _cursor = 0; }
}

void UIEngine::hModeSelect(const JoyEvent& e) {
    const uint8_t N = (uint8_t)DeviceMode::_COUNT;
    wrapCursor(_cursor, N, e.up, e.down);
    if (e.ok) { _yesno = 0; _page = Page::MODE_CONFIRM; }
    if (e.back) { _page = Page::SAFETY; _cursor = 6; }
}

void UIEngine::hModeConfirm(const JoyEvent& e) {
    if (e.up || e.down) _yesno ^= 1;
    if (e.ok) {
        if (_yesno == 0) {
            _cfg->setDefaults((DeviceMode)_cursor);
            _cfg->save();          // immediate: a reboot follows
            disp.clearDisplay();
            disp.setTextSize(2); disp.setCursor(14, 24); disp.print(F("Reboot"));
            disp.setTextSize(1); disp.display();
            disp.finish();
            delay(700);
            rp2040.reboot();
        }
        _page = Page::MODE_SELECT;
    }
    if (e.back) _page = Page::MODE_SELECT;
}

void UIEngine::hChannelList(const JoyEvent& e) {
    // 16 channels plus one action row at the end.
    wrapCursor(_cursor, CRSF_NUM_CHANNELS + 1, e.up, e.down);

    if (e.ok) {
        if (_cursor >= CRSF_NUM_CHANNELS) {
            _yesno = 1;
            _page  = Page::FS_CAPTURE_ALL;
        } else {
            _sel_ch = _cursor;
            _cursor = 0;
            _page   = Page::CHANNEL_EDIT;
        }
    }
    if (e.back) { _page = Page::CONTROL_MENU; _cursor = 0; }
}

void UIEngine::hChannelEdit(const JoyEvent& e) {
    ChannelConfig& c = _cfg->cfg.channels[_sel_ch];

    // Rows 5 (expo, when not using the curve) and 6 (weight) are ranged
    // values: OK opens them for adjustment, UP/DOWN then step the value,
    // OK or BACK closes the row again.
    if (_editing) {
        if (_cursor == 5) {
            if (e.up)   { c.expo = (int8_t)constrain((int16_t)c.expo + 5, -100, 100); _cfg->markDirty(); }
            if (e.down) { c.expo = (int8_t)constrain((int16_t)c.expo - 5, -100, 100); _cfg->markDirty(); }
        } else if (_cursor == 6) {
            if (e.up)   { c.weight = (int8_t)constrain((int16_t)c.weight + 5, -125, 125); _cfg->markDirty(); }
            if (e.down) { c.weight = (int8_t)constrain((int16_t)c.weight - 5, -125, 125); _cfg->markDirty(); }
        }
        if (e.ok || e.back) _editing = false;
        return;
    }

    wrapCursor(_cursor, CE_ROWS, e.up, e.down);

    switch (_cursor) {
        case 0:                                  // resting position: cycle
            if (e.ok) {
                int8_t v = (int8_t)c.base + 1;
                if (v > 2) v = 0;
                c.base = (MixTarget)v;
                _cfg->markDirty();
            }
            break;

        case 1: case 2: case 3:                  // mix slots
            if (e.ok) {
                _slot   = _cursor - 1;
                _cursor = 0;
                _page   = Page::SLOT_EDIT;
                return;
            }
            break;

        case 4:                                  // expo or curve
            if (e.ok) { c.curve_on = !c.curve_on; _cfg->markDirty(); }
            break;

        case 5:
            if (c.curve_on) {                    // open the curve editor
                if (e.ok) {
                    _cpt  = 0;
                    _page = Page::CURVE_EDIT;
                    return;
                }
            } else if (e.ok) {                   // open expo for adjustment
                _editing = true;
            }
            break;

        case 6:                                  // channel weight / travel
            if (e.ok) _editing = true;
            break;

        case 7:                                  // trim: full submenu
            if (e.ok) {
                _cursor = 0;
                _page   = Page::TRIM_EDIT;
                return;
            }
            break;

        case 8:                                  // failsafe
            if (e.ok) {
                _cursor = 0;
                _page   = Page::FS_EDIT;
                return;
            }
            break;

        case 9:                                  // clear this channel
            if (e.ok) {
                _yesno = 1;
                _page  = Page::CHANNEL_CLEAR;
                return;
            }
            break;
    }

    if (e.back) { _cursor = _sel_ch; _page = Page::CHANNEL_LIST; }
}

void UIEngine::hSlotEdit(const JoyEvent& e) {
    MixSource& m = _cfg->cfg.channels[_sel_ch].src[_slot];
    uint8_t rows = (m.type == SourceType::BUTTON) ? 5 : 4;

    // Row 1 (weight) is a ranged value: OK opens it, UP/DOWN step it.
    if (_editing && _cursor == 1) {
        int16_t w = m.weight;
        if (e.up)   w += 5;
        if (e.down) w -= 5;
        if (e.up || e.down) { m.weight = (int8_t)constrain(w, -100, 100); _cfg->markDirty(); }
        if (e.ok || e.back) _editing = false;
        return;
    }

    wrapCursor(_cursor, rows, e.up, e.down);

    switch (_cursor) {
        case 0:   // Source: short OK starts learning, long OK clears
            if (e.ok) {
                _learn_what = LEARN_SLOT;
                _teach.begin(_in);
                _page = Page::LEARN_WAIT;
                return;
            }
            if (e.ok_long) { m = MixSource{}; m.type = SourceType::NONE; _cfg->markDirty(); }
            break;

        case 1:   // Weight: open for adjustment
            if (e.ok) _editing = true;
            break;

        case 2:   // Reverse: just flips the weight's sign (+100 <-> -100, -60 <-> +60)
            if (e.ok) { m.weight = (int8_t)-m.weight; _cfg->markDirty(); }
            break;

        case 3:
            if (m.type == SourceType::BUTTON) {
                if (e.ok) {                       // cycle the push target
                    int8_t v = (int8_t)m.target + 1;
                    if (v > 2) v = 0;
                    m.target = (MixTarget)v;
                    _cfg->markDirty();
                }
            } else if (e.ok) {                    // "Clear slot"
                m = MixSource{}; m.type = SourceType::NONE;
                _cfg->markDirty();
                _cursor = _slot + 1;
                _page   = Page::CHANNEL_EDIT;
                return;
            }
            break;

        case 4:
            if (e.ok) {
                m.btn_mode = (m.btn_mode == ButtonMode::STICKY)
                                ? ButtonMode::MOMENTARY : ButtonMode::STICKY;
                _cfg->markDirty();
            }
            break;
    }

    if (e.back) { _cursor = _slot + 1; _page = Page::CHANNEL_EDIT; }
}

void UIEngine::hLearnWait(const JoyEvent& e) {
    if (e.back) {
        switch (_learn_what) {
            case LEARN_SLOT:       _cursor = 0; _page = Page::SLOT_EDIT; break;
            case LEARN_SOUND_TRIG: _cursor = 0; _page = Page::SOUND_TRIG_EDIT; break;
            case LEARN_SF_TRIG:    _cursor = 1; _page = Page::SF_EDIT; break;
            default:                _cursor = (uint8_t)_learn_what; _page = Page::TRIM_EDIT; break;
        }
        return;
    }
    if (!_teach.update(_in)) return;

    // Trim steps come from a button press, so ignore an axis here rather than
    // binding a stick to something it cannot drive.
    if (_learn_what == LEARN_TRIM_UP || _learn_what == LEARN_TRIM_DN) {
        if (_teach.result().type != SourceType::BUTTON) { _teach.begin(_in); return; }
        uint8_t idx = _teach.result().index;
        if (_learn_what == LEARN_TRIM_UP) _cfg->cfg.channels[_sel_ch].trim_up = idx;
        else                              _cfg->cfg.channels[_sel_ch].trim_dn = idx;
        _cfg->markDirty();
        _cursor = (_learn_what == LEARN_TRIM_UP) ? 1 : 2;
        _page   = Page::TRIM_EDIT;
        return;
    }

    if (_learn_what == LEARN_SOUND_TRIG) {
        const TeachResult& r = _teach.result();
        SoundTrigger& t = _cfg->cfg.sound.triggers[_sound_trig_sel];
        t.type  = r.type;
        t.index = r.index;
        if (r.type == SourceType::AXIS) {
            // Starting guess: trigger a little short of whichever extreme the
            // stick was moved towards during learning - Threshold/Direction
            // on the trigger screen fine-tune it from there.
            if (r.seen_max > -(int32_t)r.seen_min) {
                t.rising    = true;
                t.threshold = (int16_t)constrain((int32_t)r.seen_max - 4000, -32000, 32000);
            } else {
                t.rising    = false;
                t.threshold = (int16_t)constrain((int32_t)r.seen_min + 4000, -32000, 32000);
            }
        }
        _cfg->markDirty();
        _cursor = 0;
        _page   = Page::SOUND_TRIG_EDIT;
        return;
    }

    if (_learn_what == LEARN_SF_TRIG) {
        const TeachResult&  r = _teach.result();
        SpecialFunction& f = _cfg->cfg.sf[_sf_sel];
        f.trig_type  = r.type;
        f.trig_index = r.index;
        if (r.type == SourceType::AXIS) {
            if (r.seen_max > -(int32_t)r.seen_min) {
                f.trig_rising    = true;
                f.trig_threshold = (int16_t)constrain((int32_t)r.seen_max - 4000, -32000, 32000);
            } else {
                f.trig_rising    = false;
                f.trig_threshold = (int16_t)constrain((int32_t)r.seen_min + 4000, -32000, 32000);
            }
        }
        _cfg->markDirty();
        _cursor = 1;   // the Trigger row (Source is row 0)
        _page   = Page::SF_EDIT;
        return;
    }

    MixSource& m = _cfg->cfg.channels[_sel_ch].src[_slot];
    const TeachResult& r = _teach.result();
    m.type  = r.type;
    m.index = r.index;
    if (r.type == SourceType::BUTTON) {
        // Sensible starting point: push the channel to the opposite end
        // of wherever the channel rests.
        MixTarget base = _cfg->cfg.channels[_sel_ch].base;
        m.target   = (base == MixTarget::HIGH) ? MixTarget::LOW : MixTarget::HIGH;
        m.btn_mode = ButtonMode::MOMENTARY;
        m.weight   = 100;
    } else {
        m.weight   = 100;
    }
    _cfg->markDirty();
    _saved_at = millis();
    _page     = Page::LEARN_SAVED;
}

void UIEngine::hChannelClear(const JoyEvent& e) {
    if (e.up || e.down) _yesno ^= 1;
    if (e.ok_long) {
        if (_yesno == 0) { _cfg->cfg.channels[_sel_ch] = ChannelConfig{}; _cfg->markDirty(); }
        _cursor = 9;                       // back to the Clear row
        _page   = Page::CHANNEL_EDIT;
    }
    if (e.back) { _cursor = 9; _page = Page::CHANNEL_EDIT; }
}

void UIEngine::hFsEdit(const JoyEvent& e) {
    ChannelConfig& c = _cfg->cfg.channels[_sel_ch];

    if (_editing && _cursor == 1 && c.fs_mode == FailsafeMode::VALUE) {
        if (e.up)   { c.fs_value = (int8_t)constrain((int16_t)c.fs_value + 5, -100, 100); _cfg->markDirty(); }
        if (e.down) { c.fs_value = (int8_t)constrain((int16_t)c.fs_value - 5, -100, 100); _cfg->markDirty(); }
        if (e.ok || e.back) _editing = false;
        return;
    }

    wrapCursor(_cursor, 3, e.up, e.down);

    switch (_cursor) {
        case 0:
            if (e.ok) {
                c.fs_mode = (c.fs_mode == FailsafeMode::HOLD)
                                ? FailsafeMode::VALUE : FailsafeMode::HOLD;
                _cfg->markDirty();
            }
            break;
        case 1:
            if (e.ok && c.fs_mode == FailsafeMode::VALUE) _editing = true;
            break;
        case 2:
            if (e.ok) {
                captureFailsafe(false);          // this channel only
                _cursor = 8;
                _page   = Page::CHANNEL_EDIT;
                return;
            }
            break;
    }
    if (e.back) { _cursor = 8; _page = Page::CHANNEL_EDIT; }
}

void UIEngine::hFsCaptureAll(const JoyEvent& e) {
    if (e.up || e.down) _yesno ^= 1;
    if (e.ok) {
        if (_yesno == 0) captureFailsafe(true);
        _page = Page::CHANNEL_LIST;
    }
    if (e.back) _page = Page::CHANNEL_LIST;
}

void UIEngine::hTrimEdit(const JoyEvent& e) {
    ChannelConfig& c = _cfg->cfg.channels[_sel_ch];

    if (_editing) {
        if (_cursor == 0) {
            if (e.up)   { c.trim = (int8_t)constrain((int16_t)c.trim + 1, -100, 100); _cfg->markDirty(); }
            if (e.down) { c.trim = (int8_t)constrain((int16_t)c.trim - 1, -100, 100); _cfg->markDirty(); }
        } else if (_cursor == 3) {
            if (e.up)   { c.trim_step = (uint8_t)constrain((int16_t)c.trim_step + 1, 1, 25); _cfg->markDirty(); }
            if (e.down) { c.trim_step = (uint8_t)constrain((int16_t)c.trim_step - 1, 1, 25); _cfg->markDirty(); }
        }
        if (e.ok || e.back) _editing = false;
        return;
    }

    wrapCursor(_cursor, 6, e.up, e.down);

    switch (_cursor) {
        case 0:
        case 3:
            if (e.ok) _editing = true;
            break;

        case 4:   // trim max/min/centre sounds for this channel
            if (e.ok) { _cfg->cfg.trim_sound_off ^= (uint16_t)(1u << _sel_ch); _cfg->markDirty(); }
            break;

        case 1: case 2:
            if (e.ok) {                          // short OK: bind a button
                _learn_what = (_cursor == 1) ? LEARN_TRIM_UP : LEARN_TRIM_DN;
                _teach.begin(_in);
                _page = Page::LEARN_WAIT;
                return;
            }
            if (e.ok_long) {                     // long OK: unbind it
                if (_cursor == 1) c.trim_up = TRIM_BTN_NONE;
                else              c.trim_dn = TRIM_BTN_NONE;
                _cfg->markDirty();
            }
            break;

        default:
            if (e.ok) { c.trim = 0; _cfg->markDirty(); }
            break;
    }
    if (e.back) { _cursor = 7; _page = Page::CHANNEL_EDIT; }
}

void UIEngine::hCurveEdit(const JoyEvent& e) {
    ChannelConfig& c = _cfg->cfg.channels[_sel_ch];

    if (_editing) {
        if (e.up)   { c.curve[_cpt] = (int8_t)constrain((int16_t)c.curve[_cpt] + 5, -100, 100); _cfg->markDirty(); }
        if (e.down) { c.curve[_cpt] = (int8_t)constrain((int16_t)c.curve[_cpt] - 5, -100, 100); _cfg->markDirty(); }
        if (e.ok || e.back) _editing = false;
        return;
    }

    wrapCursor(_cpt, 5, e.up, e.down);

    if (e.ok) _editing = true;                  // open the selected point

    // Long-press OK resets the whole curve back to a straight line
    if (e.ok_long) {
        const int8_t lin[5] = { -100, -50, 0, 50, 100 };
        memcpy(c.curve, lin, sizeof(lin));
        _cfg->markDirty();
    }

    if (e.back) { _cursor = 5; _page = Page::CHANNEL_EDIT; }
}

// ── Calibration ─────────────────────────────────────────────────────────────
void UIEngine::hCalMenu(const JoyEvent& e) {
    if (_editing) {
        int16_t d = (int16_t)_cfg->cfg.cal[0].deadband;
        if (e.up)   d++;
        if (e.down) d--;
        if (e.up || e.down) {
            d = constrain(d, 0, 25);
            for (uint8_t i = 0; i < MAX_INPUT_AXES; i++)
                _cfg->cfg.cal[i].deadband = (uint8_t)d;
            _cfg->markDirty();
        }
        if (e.ok || e.back) _editing = false;
        return;
    }

    wrapCursor(_cursor, 3, e.up, e.down);

    if (_cursor == 0 && e.ok) {
        // Start the sweep with impossible extremes so the first sample wins.
        for (uint8_t i = 0; i < MAX_INPUT_AXES; i++) {
            _cmin[i] =  32767;
            _cmax[i] = -32768;
        }
        _input->setCalBypass(true);      // observe raw travel
        _page = Page::CAL_SWEEP;
        return;
    }
    if (_cursor == 1 && e.ok) _editing = true;
    if (_cursor == 2 && e.ok) {
        for (uint8_t i = 0; i < MAX_INPUT_AXES; i++) _cfg->cfg.cal[i] = AxisCal{};
        _cfg->markDirty();
    }
    if (e.back) { _page = Page::CONTROL_MENU; _cursor = 2; }
}

void UIEngine::hCalSweep(const JoyEvent& e) {
    // Track the widest excursion seen on every axis
    for (uint8_t i = 0; i < _in.num_axes && i < MAX_INPUT_AXES; i++) {
        if (_in.axes[i] < _cmin[i]) _cmin[i] = _in.axes[i];
        if (_in.axes[i] > _cmax[i]) _cmax[i] = _in.axes[i];
    }
    if (e.ok) { _page = Page::CAL_CENTER; return; }
    if (e.back) { _input->setCalBypass(false); _page = Page::CAL_MENU; _cursor = 0; }
}

void UIEngine::hCalCenter(const JoyEvent& e) {
    if (e.ok) {
        uint8_t db = _cfg->cfg.cal[0].deadband;
        for (uint8_t i = 0; i < MAX_INPUT_AXES; i++) {
            AxisCal& c = _cfg->cfg.cal[i];
            if (i < _in.num_axes && (int32_t)_cmax[i] - _cmin[i] > 8000) {
                c.min    = _cmin[i];
                c.max    = _cmax[i];
                c.center = _in.axes[i];          // whatever it rests at now
            } else {
                c = AxisCal{};                    // untouched axis: leave linear
            }
            c.deadband = db;
        }
        _cfg->markDirty();
        _input->setCalBypass(false);
        _page   = Page::CAL_MENU;
        _cursor = 0;
        return;
    }
    if (e.back) { _input->setCalBypass(false); _page = Page::CAL_MENU; _cursor = 0; }
}

// ── Safety ──────────────────────────────────────────────────────────────────
void UIEngine::hSafety(const JoyEvent& e) {
    if (_editing) {
        if (_cursor == 2) {
            int16_t v = _cfg->cfg.model_id;
            if (e.up)   v++;
            if (e.down) v--;
            if (e.up || e.down) {
                _cfg->cfg.model_id = (uint8_t)constrain((int)v, 0, 255);
                _cfg->markDirty();
                _crsf->sendModelId(_cfg->cfg.model_id);   // applied live
            }
        } else if (_cursor == 4) {
            int16_t v = _cfg->cfg.inactivity.minutes;
            if (e.up)   v++;
            if (e.down) v--;
            if (e.up || e.down) {
                _cfg->cfg.inactivity.minutes = (uint8_t)constrain((int)v, 1, 99);
                _cfg->markDirty();
            }
        }
        if (e.ok || e.back) _editing = false;
        return;
    }

    wrapCursor(_cursor, 10, e.up, e.down);

    if (e.ok) {
        switch (_cursor) {
            case 0: _page = Page::SAFETY_SUB; _cursor = 0; break;
            case 1: _page = Page::LINK_SETUP;  _cursor = 0; break;
            case 2: _editing = true; break;
            case 3:
                // Inactivity alarm - see checkInactivity() in the main update loop.
                _cfg->cfg.inactivity.enabled = !_cfg->cfg.inactivity.enabled;
                _cfg->markDirty();
                break;
            case 4: _editing = true; break;
            case 5:
                // Which port to read on a two-controller adapter. Applied when the
                // adapter is next plugged in, since interfaces are chosen at mount.
                _cfg->cfg.hid_port = (uint8_t)((_cfg->cfg.hid_port + 1) & 1);
                _cfg->markDirty();
                break;
            case 6: _page = Page::MODE_SELECT; _cursor = (uint8_t)_cfg->cfg.mode; break;
            case 7: _page = Page::INPUT_INFO;  _cursor = 0; break;
            case 8:
                // Raw HID: bypass the report descriptor entirely. Takes effect
                // when the device is next plugged in.
                _cfg->cfg.force_raw_hid = !_cfg->cfg.force_raw_hid;
                _cfg->markDirty();
                break;
            case 9:
                // Manual recovery. If the USB endpoint dies while still holding
                // its transfer, that is indistinguishable from an idle joystick,
                // so it cannot be detected automatically - restart by hand.
                _cfg->save();                 // flush anything pending first
                disp.clearDisplay();
                disp.setTextSize(2); disp.setCursor(14, 24); disp.print(F("Restart"));
                disp.setTextSize(1); disp.display();
                disp.finish();
                delay(500);
                rp2040.reboot();
                break;
        }
    }
    if (e.back) { _page = Page::MAIN_MENU; _cursor = 4; }
}

void UIEngine::hSafetySub(const JoyEvent& e) {
    if (_editing) {
        if (_cursor == 1) {
            int8_t v = (int8_t)_cfg->cfg.thr_ch;
            if (e.up)   v++;
            if (e.down) v--;
            if (e.up || e.down) {
                if (v < 0) v = CRSF_NUM_CHANNELS - 1;
                if (v >= CRSF_NUM_CHANNELS) v = 0;
                _cfg->cfg.thr_ch = (uint8_t)v;
                _cfg->markDirty();
            }
        } else if (_cursor == 3) {
            int32_t g = (int32_t)_cfg->cfg.loss_grace_ms;
            if (e.up)   g += 250;
            if (e.down) g -= 250;
            if (e.up || e.down) {
                _cfg->cfg.loss_grace_ms = (uint16_t)constrain(g, 0, 5000);
                _cfg->markDirty();
            }
        }
        if (e.ok || e.back) _editing = false;
        return;
    }

    wrapCursor(_cursor, 5, e.up, e.down);
    if (e.ok) {
        switch (_cursor) {
            case 0: _cfg->cfg.thr_check = !_cfg->cfg.thr_check; _cfg->markDirty(); break;
            case 1: _editing = true; break;
            case 2:
                _cfg->cfg.loss_action =
                    (_cfg->cfg.loss_action == LinkLossAction::CUT_TX)
                        ? LinkLossAction::CHANNELS : LinkLossAction::CUT_TX;
                _cfg->markDirty();
                break;
            case 3: _editing = true; break;
            case 4:
                // Abort-and-retry recovery for a hung endpoint. Helps some sticks,
                // stops others reporting altogether, so it is off by default.
                _cfg->cfg.stall_recovery = !_cfg->cfg.stall_recovery;
                _cfg->markDirty();
                break;
        }
    }
    if (e.back) { _page = Page::SAFETY; _cursor = 0; }
}

// ── Sound Setup ─────────────────────────────────────────────────────────────
void UIEngine::hSoundList(const JoyEvent& e) {
    if (_editing) {   // volume
        int16_t v = _cfg->cfg.sound.volume;
        if (e.up)   v++;
        if (e.down) v--;
        if (e.up || e.down) {
            _cfg->cfg.sound.volume = (uint8_t)constrain((int)v, 0, 30);
            _cfg->markDirty();
            if (_voice) _voice->setVolume(_cfg->cfg.sound.volume);
        }
        if (e.ok || e.back) _editing = false;
        return;
    }
    wrapCursor(_cursor, 4, e.up, e.down);
    if (e.ok) {
        if (_cursor == 0) {
            _cfg->cfg.sound.enabled = !_cfg->cfg.sound.enabled;
            _cfg->markDirty();
            if (_voice) _voice->setEnabled(_cfg->cfg.sound.enabled);
        } else if (_cursor == 1) {
            _editing = true;
        } else {
            _sound_menu_sel = _cursor - 2;
            _cursor = 0;
            _page   = (_sound_menu_sel == 0) ? Page::SOUND_EVENTS : Page::SOUND_TRIG_LIST;
        }
    }
    if (e.back) { _page = Page::MAIN_MENU; _cursor = 3; }
}

void UIEngine::hSoundEvents(const JoyEvent& e) {
    wrapCursor(_cursor, SOUND_EVENT_COUNT, e.up, e.down);
    if (e.ok) {
        _sound_event_sel = _cursor;
        _page = Page::SOUND_EVENT_EDIT;
    }
    if (e.back) { _sound_menu_sel = 0; _cursor = 2; _page = Page::SOUND_LIST; }
}

void UIEngine::hSoundEventEdit(const JoyEvent& e) {
    uint16_t& t = _cfg->cfg.sound.event_track[_sound_event_sel];
    if (e.up)   { t = (uint16_t)constrain((int)t + 1, 0, 999); _cfg->markDirty(); if (_voice && t) _voice->play(t); }
    if (e.down) { t = (uint16_t)constrain((int)t - 1, 0, 999); _cfg->markDirty(); if (_voice && t) _voice->play(t); }
    if (e.ok) {
        if (_voice && t) _voice->play(t);   // one more confirm-preview
        _cursor = _sound_event_sel;
        _page   = Page::SOUND_EVENTS;
    }
    if (e.back) { _cursor = _sound_event_sel; _page = Page::SOUND_EVENTS; }
}

void UIEngine::hSoundTrigList(const JoyEvent& e) {
    wrapCursor(_cursor, MAX_SOUND_TRIGGERS, e.up, e.down);
    if (e.ok) {
        _sound_trig_sel = _cursor;
        _cursor = 0;
        _page   = Page::SOUND_TRIG_EDIT;
    }
    if (e.back) { _sound_menu_sel = 1; _cursor = 3; _page = Page::SOUND_LIST; }
}

void UIEngine::hSoundTrigEdit(const JoyEvent& e) {
    SoundTrigger& t = _cfg->cfg.sound.triggers[_sound_trig_sel];
    bool    axis      = (t.type == SourceType::AXIS);
    uint8_t n         = axis ? 4 : 2;
    uint8_t track_row = axis ? 3 : 1;

    if (_editing) {
        if (axis && _cursor == 1) {
            if (e.up)   { t.threshold = (int16_t)constrain((int)t.threshold + 500, -32768, 32767); _cfg->markDirty(); }
            if (e.down) { t.threshold = (int16_t)constrain((int)t.threshold - 500, -32768, 32767); _cfg->markDirty(); }
        } else if (_cursor == track_row) {
            if (e.up)   { t.track = (uint16_t)constrain((int)t.track + 1, 0, 999); _cfg->markDirty(); if (_voice && t.track) _voice->play(t.track); }
            if (e.down) { t.track = (uint16_t)constrain((int)t.track - 1, 0, 999); _cfg->markDirty(); if (_voice && t.track) _voice->play(t.track); }
        }
        if (e.ok || e.back) _editing = false;
        return;
    }

    wrapCursor(_cursor, n, e.up, e.down);

    if (_cursor == 0) {
        if (e.ok) {
            _learn_what = LEARN_SOUND_TRIG;
            _teach.begin(_in);
            _page = Page::LEARN_WAIT;
            return;
        }
        if (e.ok_long) { t = SoundTrigger{}; _cfg->markDirty(); }
    } else if (axis && _cursor == 1) {
        if (e.ok) _editing = true;
    } else if (axis && _cursor == 2) {
        if (e.ok) { t.rising = !t.rising; _cfg->markDirty(); }
    } else if (_cursor == track_row) {
        if (e.ok) _editing = true;
    }

    if (e.back) { _cursor = _sound_trig_sel; _page = Page::SOUND_TRIG_LIST; }
}

// ── Profiles ────────────────────────────────────────────────────────────────
void UIEngine::hProfiles(const JoyEvent& e) {
    if (!_prof || !_prof->available()) {
        if (e.back) { _page = Page::MAIN_MENU; _cursor = 0; }
        return;
    }
    uint8_t total = _pcount + 2;
    wrapCursor(_cursor, total, e.up, e.down);

    if (e.ok) {
        if (_cursor == 0) {
            if (_vid) {
                const char* keep = nullptr;   // keep a renamed profile's name
                for (uint8_t i = 0; i < _pcount; i++)
                    if (_plist[i].vid == _vid && _plist[i].pid == _pid) keep = _plist[i].name;
                _prof->save(_vid, _pid, keep, _cfg->cfg.channels, _cfg->cfg.cal, _cfg->cfg.model_id,
                            _cfg->cfg.sf, _cfg->cfg.timers, _cfg->cfg.timer_minute_call,
                _cfg->cfg.trim_sound_off);
                refreshProfiles();
                adoptProfile(_vid, _pid);
            }
        } else if (_cursor == 1) {
            saveAsNewSlot();
        } else {
            _psel   = _cursor - 2;         // open the load / delete menu
            _cursor = 0;
            _page   = Page::PROFILE_ACTION;
            return;
        }
    }
    if (e.back) { _page = Page::MAIN_MENU; _cursor = 0; }
}

void UIEngine::hProfileAction(const JoyEvent& e) {
    wrapCursor(_cursor, 3, e.up, e.down);

    if (e.ok && _psel < _pcount) {
        const ProfileInfo& pi = _plist[_psel];
        if (_cursor == 0) {
            if (_prof->load(pi.vid, pi.pid, _cfg->cfg.channels, _cfg->cfg.cal, &_cfg->cfg.model_id,
                            nullptr, 0, _cfg->cfg.sf, _cfg->cfg.timers, _cfg->cfg.timer_minute_call,
                            &_cfg->cfg.trim_sound_off)) {
                resetTimers();
                _cfg->markDirty();
                _crsf->sendModelId(_cfg->cfg.model_id);   // applied right away, not just on next reconnect
                setActiveProfile(pi.vid, pi.pid, pi.name);   // "the selected model" now - update Status page 0
            }
            _cursor = _psel + 2;
            _page   = Page::PROFILES;
        } else if (_cursor == 1) {
            memset(_str_buf, 0, sizeof(_str_buf));
            strncpy(_str_buf, pi.name, PROFILE_NAME_LEN - 1);
            _str_cursor = 0;
            _page       = Page::PROFILE_RENAME;
        } else {
            _yesno = 1;
            _page  = Page::PROFILE_DELETE;
        }
        return;
    }
    if (e.back) { _cursor = _psel + 2; _page = Page::PROFILES; }
}

void UIEngine::hProfileRename(const JoyEvent& e) {
    static const char CHARSET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:";
    const uint8_t CHARSET_N = sizeof(CHARSET) - 1;
    const uint8_t maxlen    = PROFILE_NAME_LEN - 1;

    // Pad with spaces up to the cursor if it's sitting past the current end.
    uint8_t len = (uint8_t)strlen(_str_buf);
    while (len <= _str_cursor && len < maxlen) { _str_buf[len] = ' '; len++; _str_buf[len] = 0; }

    if (e.up || e.down) {
        const char* at = strchr(CHARSET, _str_buf[_str_cursor]);
        uint8_t ci = at ? (uint8_t)(at - CHARSET) : 0;
        if (e.up)   ci = (uint8_t)((ci + 1) % CHARSET_N);
        if (e.down) ci = (ci == 0) ? (uint8_t)(CHARSET_N - 1) : (uint8_t)(ci - 1);
        _str_buf[_str_cursor] = CHARSET[ci];
    }

    if (e.ok_long) {
        int16_t n = (int16_t)strlen(_str_buf);
        while (n > 0 && _str_buf[n - 1] == ' ') n--;
        _str_buf[n] = 0;
        if (n > 0 && _psel < _pcount && _prof) {
            ProfileInfo& pi = _plist[_psel];
            if (_prof->rename(pi.vid, pi.pid, _str_buf)) {
                bool is_active = _active_valid && pi.vid == _active_vid && pi.pid == _active_pid;
                strncpy(pi.name, _str_buf, PROFILE_NAME_LEN - 1);
                pi.name[PROFILE_NAME_LEN - 1] = 0;
                if (is_active) setModelName(pi.name);
            }
        }
        _cursor = _psel + 2;
        _page   = Page::PROFILES;
        return;
    }
    if (e.ok && _str_cursor < maxlen - 1) _str_cursor++;
    if (e.back) {
        if (_str_cursor > 0) _str_cursor--;
        else { _cursor = 1; _page = Page::PROFILE_ACTION; }
    }
    if (e.back_long) { _cursor = 1; _page = Page::PROFILE_ACTION; }
}

void UIEngine::hProfileDelete(const JoyEvent& e) {
    if (e.up || e.down) _yesno ^= 1;
    // OK must be held to actually leave this screen via OK, whichever way
    // it's pointing - deleting is a long-press even with Yes selected, and
    // a short OK does nothing at all so a delete can never happen from a
    // single accidental tap. BACK still cancels immediately, as always.
    if (e.ok_long) {
        if (_yesno == 0 && _psel < _pcount) {
            if (_active_valid && _plist[_psel].vid == _active_vid && _plist[_psel].pid == _active_pid)
                setActiveProfile(0, 0, nullptr);   // active one deleted
            _prof->remove(_plist[_psel].vid, _plist[_psel].pid);
            refreshProfiles();
        }
        _cursor = 0;
        _page   = Page::PROFILES;
    }
    if (e.back) { _cursor = 0; _page = Page::PROFILE_ACTION; }
}

void UIEngine::hElrsList(const JoyEvent& e) {
    uint8_t kids[16];
    uint8_t n = elrsChildren(_elrs_folder, kids, 16);
    wrapCursor(_cursor, n, e.up, e.down);

    if (e.ok && n > 0 && _cursor < n) {
        const LiveParam* p = _crsf->liveByField(kids[_cursor]);
        if (p) {
            _elrs_field = p->field_id;
            switch (p->type) {
                case ParamType::P_FOLDER:
                    if (_elrs_stack_depth < 4) {
                        _elrs_stack[_elrs_stack_depth]        = _elrs_folder;
                        _elrs_stack_cursor[_elrs_stack_depth] = _cursor;
                        _elrs_stack_depth++;
                    }
                    _elrs_folder = p->field_id;
                    _cursor = 0;
                    break;
                case ParamType::P_TEXT_SELECTION:
                    _val  = (uint8_t)p->value;
                    _page = Page::ELRS_VALUE;
                    break;
                case ParamType::P_COMMAND:
                    _yesno       = 0;
                    _cmd_running = false;
                    _page        = Page::ELRS_COMMAND;
                    break;
                case ParamType::P_STRING:
                    strncpy(_str_buf, p->text, sizeof(_str_buf) - 1);
                    _str_buf[sizeof(_str_buf) - 1] = 0;
                    _str_cursor = 0;
                    _page = Page::ELRS_STR_EDIT;
                    break;
                case ParamType::P_INFO:
                    _page = Page::ELRS_INFO;
                    break;
                case ParamType::P_UINT8:  case ParamType::P_INT8:
                case ParamType::P_UINT16: case ParamType::P_INT16:
                case ParamType::P_UINT32: case ParamType::P_INT32:
                case ParamType::P_FLOAT:
                    _num_val = p->value;
                    _page    = Page::ELRS_NUM_EDIT;
                    break;
                default: break;   // VTX / unrecognised type - nothing sensible to do
            }
        }
    }

    if (e.back) {
        if (_elrs_stack_depth > 0) {
            _elrs_stack_depth--;
            _elrs_folder = _elrs_stack[_elrs_stack_depth];
            _cursor      = _elrs_stack_cursor[_elrs_stack_depth];
        } else {
            _page = Page::MAIN_MENU; _cursor = 2;
        }
    }
}

void UIEngine::hElrsValue(const JoyEvent& e) {
    const LiveParam* p = _crsf->liveByField(_elrs_field);
    if (!p) { _page = Page::ELRS_LIST; return; }
    uint8_t count = countOptions(p->text);
    wrapCursor(_val, count, e.up, e.down);
    if (e.ok) {
        _crsf->writeParamValue(*p, _val);
        _page = Page::ELRS_LIST;
    }
    if (e.back) _page = Page::ELRS_LIST;
}

void UIEngine::hElrsNumEdit(const JoyEvent& e) {
    const LiveParam* p = _crsf->liveByField(_elrs_field);
    if (!p) { _page = Page::ELRS_LIST; return; }

    int32_t range = p->vmax - p->vmin;
    int32_t step  = (p->type == ParamType::P_FLOAT)
                        ? (p->step > 0 ? p->step : 1)
                        : (range > 100 ? range / 50 : 1);
    if (step < 1) step = 1;

    if (e.up)   _num_val = (int32_t)constrain((long)(_num_val + step), (long)p->vmin, (long)p->vmax);
    if (e.down) _num_val = (int32_t)constrain((long)(_num_val - step), (long)p->vmin, (long)p->vmax);

    if (e.ok) {
        _crsf->writeParamValue(*p, _num_val);
        _page = Page::ELRS_LIST;
    }
    if (e.back) _page = Page::ELRS_LIST;
}

void UIEngine::hElrsStrEdit(const JoyEvent& e) {
    static const char CHARSET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    const uint8_t CHARSET_N = sizeof(CHARSET) - 1;   // exclude the trailing null

    const LiveParam* p = _crsf->liveByField(_elrs_field);
    if (!p) { _page = Page::ELRS_LIST; return; }

    uint8_t maxlen = (uint8_t)constrain((int)p->vmax, 1, (int)sizeof(_str_buf) - 1);
    uint8_t len    = (uint8_t)strlen(_str_buf);
    // Pad with spaces up to the cursor if it's sitting past the current end.
    while (len <= _str_cursor && len < maxlen) { _str_buf[len] = ' '; len++; _str_buf[len] = 0; }

    if (e.up || e.down) {
        const char* at = strchr(CHARSET, _str_buf[_str_cursor]);
        uint8_t ci = at ? (uint8_t)(at - CHARSET) : 0;
        if (e.up)   ci = (uint8_t)((ci + 1) % CHARSET_N);
        if (e.down) ci = (ci == 0) ? (uint8_t)(CHARSET_N - 1) : (uint8_t)(ci - 1);
        _str_buf[_str_cursor] = CHARSET[ci];
    }

    if (e.ok_long) {
        int16_t n = (int16_t)strlen(_str_buf);
        while (n > 0 && _str_buf[n - 1] == ' ') n--;   // trim trailing padding before sending
        _str_buf[n] = 0;
        _crsf->writeParamString(*p, _str_buf);
        _page = Page::ELRS_LIST;
    } else if (e.ok) {
        if (_str_cursor < maxlen - 1) _str_cursor++;
    }

    if (e.back) {
        if (_str_cursor > 0) _str_cursor--;
        else _page = Page::ELRS_LIST;   // already at the first character - back out
    }
    if (e.back_long) _page = Page::ELRS_LIST;
}

void UIEngine::hElrsInfo(const JoyEvent& e) {
    if (e.ok || e.back) _page = Page::ELRS_LIST;
}

void UIEngine::hElrsCommand(const JoyEvent& e) {
    if (!_cmd_running) {
        if (e.up || e.down) _yesno ^= 1;
        if (e.ok) {
            if (_yesno == 0) {
                _crsf->writeParamCommand(_elrs_field);
                _cmd_running     = true;
                _cmd_last_status = 0;
            } else {
                _page = Page::ELRS_LIST;
            }
        }
        if (e.back) _page = Page::ELRS_LIST;
    } else {
        const LiveParam* p = _crsf->liveByField(_elrs_field);
        if (p) _cmd_last_status = p->cmd_status;
        if (e.ok || e.back) {
            _cmd_running = false;
            _page = Page::ELRS_LIST;
        }
    }
}

void UIEngine::hSaveConfirm(const JoyEvent& e) {
    if (e.up || e.down) _yesno ^= 1;
    if (e.ok) {
        if (_yesno == 0) { _cfg->save(); }
        _page = Page::STATUS; _cursor = 0;
    }
    if (e.back) { _page = Page::STATUS; _cursor = 0; }
}

void UIEngine::hLinkSetup(const JoyEvent& e) {
    bool ppm = (_cfg->cfg.out_mode == OutputMode::PPM);
    const uint8_t N = ppm ? 5 : 3;

    // PPM's channel count / frame / pulse rows are ranged values.
    if (_editing && ppm) {
        PPMConfig& p = _cfg->cfg.ppm;
        int8_t dir = e.up ? 1 : (e.down ? -1 : 0);
        if (dir) {
            switch (_cursor) {
                case 1: p.channels = (uint8_t)constrain((int)p.channels + dir, 2, PPM_OUT_MAX_CH); break;
                case 2: p.frame_us = (uint16_t)constrain((int)p.frame_us + dir * 500, 5000, 40000); break;
                case 3: p.pulse_us = (uint16_t)constrain((int)p.pulse_us + dir * 50, 100, 1000); break;
            }
            _cfg->markDirty();
            g_ppm_out.begin(PIN_PPM_OUT, p);    // restart with the new timing
        }
        if (e.ok || e.back) _editing = false;
        return;
    }

    wrapCursor(_cursor, N, e.up, e.down);

    if (e.ok) {
        if (_cursor == 0) {
            // Switching output rewires GP0, so restart to bring it up cleanly.
            _cfg->cfg.out_mode = ppm ? OutputMode::CRSF : OutputMode::PPM;
            _cfg->save();
            disp.clearDisplay();
            disp.setTextSize(2); disp.setCursor(14, 24); disp.print(F("Reboot"));
            disp.setTextSize(1); disp.display();
            disp.finish();
            delay(600);
            rp2040.reboot();
        }
        else if (!ppm) {
            if (_cursor == 1) {
                // Choosing by hand ends the automatic search.
                _cfg->cfg.crsf_invert   = !_cfg->cfg.crsf_invert;
                _cfg->cfg.crsf_auto_pol = false;
                _crsf->setInvert(_cfg->cfg.crsf_invert);       // applied live
                _crsf->setAutoPolarity(false);
                _cfg->markDirty();
            } else {
                // Opens a list panel (2 pin TX/RX / 2 pin 1-wire / 1 pin
                // GP0) instead of cycling one step per press, so browsing
                // the options doesn't reboot the board every time - only
                // confirming one with OK does.
                uint8_t st = _cfg->cfg.crsf_single_pin ? 2
                           : _cfg->cfg.crsf_half_duplex ? 1 : 0;
                _cursor = st;
                _page   = Page::LINK_WIRE_SELECT;
                return;
            }
        }
        else if (_cursor == 4) {
            PPMConfig& p = _cfg->cfg.ppm;
            p.invert = !p.invert;
            _cfg->markDirty();
            g_ppm_out.begin(PIN_PPM_OUT, p);
        }
        else {
            _editing = true;         // open channels / frame / pulse for adjustment
        }
    }
    if (e.back) { _page = Page::SAFETY; _cursor = 1; }
}

static const char* WIRE_MODE_NAMES[3] = {
    "2 pin TX/RX", "2 pin half-duplex", "1 pin (GP0 only)"
};

// A list panel for the CRSF wiring row: UP/DOWN move the cursor, OK commits
// the choice (and reboots, same as before - switching to or from single pin
// re-plumbs the receiver, so it isn't done live), BACK returns to Link Setup
// without changing or rebooting.
void UIEngine::drawLinkWireSelect() {
    disp.clearDisplay();
    header("CRSF Wiring");
    uint8_t cur = _cfg->cfg.crsf_single_pin ? 2 : _cfg->cfg.crsf_half_duplex ? 1 : 0;
    for (uint8_t i = 0; i < 3; i++) {
        char b[22];
        snprintf(b, sizeof(b), "%s%s", WIRE_MODE_NAMES[i], i == cur ? "  *" : "");
        row(i, _cursor == i, b);
    }
    footer("Select", "Back");
    disp.display();
}

void UIEngine::hLinkWireSelect(const JoyEvent& e) {
    wrapCursor(_cursor, 3, e.up, e.down);
    if (e.ok) {
        uint8_t st = _cursor;
        _cfg->cfg.crsf_half_duplex = (st >= 1);
        _cfg->cfg.crsf_single_pin  = (st == 2);
        _cfg->save();
        disp.clearDisplay();
        disp.setTextSize(2); disp.setCursor(14, 24); disp.print(F("Reboot"));
        disp.setTextSize(1); disp.display();
        disp.finish();
        delay(600);
        rp2040.reboot();
    }
    if (e.back) { _cursor = 2; _page = Page::LINK_SETUP; }
}

void UIEngine::hInputInfo(const JoyEvent& e) {
    wrapCursor(_cursor, diagPageCount(), e.up, e.down);
    if (e.back) { _page = Page::SAFETY; _cursor = 7; }
}

// ═══════════════════════════════════════════════════════════════════════════════
//  MAIN UPDATE
// ═══════════════════════════════════════════════════════════════════════════════
void UIEngine::update(const uint16_t ch_out[CRSF_NUM_CHANNELS], const InputState& in) {
    _live = ch_out;
    _in   = in;

    if (_oled_ok) { disp.setAsync(true); disp.pump(); }   // one slice of the previous frame per pass

    JoyEvent e = _joy.read();

    // OK + BACK held together: save into the active profile, with a popup.
    if (e.save) saveActiveProfile();
    if (millis() < _toast_until) e = JoyEvent{};   // popup swallows input

    // Resend the Model ID whenever the module is (re)detected, regardless of
    // whether a voice module is fitted - a module that reset comes back
    // assuming Model ID 0 until told otherwise, same reasoning as EdgeTX's
    // own "resend after module reset" fix. Independent of the voice-chime
    // tracking below, which only exists when _voice is non-null.
    {
        bool found = _crsf->deviceFound();
        if (found && !_link_was_found) _crsf->sendModelId(_cfg->cfg.model_id);
        _link_was_found = found;
    }

    // Announce the link going up or down, whatever screen happens to be open.
    if (_voice) {
        bool link = _crsf->deviceFound();
        if (link != _voice_link_was) {
            playEvent(link ? SE_LINK_UP : SE_LINK_LOST);
            _voice_link_was = link;
        }

        // The startup throttle-not-idle warning, the moment it first appears.
        if (_thr_warn != _voice_thr_warn_was) {
            if (_thr_warn) playEvent(SE_THR_WARN);
            _voice_thr_warn_was = _thr_warn;
        }

        // Link quality tiers, with hysteresis so a value hovering near a
        // threshold doesn't chatter back and forth between announcements.
        // Only the way down is announced - recovery is silent, same as how
        // this pack's own RSSI alarms behave.
        LinkStats ls = _crsf->linkStats();
        if (ls.valid) {
            uint8_t tier = _voice_sig_tier;
            switch (tier) {
                case 0: if (ls.lq < 50) tier = 2; else if (ls.lq < 70) tier = 1; break;
                case 1: if (ls.lq < 50) tier = 2; else if (ls.lq >= 80) tier = 0; break;
                default: if (ls.lq >= 60) tier = 1; break;   // was critical
            }
            if (tier > _voice_sig_tier) playEvent(tier == 2 ? SE_SIG_CRIT : SE_SIG_LOW);
            _voice_sig_tier = tier;
        }

        // User-assigned button/channel triggers - see Menu > Sound Setup > Triggers.
        checkSoundTriggers(in);
        // Any channel's trim hitting max/min/centre, and the inactivity alarm.
        checkTrimEvents();
        checkInactivity(in, millis());
    }

    // Special Functions (Play Sound, Set/Start/Stop/Reset/Run Timer, Volume)
    // - see Menu > Special Functions > Functions. Override Channel is
    // handled separately, inline with mixing, via applyOverrides(). Runs
    // with or without a voice module: the timer functions don't need one.
    tickSpecialFunctions(in);

    // Timers always tick, whatever screen is open.
    tickTimers(millis());

    if (_page == Page::LEARN_SAVED && millis() - _saved_at > 900) {
        _cursor = 0;
        _page   = Page::SLOT_EDIT;
    }

    // A screen never inherits an "adjusting a value" state left over from
    // whatever page was open before it.
    if (_page != _last_page) {
        _editing = false;
        if (_page == Page::ELRS_LIST) {
            _crsf->beginDiscovery();
            _elrs_folder      = 0;
            _elrs_stack_depth = 0;
            _cursor           = 0;
        }
        if (_page == Page::SOUND_EVENT_EDIT) {
            uint16_t t = _cfg->cfg.sound.event_track[_sound_event_sel];
            if (_voice && t) _voice->play(t);
        }
        _last_page = _page;
    }

    switch (_page) {
        case Page::STATUS:        hStatus(e);       break;
        case Page::MAIN_MENU:     hMainMenu(e);     break;
        case Page::CONTROL_MENU:  hControlMenu(e);  break;
        case Page::MODE_SELECT:   hModeSelect(e);   break;
        case Page::MODE_CONFIRM:  hModeConfirm(e);  break;
        case Page::CHANNEL_LIST:  hChannelList(e);  break;
        case Page::CHANNEL_EDIT:  hChannelEdit(e);  break;
        case Page::SLOT_EDIT:     hSlotEdit(e);     break;
        case Page::LEARN_WAIT:    hLearnWait(e);    break;
        case Page::LEARN_SAVED:                     break;
        case Page::CHANNEL_CLEAR: hChannelClear(e); break;
        case Page::CURVE_EDIT:    hCurveEdit(e);    break;
        case Page::TRIM_EDIT:     hTrimEdit(e);     break;
        case Page::CAL_MENU:      hCalMenu(e);      break;
        case Page::CAL_SWEEP:     hCalSweep(e);     break;
        case Page::CAL_CENTER:    hCalCenter(e);    break;
        case Page::SAFETY:        hSafety(e);       break;
        case Page::SAFETY_SUB:    hSafetySub(e);    break;
        case Page::FS_EDIT:       hFsEdit(e);       break;
        case Page::FS_CAPTURE_ALL:hFsCaptureAll(e); break;
        case Page::PROFILES:      hProfiles(e);     break;
        case Page::PROFILE_ACTION:hProfileAction(e);break;
        case Page::PROFILE_DELETE:hProfileDelete(e);break;
        case Page::PROFILE_RENAME:hProfileRename(e);break;
        case Page::ELRS_LIST:     hElrsList(e);     break;
        case Page::ELRS_VALUE:    hElrsValue(e);    break;
        case Page::ELRS_NUM_EDIT: hElrsNumEdit(e);  break;
        case Page::ELRS_STR_EDIT: hElrsStrEdit(e);  break;
        case Page::ELRS_INFO:     hElrsInfo(e);     break;
        case Page::ELRS_COMMAND:  hElrsCommand(e);  break;
        case Page::LINK_SETUP:    hLinkSetup(e);    break;
        case Page::SAVE_CONFIRM:  hSaveConfirm(e);  break;
        case Page::INPUT_INFO:    hInputInfo(e);    break;
        case Page::SOUND_LIST:      hSoundList(e);      break;
        case Page::SOUND_EVENTS:    hSoundEvents(e);    break;
        case Page::SOUND_EVENT_EDIT: hSoundEventEdit(e); break;
        case Page::SOUND_TRIG_LIST: hSoundTrigList(e);  break;
        case Page::SOUND_TRIG_EDIT: hSoundTrigEdit(e);  break;
        case Page::SF_HOME:       hSFHome(e);       break;
        case Page::TIMER_LIST:    hTimerList(e);    break;
        case Page::TIMER_EDIT:    hTimerEdit(e);    break;
        case Page::SF_LIST:      hSFList(e);      break;
        case Page::SF_EDIT:      hSFEdit(e);      break;
        case Page::SF_FUNC_SELECT: hSFFuncSelect(e); break;
        case Page::SF_DELETE_CONFIRM:    hSFDeleteConfirm(e);    break;
        case Page::TIMER_DELETE_CONFIRM: hTimerDeleteConfirm(e); break;
        case Page::LINK_WIRE_SELECT:     hLinkWireSelect(e);     break;
    }

    uint32_t now = millis();
    if (now - _last_draw < 25) return;   // ~40 fps cap
    if (disp.busy()) return;             // previous frame still going out slice by slice
    _last_draw = now;

    if (millis() < _toast_until) { drawToast(); return; }

    // The throttle warning replaces the status screen but leaves the menu
    // reachable, so the check can still be turned off if it misfires.
    if (_thr_warn && _page == Page::STATUS) { drawThrottleWarn(); return; }

    switch (_page) {
        case Page::STATUS:        drawStatus();       break;
        case Page::MAIN_MENU:     drawMainMenu();     break;
        case Page::CONTROL_MENU:  drawControlMenu();  break;
        case Page::MODE_SELECT:   drawModeSelect();   break;
        case Page::MODE_CONFIRM:  drawModeConfirm();  break;
        case Page::CHANNEL_LIST:  drawChannelList();  break;
        case Page::CHANNEL_EDIT:  drawChannelEdit();  break;
        case Page::SLOT_EDIT:     drawSlotEdit();     break;
        case Page::LEARN_WAIT:    drawLearnWait();    break;
        case Page::LEARN_SAVED:   drawLearnSaved();   break;
        case Page::CHANNEL_CLEAR: drawChannelClear(); break;
        case Page::CURVE_EDIT:    drawCurveEdit();    break;
        case Page::TRIM_EDIT:     drawTrimEdit();     break;
        case Page::CAL_MENU:      drawCalMenu();      break;
        case Page::CAL_SWEEP:     drawCalSweep();     break;
        case Page::CAL_CENTER:    drawCalCenter();    break;
        case Page::SAFETY:        drawSafety();       break;
        case Page::SAFETY_SUB:    drawSafetySub();    break;
        case Page::FS_EDIT:       drawFsEdit();       break;
        case Page::FS_CAPTURE_ALL:drawFsCaptureAll(); break;
        case Page::PROFILES:      drawProfiles();     break;
        case Page::PROFILE_ACTION:drawProfileAction();break;
        case Page::PROFILE_DELETE:drawProfileDelete();break;
        case Page::PROFILE_RENAME:drawProfileRename();break;
        case Page::ELRS_LIST:     drawElrsList();     break;
        case Page::ELRS_VALUE:    drawElrsValue();    break;
        case Page::ELRS_NUM_EDIT: drawElrsNumEdit();  break;
        case Page::ELRS_STR_EDIT: drawElrsStrEdit();  break;
        case Page::ELRS_INFO:     drawElrsInfo();     break;
        case Page::ELRS_COMMAND:  drawElrsCommand();  break;
        case Page::LINK_SETUP:    drawLinkSetup();    break;
        case Page::SAVE_CONFIRM:  drawSaveConfirm();  break;
        case Page::INPUT_INFO:    drawInputInfo();    break;
        case Page::SOUND_LIST:      drawSoundList();      break;
        case Page::SOUND_EVENTS:    drawSoundEvents();    break;
        case Page::SOUND_EVENT_EDIT: drawSoundEventEdit(); break;
        case Page::SOUND_TRIG_LIST: drawSoundTrigList();  break;
        case Page::SOUND_TRIG_EDIT: drawSoundTrigEdit();  break;
        case Page::SF_HOME:       drawSFHome();       break;
        case Page::TIMER_LIST:    drawTimerList();    break;
        case Page::TIMER_EDIT:    drawTimerEdit();    break;
        case Page::SF_LIST:      drawSFList();      break;
        case Page::SF_EDIT:      drawSFEdit();      break;
        case Page::SF_FUNC_SELECT: drawSFFuncSelect(); break;
        case Page::SF_DELETE_CONFIRM:    drawSFDeleteConfirm();    break;
        case Page::TIMER_DELETE_CONFIRM: drawTimerDeleteConfirm(); break;
        case Page::LINK_WIRE_SELECT:     drawLinkWireSelect();     break;
    }
}
