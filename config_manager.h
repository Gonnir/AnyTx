#pragma once
#include <EEPROM.h>
#include "config.h"
#include "storage.h"

// ═══════════════════════════════════════════════════════════════════════════════
//  Global settings
//
//  Stored as /settings.bin in LittleFS, which survives reflashing the firmware.
//  EEPROM is used only when no filesystem partition exists, and is read once at
//  startup to migrate settings saved by older firmware.
//
//  Loading is version-tolerant: see the note in storage.h. Append new fields to
//  AppConfig and existing settings keep working across firmware updates.
// ═══════════════════════════════════════════════════════════════════════════════

class ConfigManager {
public:
    AppConfig cfg;

    void begin() {
        EEPROM.begin(4096);
        _fs = fsMount();

        if (_fs && fsLoadRecord(FS_SETTINGS, cfg, (uint32_t)CONFIG_MAGIC)) {
            sanitise();
            return;                       // loaded from the filesystem
        }

        // No file yet. Settings written by older firmware live in EEPROM, so
        // pull them across rather than starting from scratch.
        AppConfig old{};
        EEPROM.get(0, old);
        if (old.magic == CONFIG_MAGIC) {
            cfg = old;
            sanitise();
            save();                       // promote it to the filesystem
            return;
        }

        setDefaults(DeviceMode::USB_INPUT);
        save();
    }

    bool usingFilesystem() const { return _fs; }

    // Immediate write. Use when the value must survive right now, e.g. just
    // before a reboot.
    bool save() {
        cfg.magic   = CONFIG_MAGIC;
        cfg.version = CONFIG_VERSION;
        _dirty = false;

        if (_fs && fsSaveRecord(FS_SETTINGS, FS_SETTINGS_T, cfg)) return true;

        // No filesystem: fall back to EEPROM so settings are not simply lost.
        EEPROM.put(0, cfg);
        return EEPROM.commit();
    }

    // Preferred for menu edits. Holding left or right would otherwise rewrite
    // flash on every step; the change is kept in RAM and written once the user
    // stops adjusting.
    void markDirty() { _dirty = true; _dirty_at = millis(); }

    // Call once per loop from Core0.
    bool maybeCommit(uint32_t settle_ms = 600) {
        if (!_dirty) return false;
        if (millis() - _dirty_at < settle_ms) return false;
        return save();
    }

    bool pendingSave() const { return _dirty; }

    // Factory defaults. Every mode gets a sane 1:1 axis mapping, because all
    // input sources expose the same generic axis/button model.
    void setDefaults(DeviceMode m) {
        cfg = AppConfig{};
        cfg.magic   = CONFIG_MAGIC;
        cfg.version = CONFIG_VERSION;
        cfg.mode    = m;
        setChannelDefaults(cfg.channels, m);
    }

    // Also used when a USB device is plugged in with no saved profile.
    static void setChannelDefaults(ChannelConfig* ch, DeviceMode m) {
        for (uint8_t i = 0; i < CRSF_NUM_CHANNELS; i++) ch[i] = ChannelConfig{};

        if (m == DeviceMode::USB_INPUT) {
            // Typical gamepad: sticks on CH1-4, pitch reversed.
            const uint8_t axmap[4] = { 0, 1, 3, 2 };
            const int8_t  wmap [4] = { 100, -100, 100, 100 };
            for (uint8_t i = 0; i < 4; i++) {
                ch[i].base          = MixTarget::MID;
                ch[i].src[0].type   = SourceType::AXIS;
                ch[i].src[0].index  = axmap[i];
                ch[i].src[0].weight = wmap[i];
            }
            // CH5: rests low, first button latches it high (arm switch).
            // On a PlayStation pad button 0 is Square, on Xbox it is A - in
            // both cases a face button, which is the point.
            ch[4].base            = MixTarget::LOW;
            ch[4].src[0].type     = SourceType::BUTTON;
            ch[4].src[0].index    = 0;
            ch[4].src[0].weight   = 100;
            ch[4].src[0].target   = MixTarget::HIGH;
            ch[4].src[0].btn_mode = ButtonMode::STICKY;
        } else {
            // SBUS / PPM / ELRS-RX: straight 1:1 passthrough
            for (uint8_t i = 0; i < CRSF_NUM_CHANNELS; i++) {
                ch[i].base          = MixTarget::MID;
                ch[i].src[0].type   = SourceType::AXIS;
                ch[i].src[0].index  = i;
                ch[i].src[0].weight = 100;
            }
        }
    }

private:
    bool     _fs       = false;
    bool     _dirty    = false;
    uint32_t _dirty_at = 0;

    // A file written by different firmware may hold values this build cannot
    // use. Clamp anything that would otherwise misbehave.
    void sanitise() {
        if ((uint8_t)cfg.mode >= (uint8_t)DeviceMode::_COUNT)
            cfg.mode = DeviceMode::USB_INPUT;
        if (cfg.thr_ch >= CRSF_NUM_CHANNELS) cfg.thr_ch = 2;
        if (cfg.ppm.channels < 2 || cfg.ppm.channels > PPM_OUT_MAX_CH)
            cfg.ppm.channels = 8;
        if (cfg.ppm.frame_us < 5000 || cfg.ppm.frame_us > 40000)
            cfg.ppm.frame_us = 22500;
        if (cfg.ppm.pulse_us < 100 || cfg.ppm.pulse_us > 1000)
            cfg.ppm.pulse_us = 300;
        if (cfg.loss_grace_ms > 5000) cfg.loss_grace_ms = 1000;
        for (uint8_t i = 0; i < MAX_INPUT_AXES; i++)
            if (cfg.cal[i].deadband > 25) cfg.cal[i].deadband = 0;

        // hid_port out of range would match no interface at all, so no
        // controller would ever be adopted - a silent total failure.
        if (cfg.hid_port > 1) cfg.hid_port = 0;

        // Per-channel values need the same treatment. A file written by other
        // firmware can hold anything, and an out-of-range enum or a zero trim
        // step misbehaves quietly rather than obviously.
        for (uint8_t i = 0; i < CRSF_NUM_CHANNELS; i++) {
            ChannelConfig& c = cfg.channels[i];
            if ((uint8_t)c.base    > (uint8_t)MixTarget::HIGH)  c.base    = MixTarget::MID;
            if ((uint8_t)c.fs_mode > (uint8_t)FailsafeMode::VALUE)
                c.fs_mode = FailsafeMode::HOLD;
            if (c.trim_step < 1 || c.trim_step > 25) c.trim_step = 1;
            if (c.weight   < -125 || c.weight   > 125) c.weight   = 100;
            for (uint8_t k = 0; k < MAX_MIX_SOURCES; k++) {
                MixSource& m = c.src[k];
                if ((uint8_t)m.type   > (uint8_t)SourceType::BUTTON)
                    m.type = SourceType::NONE;
                if ((uint8_t)m.target > (uint8_t)MixTarget::HIGH)
                    m.target = MixTarget::HIGH;
                if ((uint8_t)m.btn_mode > (uint8_t)ButtonMode::STICKY)
                    m.btn_mode = ButtonMode::MOMENTARY;
            }
        }
    }
};
