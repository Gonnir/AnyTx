#pragma once
#include <Arduino.h>
#include <stdarg.h>
#include <hardware/watchdog.h>
#include "config.h"
#include "config_manager.h"
#include "profiles.h"
#include "crsf.h"
#include "sound.h"
#include "ui.h"

// ═══════════════════════════════════════════════════════════════════════════════
//  TinyTxCompanion - serial settings link
//
//  Talks to TinyTxCompanion.html (Web Serial) through a USB-serial adapter
//  wired where the ELRS module normally goes:
//
//      adapter RX  <-  GP0 (board TX)        adapter TX  ->  GP1 (board RX)
//      adapter GND  -  GND                    3.3 V logic ONLY, 420000 baud
//
//  Disconnect the ELRS module first. Nothing changes while the page is not
//  connected: the CRSF engine just passes every received byte through the
//  tap below and the handshake is "HELLO\n". Once seen, the CRSF engine
//  steps aside (no RC frames, plain polarity, TX driven) until the page says
//  BYE, goes quiet for IDLE_MS, or the board is restarted.
//
//  PROTOCOL  (text, one command per line, replies end with OK or ERR <why>)
//      HELLO                      OK TinyTxCompanion 1
//      PING | BYE | REBOOT
//      GET                        k=v lines: every global + live profile value
//      SET k=v                    change one live value (RAM only)
//      APPLY                      apply side effects + write settings to flash
//      PSAVE                      live values -> the active profile
//      PLIST                      "P vvvv pppp name" per profile, "A vvvv pppp"
//      PGET vvvv pppp             name=... + the profile's k=v lines
//      PBEGIN vvvv pppp name      start staging a profile (defaults)
//      PSET k=v                   edit the staged profile
//      PEND                       write the staged profile to flash
//      PLOAD vvvv pppp | PDEL vvvv pppp | PREN vvvv pppp name
//      LIVE 0|1                   stream "L ch0,..ch15" (CRSF units) at 10 Hz
//  vvvv/pppp are hex. Key/value schema: see dumpView()/dumpGlobal() below.
// ═══════════════════════════════════════════════════════════════════════════════

#define COMPANION_PROTO   1
#define COMPANION_IDLE_MS 12000

// Everything that belongs to a profile, as pointers, so the very same
// dump/set code serves the live settings and a staged/loaded profile.
struct CompView {
    ChannelConfig*   ch;
    AxisCal*         cal;
    uint8_t*         mid;
    SpecialFunction* sf;
    TimerDef*        tm;
    bool*            tmc;
    uint16_t*        tso;
};

static volatile bool g_comp_hello = false;

// Spots "HELLO\n" in the raw byte stream the CRSF engine reads.
static void companionTap(uint8_t b) {
    static const char pat[] = "HELLO\n";
    static uint8_t idx = 0;
    if (b == (uint8_t)pat[idx]) { if (++idx == 6) { idx = 0; g_comp_hello = true; } }
    else idx = (b == (uint8_t)pat[0]) ? 1 : 0;
}

class Companion {
public:
    void begin(HardwareSerial* s, ConfigManager* cfg, ProfileStore* prof, CRSFEngine* crsf,
               UIEngine* ui, DFPlayerVoice* voice, const uint16_t* live_ch) {
        _s = s; _cfg = cfg; _prof = prof; _crsf = crsf; _ui = ui; _voice = voice; _ch = live_ch;
        _crsf->setRxTap(companionTap);
    }

    bool active() const { return _active; }

    // Call every loop() while the output mode is CRSF.
    void service() {
        if (!_active) {
            if (g_comp_hello) { g_comp_hello = false; start(); }
            return;
        }
        while (_s->available()) {
            int c = _s->read();
            _last_rx = millis();
            if (c == '\r') continue;
            if (c == '\n') {
                _line[_n] = 0;
                if (!_overflow && _n) handle(_line);
                _n = 0; _overflow = false;
                if (!_active) return;
            } else if (_n < sizeof(_line) - 1) _line[_n++] = (char)c;
            else _overflow = true;
        }
        if (_live && millis() - _last_live >= 100) {
            _last_live = millis();
            char b[140]; int k = snprintf(b, sizeof(b), "L ");
            for (uint8_t i = 0; i < CRSF_NUM_CHANNELS; i++)
                k += snprintf(b + k, sizeof(b) - k, i ? ",%u" : "%u", _ch[i]);
            line("%s", b);
        }
        if (millis() - _last_rx > COMPANION_IDLE_MS) stop();
    }

private:
    HardwareSerial* _s = nullptr;
    ConfigManager*  _cfg = nullptr;
    ProfileStore*   _prof = nullptr;
    CRSFEngine*     _crsf = nullptr;
    UIEngine*       _ui = nullptr;
    DFPlayerVoice*  _voice = nullptr;
    const uint16_t* _ch = nullptr;

    bool     _active = false, _live = false, _overflow = false;
    uint32_t _last_rx = 0, _last_live = 0;
    char     _line[160];
    uint8_t  _n = 0;

    // Staged / loaded profile (one at a time).
    ChannelConfig   _pch[CRSF_NUM_CHANNELS];
    AxisCal         _pcal[MAX_INPUT_AXES];
    uint8_t         _pmid = 0;
    SpecialFunction _psf[MAX_SPECIAL_FUNCS];
    TimerDef        _ptm[MAX_TIMERS];
    bool            _ptmc[MAX_TIMERS] = { true, true, true, true };
    uint16_t        _ptso = 0;
    char            _pname[PROFILE_NAME_LEN] = {};
    uint16_t        _pvid = 0, _ppid = 0;
    bool            _staging = false;

    // ── session ─────────────────────────────────────────────────────────────
    void start() {
        _crsf->beginSession();
        _active = true; _live = false; _n = 0; _overflow = false; _staging = false;
        _last_rx = millis();
        line("OK TinyTxCompanion %d", COMPANION_PROTO);
    }
    void stop() {
        _live = false; _staging = false;
        _active = false;
        _crsf->endSession();
    }

    // ── output ──────────────────────────────────────────────────────────────
    void line(const char* fmt, ...) {
        char b[200];
        va_list ap; va_start(ap, fmt);
        int k = vsnprintf(b, sizeof(b) - 1, fmt, ap);
        va_end(ap);
        if (k < 0) return;
        if (k > (int)sizeof(b) - 2) k = sizeof(b) - 2;
        b[k++] = '\n';
        _s->write((const uint8_t*)b, k);
        watchdog_update();     // a long dump must not trip the 5 s watchdog
    }
    bool ok()                    { line("OK");        return true; }
    bool err(const char* why)    { line("ERR %s", why); return false; }

    // ── parsing helpers ─────────────────────────────────────────────────────
    static long cl(long x, long lo, long hi) { return x < lo ? lo : (x > hi ? hi : x); }

    // Comma-separated integers. Returns the count, or -1 on malformed input.
    static int parseList(const char* s, long* v, int max) {
        int n = 0;
        while (*s && n < max) {
            char* e; v[n] = strtol(s, &e, 10);
            if (e == s) return -1;
            n++; s = e;
            if (*s == ',') s++; else if (*s) return -1;
        }
        return *s ? -1 : n;
    }

    static void cleanName(char* dst, const char* src) {
        size_t k = 0;
        while (*src == ' ') src++;
        for (; *src && k < PROFILE_NAME_LEN - 1; src++)
            if (*src >= 32 && *src < 127 && *src != '=') dst[k++] = *src;
        while (k && dst[k - 1] == ' ') k--;
        dst[k] = 0;
    }

    CompView liveView() {
        AppConfig& c = _cfg->cfg;
        return { c.channels, c.cal, &c.model_id, c.sf, c.timers, c.timer_minute_call, &c.trim_sound_off };
    }
    CompView stageView() { return { _pch, _pcal, &_pmid, _psf, _ptm, _ptmc, &_ptso }; }

    void stageDefaults() {
        ConfigManager::setChannelDefaults(_pch, _cfg->cfg.mode);
        for (auto& a : _pcal) a = AxisCal{};
        for (auto& s : _psf)  s = SpecialFunction{};
        for (uint8_t i = 0; i < MAX_TIMERS; i++) { _ptm[i] = TimerDef{}; _ptmc[i] = true; }
        _pmid = 0; _ptso = 0;
    }

    // ── key/value schema: per-profile part ──────────────────────────────────
    void dumpView(const CompView& v) {
        line("mid=%u", *v.mid);
        line("tso=%u", *v.tso);
        for (uint8_t i = 0; i < CRSF_NUM_CHANNELS; i++) {
            const ChannelConfig& c = v.ch[i];
            line("ch.%u=%d,%d,%d,%d,%d,%d,%d,%u,%u,%u", i, (int)c.base, c.expo, c.curve_on,
                 c.weight, c.trim, (int)c.fs_mode, c.fs_value, c.trim_up, c.trim_dn, c.trim_step);
            line("ch.%u.cv=%d,%d,%d,%d,%d", i, c.curve[0], c.curve[1], c.curve[2], c.curve[3], c.curve[4]);
            for (uint8_t k = 0; k < MAX_MIX_SOURCES; k++) {
                const MixSource& m = c.src[k];
                line("ch.%u.s%u=%d,%u,%d,%d,%d", i, k, (int)m.type, m.index, m.weight,
                     (int)m.target, (int)m.btn_mode);
            }
        }
        for (uint8_t i = 0; i < MAX_INPUT_AXES; i++) {
            const AxisCal& a = v.cal[i];
            line("cal.%u=%d,%d,%d,%u", i, a.min, a.center, a.max, a.deadband);
        }
        for (uint8_t i = 0; i < MAX_SPECIAL_FUNCS; i++) {
            const SpecialFunction& f = v.sf[i];
            line("sf.%u=%d,%d,%u,%d,%d,%d,%u,%d,%u,%u,%u,%u,%u", i, (int)f.type, (int)f.trig_type,
                 f.trig_index, f.trig_threshold, f.trig_rising, f.fire_active, f.override_ch,
                 f.override_pct, f.sound_track, f.timer_idx, f.timer_set_s, f.vol_min, f.vol_max);
        }
        for (uint8_t i = 0; i < MAX_TIMERS; i++)
            line("tm.%u=%d,%d,%u,%d", i, v.tm[i].enabled, (int)v.tm[i].kind, v.tm[i].start_s, v.tmc[i]);
    }

    bool setView(const CompView& v, const char* key, const char* val) {
        long a[16]; int n;
        if (!strcmp(key, "mid")) { if (parseList(val, a, 1) != 1) return false; *v.mid = cl(a[0], 0, 255); return true; }
        if (!strcmp(key, "tso")) { if (parseList(val, a, 1) != 1) return false; *v.tso = (uint16_t)(a[0] & 0xFFFF); return true; }

        if (!strncmp(key, "ch.", 3)) {
            char* e; long i = strtol(key + 3, &e, 10);
            if (e == key + 3 || i < 0 || i >= CRSF_NUM_CHANNELS) return false;
            ChannelConfig& c = v.ch[i];
            if (*e == 0) {
                if (parseList(val, a, 10) != 10) return false;
                c.base = (MixTarget)cl(a[0], 0, 2);   c.expo = cl(a[1], -100, 100);
                c.curve_on = a[2] != 0;               c.weight = cl(a[3], -125, 125);
                c.trim = cl(a[4], -100, 100);         c.fs_mode = (FailsafeMode)cl(a[5], 0, 1);
                c.fs_value = cl(a[6], -100, 100);
                c.trim_up = (a[7] >= 0 && a[7] < 32) ? a[7] : TRIM_BTN_NONE;
                c.trim_dn = (a[8] >= 0 && a[8] < 32) ? a[8] : TRIM_BTN_NONE;
                c.trim_step = cl(a[9], 1, 25);
                return true;
            }
            if (!strcmp(e, ".cv")) {
                if (parseList(val, a, 5) != 5) return false;
                for (uint8_t k = 0; k < 5; k++) c.curve[k] = cl(a[k], -100, 100);
                return true;
            }
            if (e[0] == '.' && e[1] == 's' && e[2] >= '0' && e[2] < '0' + MAX_MIX_SOURCES && !e[3]) {
                if (parseList(val, a, 5) != 5) return false;
                MixSource& m = c.src[e[2] - '0'];
                m.type = (SourceType)cl(a[0], 0, 2);  m.index = cl(a[1], 0, 31);
                m.weight = cl(a[2], -100, 100);       m.target = (MixTarget)cl(a[3], 0, 2);
                m.btn_mode = (ButtonMode)cl(a[4], 0, 1);
                return true;
            }
            return false;
        }
        if (!strncmp(key, "cal.", 4)) {
            long i = atol(key + 4);
            if (i < 0 || i >= MAX_INPUT_AXES || parseList(val, a, 4) != 4) return false;
            v.cal[i].min = cl(a[0], -32768, 32767);  v.cal[i].center = cl(a[1], -32768, 32767);
            v.cal[i].max = cl(a[2], -32768, 32767);  v.cal[i].deadband = cl(a[3], 0, 25);
            return true;
        }
        if (!strncmp(key, "sf.", 3)) {
            long i = atol(key + 3);
            if (i < 0 || i >= MAX_SPECIAL_FUNCS || parseList(val, a, 13) != 13) return false;
            SpecialFunction& f = v.sf[i];
            f.type = (SFType)cl(a[0], 0, (long)SFType::_COUNT - 1);
            f.trig_type = (SourceType)cl(a[1], 0, 3);   f.trig_index = cl(a[2], 0, 31);
            f.trig_threshold = cl(a[3], -32768, 32767); f.trig_rising = a[4] != 0;
            f.fire_active = a[5] != 0;                  f.override_ch = cl(a[6], 0, CRSF_NUM_CHANNELS - 1);
            f.override_pct = cl(a[7], -100, 100);       f.sound_track = cl(a[8], 0, 3000);
            f.timer_idx = cl(a[9], 0, MAX_TIMERS - 1);  f.timer_set_s = cl(a[10], 0, 5999);
            f.vol_min = cl(a[11], 0, 30);               f.vol_max = cl(a[12], 0, 30);
            return true;
        }
        if (!strncmp(key, "tm.", 3)) {
            long i = atol(key + 3);
            if (i < 0 || i >= MAX_TIMERS || parseList(val, a, 4) != 4) return false;
            v.tm[i].enabled = a[0] != 0;  v.tm[i].kind = (TimerKind)cl(a[1], 0, 1);
            v.tm[i].start_s = cl(a[2], 0, 5999);  v.tmc[i] = a[3] != 0;
            return true;
        }
        return false;
    }

    // ── key/value schema: global part ───────────────────────────────────────
    void dumpGlobal() {
        const AppConfig& c = _cfg->cfg;
        line("mode=%d", (int)c.mode);
        line("out=%d", (int)c.out_mode);
        line("inv=%d", c.crsf_invert);
        line("hd=%d", c.crsf_half_duplex);
        line("single=%d", c.crsf_single_pin);
        line("autopol=%d", c.crsf_auto_pol);
        line("hidport=%u", c.hid_port);
        line("raw=%d", c.force_raw_hid);
        line("stall=%d", c.stall_recovery);
        line("ppm=%u,%u,%u,%d", c.ppm.channels, c.ppm.frame_us, c.ppm.pulse_us, c.ppm.invert);
        line("thr=%d,%u", c.thr_check, c.thr_ch);
        line("loss=%d,%u", (int)c.loss_action, c.loss_grace_ms);
        line("snd=%d,%u", c.sound.enabled, c.sound.volume);
        { char b[120]; int k = 0;
          for (uint8_t i = 0; i < SOUND_EVENT_COUNT; i++)
              k += snprintf(b + k, sizeof(b) - k, i ? ",%u" : "%u", c.sound.event_track[i]);
          line("snd.ev=%s", b); }
        for (uint8_t i = 0; i < MAX_SOUND_TRIGGERS; i++) {
            const SoundTrigger& t = c.sound.triggers[i];
            line("snd.tg.%u=%d,%u,%d,%d,%u", i, (int)t.type, t.index, t.threshold, t.rising, t.track);
        }
        line("inact=%d,%u", c.inactivity.enabled, c.inactivity.minutes);
    }

    bool setGlobal(const char* key, const char* val) {
        AppConfig& c = _cfg->cfg;
        long a[SOUND_EVENT_COUNT]; int n;
        auto one = [&](long lo, long hi, long& out) { if (parseList(val, a, 1) != 1) return false; out = cl(a[0], lo, hi); return true; };
        long x;
        if (!strcmp(key, "mode"))    { if (!one(0, (long)DeviceMode::_COUNT - 1, x)) return false; c.mode = (DeviceMode)x; return true; }
        if (!strcmp(key, "out"))     { if (!one(0, 1, x)) return false; c.out_mode = (OutputMode)x; return true; }
        if (!strcmp(key, "inv"))     { if (!one(0, 1, x)) return false; c.crsf_invert = x; return true; }
        if (!strcmp(key, "hd"))      { if (!one(0, 1, x)) return false; c.crsf_half_duplex = x; return true; }
        if (!strcmp(key, "single"))  { if (!one(0, 1, x)) return false; c.crsf_single_pin = x; return true; }
        if (!strcmp(key, "autopol")) { if (!one(0, 1, x)) return false; c.crsf_auto_pol = x; return true; }
        if (!strcmp(key, "hidport")) { if (!one(0, 1, x)) return false; c.hid_port = x; return true; }
        if (!strcmp(key, "raw"))     { if (!one(0, 1, x)) return false; c.force_raw_hid = x; return true; }
        if (!strcmp(key, "stall"))   { if (!one(0, 1, x)) return false; c.stall_recovery = x; return true; }
        if (!strcmp(key, "ppm"))     { if (parseList(val, a, 4) != 4) return false;
            c.ppm.channels = cl(a[0], 2, PPM_OUT_MAX_CH);  c.ppm.frame_us = cl(a[1], 5000, 40000);
            c.ppm.pulse_us = cl(a[2], 100, 1000);          c.ppm.invert = a[3] != 0; return true; }
        if (!strcmp(key, "thr"))     { if (parseList(val, a, 2) != 2) return false;
            c.thr_check = a[0] != 0; c.thr_ch = cl(a[1], 0, CRSF_NUM_CHANNELS - 1); return true; }
        if (!strcmp(key, "loss"))    { if (parseList(val, a, 2) != 2) return false;
            c.loss_action = (LinkLossAction)cl(a[0], 0, 1); c.loss_grace_ms = cl(a[1], 0, 5000); return true; }
        if (!strcmp(key, "snd"))     { if (parseList(val, a, 2) != 2) return false;
            c.sound.enabled = a[0] != 0; c.sound.volume = cl(a[1], 0, 30); return true; }
        if (!strcmp(key, "snd.ev"))  { if (parseList(val, a, SOUND_EVENT_COUNT) != SOUND_EVENT_COUNT) return false;
            for (uint8_t i = 0; i < SOUND_EVENT_COUNT; i++) c.sound.event_track[i] = cl(a[i], 0, 3000); return true; }
        if (!strncmp(key, "snd.tg.", 7)) {
            long i = atol(key + 7);
            if (i < 0 || i >= MAX_SOUND_TRIGGERS || parseList(val, a, 5) != 5) return false;
            SoundTrigger& t = c.sound.triggers[i];
            t.type = (SourceType)cl(a[0], 0, 2); t.index = cl(a[1], 0, 31);
            t.threshold = cl(a[2], -32768, 32767); t.rising = a[3] != 0; t.track = cl(a[4], 0, 3000);
            return true;
        }
        if (!strcmp(key, "inact"))   { if (parseList(val, a, 2) != 2) return false;
            c.inactivity.enabled = a[0] != 0; c.inactivity.minutes = cl(a[1], 1, 99); return true; }
        (void)n;
        return false;
    }

    // ── commands ────────────────────────────────────────────────────────────
    static bool parseId(const char* s, uint16_t& vid, uint16_t& pid, const char** rest) {
        char* e;
        unsigned long a = strtoul(s, &e, 16);          if (e == s) return false;
        const char* t = e; unsigned long b = strtoul(t, &e, 16); if (e == t) return false;
        if (a > 0xFFFF || b > 0xFFFF) return false;
        vid = a; pid = b; if (rest) *rest = e;
        return true;
    }

    void handle(char* l) {
        char* arg = strchr(l, ' ');
        if (arg) { *arg++ = 0; while (*arg == ' ') arg++; } else arg = (char*)"";
        const char* cmd = l;
        uint16_t vid, pid; const char* rest;

        if (!strcmp(cmd, "HELLO"))  { line("OK TinyTxCompanion %d", COMPANION_PROTO); return; }
        if (!strcmp(cmd, "PING"))   { ok(); return; }
        if (!strcmp(cmd, "BYE"))    { ok(); stop(); return; }
        if (!strcmp(cmd, "LIVE"))   { _live = (*arg == '1'); ok(); return; }
        if (!strcmp(cmd, "REBOOT")) {
            ok(); _s->flush(); _cfg->save(); delay(100); rp2040.reboot(); return;
        }

        if (!strcmp(cmd, "GET")) {
            dumpGlobal(); dumpView(liveView()); ok(); return;
        }
        if (!strcmp(cmd, "SET")) {
            char* eq = strchr(arg, '=');
            if (!eq) { err("format"); return; }
            *eq = 0;
            if (setGlobal(arg, eq + 1) || setView(liveView(), arg, eq + 1)) { _cfg->markDirty(); ok(); }
            else err("bad key or value");
            return;
        }
        if (!strcmp(cmd, "APPLY")) {
            if (_voice) { _voice->setEnabled(_cfg->cfg.sound.enabled); _voice->setVolume(_cfg->cfg.sound.volume); }
            _ui->resetTimers();
            if (_cfg->save()) ok(); else err("flash write failed");
            return;
        }
        if (!strcmp(cmd, "PSAVE")) {
            if (_ui->companionSaveActive()) ok(); else err("no active profile");
            return;
        }

        // ── profiles ────────────────────────────────────────────────────────
        if (!_prof || !_prof->available()) { err("no filesystem"); return; }

        if (!strcmp(cmd, "PLIST")) {
            ProfileInfo pl[MAX_PROFILES];
            uint8_t cnt = _prof->list(pl, MAX_PROFILES);
            for (uint8_t i = 0; i < cnt; i++) line("P %04X %04X %s", pl[i].vid, pl[i].pid, pl[i].name);
            if (_ui->activeProfile(vid, pid)) line("A %04X %04X", vid, pid);
            ok(); return;
        }
        if (!strcmp(cmd, "PGET")) {
            if (!parseId(arg, vid, pid, nullptr)) { err("format"); return; }
            stageDefaults(); _pname[0] = 0;
            if (!_prof->load(vid, pid, _pch, _pcal, &_pmid, _pname, sizeof(_pname), _psf, _ptm, _ptmc, &_ptso))
                { err("not found"); return; }
            line("name=%s", _pname);
            dumpView(stageView()); ok(); return;
        }
        if (!strcmp(cmd, "PBEGIN")) {
            if (!parseId(arg, vid, pid, &rest)) { err("format"); return; }
            ProfileInfo pl[MAX_PROFILES];
            uint8_t cnt = _prof->list(pl, MAX_PROFILES);
            if (!_prof->exists(vid, pid) && cnt >= MAX_PROFILES) { err("profile store full"); return; }
            stageDefaults();
            cleanName(_pname, rest);
            if (!_pname[0]) snprintf(_pname, sizeof(_pname), "%04X:%04X", vid, pid);
            _pvid = vid; _ppid = pid; _staging = true;
            ok(); return;
        }
        if (!strcmp(cmd, "PSET")) {
            if (!_staging) { err("no PBEGIN"); return; }
            char* eq = strchr(arg, '=');
            if (!eq) { err("format"); return; }
            *eq = 0;
            if (setView(stageView(), arg, eq + 1)) ok(); else err("bad key or value");
            return;
        }
        if (!strcmp(cmd, "PEND")) {
            if (!_staging) { err("no PBEGIN"); return; }
            _staging = false;
            if (!_prof->save(_pvid, _ppid, _pname, _pch, _pcal, _pmid, _psf, _ptm, _ptmc, _ptso)) { err("flash write failed"); return; }
            uint16_t av, ap;
            bool is_active = _ui->activeProfile(av, ap) && av == _pvid && ap == _ppid;
            _ui->companionProfilesChanged();
            if (is_active) _ui->companionLoadProfile(_pvid, _ppid);   // live copy follows the stored one
            ok(); return;
        }
        if (!strcmp(cmd, "PLOAD")) {
            if (!parseId(arg, vid, pid, nullptr)) { err("format"); return; }
            if (_ui->companionLoadProfile(vid, pid)) ok(); else err("not found");
            return;
        }
        if (!strcmp(cmd, "PDEL")) {
            if (!parseId(arg, vid, pid, nullptr)) { err("format"); return; }
            if (_prof->remove(vid, pid)) { _ui->companionProfilesChanged(); ok(); } else err("not found");
            return;
        }
        if (!strcmp(cmd, "PREN")) {
            if (!parseId(arg, vid, pid, &rest)) { err("format"); return; }
            char nm[PROFILE_NAME_LEN]; cleanName(nm, rest);
            if (!nm[0]) { err("empty name"); return; }
            if (_prof->rename(vid, pid, nm)) { _ui->companionProfilesChanged(); ok(); } else err("not found");
            return;
        }
        err("unknown command");
    }
};
