#pragma once
#include <LittleFS.h>

// ═══════════════════════════════════════════════════════════════════════════════
//  Filesystem mount
//
//  Everything persistent lives in LittleFS on the onboard flash:
//      /settings.bin      global configuration
//      /p/VVVVPPPP        one file per USB controller
//
//  This survives reflashing the firmware, because a UF2 upload only rewrites
//  the sketch region and leaves the filesystem partition alone. EEPROM is kept
//  only as a fallback for boards configured without an FS partition, and as a
//  migration path for settings saved by older firmware.
//
//  REQUIRES Tools > Flash Size set to an option with a filesystem, e.g.
//  "2MB (Sketch: 1MB, FS: 1MB)".
// ═══════════════════════════════════════════════════════════════════════════════

#define FS_SETTINGS   "/settings.bin"
#define FS_SETTINGS_T "/settings.tmp"
#define FS_PROFILE_DIR "/p"

// Mounts once and remembers the result. Safe to call from anywhere.
inline bool fsMount() {
    static bool tried = false;
    static bool ok    = false;
    if (!tried) {
        tried = true;
        ok = LittleFS.begin();
        if (!ok) {                    // first use: format, then retry
            LittleFS.format();
            ok = LittleFS.begin();
        }
        if (ok) LittleFS.mkdir(FS_PROFILE_DIR);
    }
    return ok;
}

// ─── Version-tolerant record I/O ─────────────────────────────────────────────
//
//  Records are read as "however many bytes the file actually holds, up to the
//  size this firmware expects". Fields missing from an older file simply keep
//  their compile-time defaults, and trailing bytes from a newer file are
//  ignored.
//
//  THE RULE THAT MAKES THIS WORK: only ever APPEND fields to the persisted
//  structs. Never reorder or remove one, and never change the type of an
//  existing field. Follow that and settings survive firmware updates instead
//  of being wiped by a version check.

template <typename T>
inline bool fsLoadRecord(const char* path, T& out, uint32_t magic) {
    if (!fsMount()) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;

    T tmp{};                                   // start from the defaults
    size_t n = f.size();
    if (n > sizeof(T)) n = sizeof(T);          // newer file: ignore the extra
    size_t got = f.read((uint8_t*)&tmp, n);
    f.close();

    if (got < sizeof(uint32_t) || tmp.magic != magic) return false;
    out = tmp;                                 // older file: rest stays default
    return true;
}

// Written to a temporary file and renamed, so an interrupted write cannot
// leave a half-updated settings file behind.
template <typename T>
inline bool fsSaveRecord(const char* path, const char* tmp_path, const T& in) {
    if (!fsMount()) return false;

    File f = LittleFS.open(tmp_path, "w");
    if (!f) return false;
    size_t n = f.write((const uint8_t*)&in, sizeof(T));
    f.close();
    if (n != sizeof(T)) { LittleFS.remove(tmp_path); return false; }

    LittleFS.remove(path);
    return LittleFS.rename(tmp_path, path);
}
