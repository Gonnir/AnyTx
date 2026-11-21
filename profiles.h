#pragma once
#include <stddef.h>
#include "config.h"
#include "storage.h"

// ═══════════════════════════════════════════════════════════════════════════════
//  Per-device profile storage (LittleFS on the RP2040's onboard flash)
//
//  Every USB controller is identified by its VID/PID and gets its own channel
//  mix, saved as /p/VVVVPPPP.  Plug the device back in and its mapping is
//  restored automatically. On top of that, Menu > Profiles > "Save as new
//  slot" saves a named, hand-managed profile that isn't tied to any device
//  at all - so more than one profile can exist for the same USB controller,
//  or for a mode with no USB device to key on in the first place (SBUS,
//  PPM, ELRS Repeater, Pot/Btn UART). See SLOT_PROFILE_VID below.
//
//  REQUIRES a filesystem partition: in the Arduino IDE choose
//      Tools > Flash Size > "2MB (Sketch: 1MB, FS: 1MB)"
//  (or any option with a non-zero FS).  If no partition exists, available()
//  returns false and the UI says so rather than silently losing profiles.
//  Global settings live in EEPROM and are unaffected either way.
// ═══════════════════════════════════════════════════════════════════════════════

#define PROFILE_DIR      FS_PROFILE_DIR
#define MAX_PROFILES     16

// A "named slot" profile isn't tied to any physical USB device - it's saved
// and loaded by hand from Menu > Profiles > Save as new slot, so more than
// one profile can exist for the same input mode (or for a mode that has no
// USB device to key on at all, like SBUS/PPM/Pot-Btn UART). It's stored
// exactly like a real device's profile, just addressed with this reserved
// vid (outside any real USB vendor's range) and the slot number as the pid.
#define SLOT_PROFILE_VID 0xFFF0

struct ProfileInfo {
    uint16_t vid, pid;
    char     name[PROFILE_NAME_LEN];
};

class ProfileStore {
public:
    bool begin() {
        _ok = fsMount();
        return _ok;
    }

    bool available() const { return _ok; }

    // ── Load a device's channel mix ─────────────────────────────────────────
    // name_out/name_out_sz are optional: pass a buffer to also get the
    // profile's own saved name back, e.g. to show "which model" is now
    // active (see UIEngine::setModelName()).
    bool load(uint16_t vid, uint16_t pid, ChannelConfig* out, AxisCal* cal, uint8_t* model_id = nullptr,
              char* name_out = nullptr, size_t name_out_sz = 0, SpecialFunction* sf = nullptr,
              TimerDef* timers = nullptr, bool* timer_minute_call = nullptr,
              uint16_t* trim_sound_off = nullptr) {
        if (!_ok) return false;
        char path[24];
        makePath(path, sizeof(path), vid, pid);
        File f = LittleFS.open(path, "r");
        if (!f) return false;

        // Read whatever the file holds, up to what this firmware expects.
        // Fields added since the file was written keep their defaults, so a
        // profile saved by older firmware still loads. See storage.h.
        DeviceProfile p{};
        size_t n = f.size();
        if (n > sizeof(p)) n = sizeof(p);
        size_t got = f.read((uint8_t*)&p, n);
        f.close();
        if (got < sizeof(uint32_t) || p.magic != PROFILE_MAGIC) return false;

        memcpy(out, p.channels, sizeof(p.channels));
        if (cal) memcpy(cal, p.cal, sizeof(p.cal));
        if (model_id) *model_id = p.model_id;
        // Profiles saved before special functions travelled with them are
        // shorter than the current struct - leave the live ones untouched
        // rather than wiping them with empty defaults.
        if (sf && n >= offsetof(DeviceProfile, sf) + sizeof(p.sf)) memcpy(sf, p.sf, sizeof(p.sf));
        if (trim_sound_off && n >= offsetof(DeviceProfile, trim_sound_off) + sizeof(p.trim_sound_off))
            *trim_sound_off = p.trim_sound_off;
        // Same for timers (added after special functions).
        if (timers && n >= offsetof(DeviceProfile, timer_minute_call) + sizeof(p.timer_minute_call)) {
            memcpy(timers, p.timers, sizeof(p.timers));
            if (timer_minute_call) memcpy(timer_minute_call, p.timer_minute_call, sizeof(p.timer_minute_call));
        }
        if (name_out && name_out_sz) {
            strncpy(name_out, p.name, name_out_sz - 1);
            name_out[name_out_sz - 1] = 0;
        }
        return true;
    }

    // ── Save / overwrite ────────────────────────────────────────────────────
    bool save(uint16_t vid, uint16_t pid, const char* name,
              const ChannelConfig* channels, const AxisCal* cal, uint8_t model_id = 0,
              const SpecialFunction* sf = nullptr, const TimerDef* timers = nullptr,
              const bool* timer_minute_call = nullptr, uint16_t trim_sound_off = 0) {
        if (!_ok) return false;

        DeviceProfile p = {};
        p.magic = PROFILE_MAGIC;
        p.vid   = vid;
        p.pid   = pid;
        if (name && *name) strncpy(p.name, name, PROFILE_NAME_LEN - 1);
        else               snprintf(p.name, PROFILE_NAME_LEN, "%04X:%04X", vid, pid);
        memcpy(p.channels, channels, sizeof(p.channels));
        if (cal) memcpy(p.cal, cal, sizeof(p.cal));
        p.model_id = model_id;
        if (sf) memcpy(p.sf, sf, sizeof(p.sf));
        p.trim_sound_off = trim_sound_off;
        if (timers) memcpy(p.timers, timers, sizeof(p.timers));
        if (timer_minute_call) memcpy(p.timer_minute_call, timer_minute_call, sizeof(p.timer_minute_call));

        char path[24];
        makePath(path, sizeof(path), vid, pid);
        File f = LittleFS.open(path, "w");
        if (!f) return false;
        size_t n = f.write((const uint8_t*)&p, sizeof(p));
        f.close();
        return n == sizeof(p);
    }

    bool remove(uint16_t vid, uint16_t pid) {
        if (!_ok) return false;
        char path[24];
        makePath(path, sizeof(path), vid, pid);
        return LittleFS.remove(path);
    }

    // Rename in place: only the name bytes are rewritten, so an older, shorter
    // file keeps its size (and therefore keeps being read as "no special
    // functions / timers in here").
    bool rename(uint16_t vid, uint16_t pid, const char* name) {
        if (!_ok || !name || !*name) return false;
        char path[24];
        makePath(path, sizeof(path), vid, pid);
        File f = LittleFS.open(path, "r+");
        if (!f) return false;
        char buf[PROFILE_NAME_LEN] = {};
        strncpy(buf, name, PROFILE_NAME_LEN - 1);
        bool ok = f.seek(offsetof(DeviceProfile, name)) &&
                  f.write((const uint8_t*)buf, sizeof(buf)) == sizeof(buf);
        f.close();
        return ok;
    }

    bool exists(uint16_t vid, uint16_t pid) {
        if (!_ok) return false;
        char path[24];
        makePath(path, sizeof(path), vid, pid);
        return LittleFS.exists(path);
    }

    // ── Enumerate saved profiles ────────────────────────────────────────────
    uint8_t list(ProfileInfo* out, uint8_t max) {
        if (!_ok) return 0;
        uint8_t n = 0;
        Dir dir = LittleFS.openDir(PROFILE_DIR);
        while (dir.next() && n < max) {
            File f = dir.openFile("r");
            if (!f) continue;
            DeviceProfile p{};
            size_t sz = f.size();
            if (sz > sizeof(p)) sz = sizeof(p);
            if (f.read((uint8_t*)&p, sz) >= (int)sizeof(uint32_t) &&
                p.magic == PROFILE_MAGIC) {
                out[n].vid = p.vid;
                out[n].pid = p.pid;
                memcpy(out[n].name, p.name, PROFILE_NAME_LEN);
                out[n].name[PROFILE_NAME_LEN - 1] = 0;
                n++;
            }
            f.close();
        }
        return n;
    }

private:
    bool _ok = false;
    static void makePath(char* buf, size_t sz, uint16_t vid, uint16_t pid) {
        snprintf(buf, sz, PROFILE_DIR "/%04X%04X", vid, pid);
    }
};
