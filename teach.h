#pragma once
#include "config.h"
#include "input.h"

// ═══════════════════════════════════════════════════════════════════════════════
//  Channel learning
//
//  Watches the generic InputState and reports whichever axis was moved the most
//  or whichever button was pressed.  Because every input source is normalised
//  the same way, this works identically for USB gamepads, EdgeTX radios, SBUS,
//  PPM and ELRS receivers.
// ═══════════════════════════════════════════════════════════════════════════════

#define TEACH_AXIS_THRESHOLD  12000   // ~18% of full travel = deliberate movement
#define TEACH_BTN_DEBOUNCE_MS 40

struct TeachResult {
    SourceType type      = SourceType::NONE;
    uint8_t    index     = 0;
    int16_t    seen_min  = 0;
    int16_t    seen_max  = 0;
};

class TeachDetector {
public:
    void begin(const InputState& in) {
        _done   = false;
        _result = TeachResult{};
        for (uint8_t i = 0; i < MAX_INPUT_AXES; i++) {
            _mn[i] = _mx[i] = (i < in.num_axes) ? in.axes[i] : 0;
        }
        _base_btns = in.buttons;
        _btn_cand  = 0;
        _btn_time  = 0;
        _best_idx  = 0;
        _best_span = 0;
    }

    // Returns true once something has been identified.
    bool update(const InputState& in) {
        if (_done) return true;

        // ── Track travel on every axis ──────────────────────────────────────
        for (uint8_t i = 0; i < in.num_axes && i < MAX_INPUT_AXES; i++) {
            int16_t v = in.axes[i];
            if (v < _mn[i]) _mn[i] = v;
            if (v > _mx[i]) _mx[i] = v;
            int32_t span = (int32_t)_mx[i] - _mn[i];
            if (span > _best_span) { _best_span = span; _best_idx = i; }
        }

        // ── Button press wins if one is held (checked first so a stick
        //    resting off-centre doesn't mask a button press) ────────────────
        uint32_t pressed = in.buttons & ~_base_btns;
        if (pressed && _btn_time == 0) {
            _btn_cand = pressed;
            _btn_time = millis();
        }
        if (_btn_time && (millis() - _btn_time >= TEACH_BTN_DEBOUNCE_MS)) {
            if (in.buttons & _btn_cand) {
                uint8_t  id = 0;
                uint32_t m  = _btn_cand;
                while (m && !(m & 1u)) { m >>= 1; id++; }
                _result.type  = SourceType::BUTTON;
                _result.index = id;
                _done = true;
                return true;
            }
            _btn_time = 0;
            _btn_cand = 0;
        }

        // ── Axis detection ──────────────────────────────────────────────────
        if (_best_span >= TEACH_AXIS_THRESHOLD && in.buttons == 0) {
            _result.type     = SourceType::AXIS;
            _result.index    = _best_idx;
            _result.seen_min = _mn[_best_idx];
            _result.seen_max = _mx[_best_idx];
            _done = true;
            return true;
        }
        return false;
    }

    bool               done()   const { return _done; }
    const TeachResult& result() const { return _result; }

    // Live 0..1 bar for the detected (or currently-leading) axis
    float bar(const InputState& in, bool reversed) const {
        uint8_t idx = _done ? _result.index : _best_idx;
        if (idx >= in.num_axes) return 0.5f;
        float v = ((float)in.axes[idx] + 32768.0f) / 65535.0f;
        if (reversed) v = 1.0f - v;
        return constrain(v, 0.0f, 1.0f);
    }

    // Progress 0..1 towards the movement threshold (for the "keep moving" hint)
    float progress() const {
        return constrain((float)_best_span / TEACH_AXIS_THRESHOLD, 0.0f, 1.0f);
    }

private:
    bool        _done = false;
    TeachResult _result;
    int16_t     _mn[MAX_INPUT_AXES] = {}, _mx[MAX_INPUT_AXES] = {};
    int32_t     _best_span = 0;
    uint8_t     _best_idx  = 0;
    uint32_t    _base_btns = 0, _btn_cand = 0, _btn_time = 0;
};
