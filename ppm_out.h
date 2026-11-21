#pragma once
#include <Arduino.h>
#include <hardware/gpio.h>
#include <pico/time.h>
#include "config.h"

// ═══════════════════════════════════════════════════════════════════════════════
//  PPM output
//
//  Drives a standard PPM sum signal so the board can feed another radio's
//  trainer port instead of an ELRS module.
//
//  Frame layout, repeated every frame_us:
//      [pulse][gap][pulse][gap] ... [pulse][sync gap]
//  Each channel occupies pulse + gap = 1000..2000 us, and the sync gap pads
//  the frame out to its full length so the receiving radio can find the start.
//
//  Timing comes from a hardware alarm, not the main loop, so jitter stays in
//  the microsecond range regardless of what else the firmware is doing.
// ═══════════════════════════════════════════════════════════════════════════════

class PPMOut {
public:
    void begin(uint8_t pin, const PPMConfig& cfg) {
        end();
        _pin = pin;

        _num    = constrain((int)cfg.channels, 2, PPM_OUT_MAX_CH);
        _frame  = constrain((int)cfg.frame_us, 5000, 40000);
        _pulse  = constrain((int)cfg.pulse_us,  100,  1000);
        _invert = cfg.invert;

        for (uint8_t i = 0; i < PPM_OUT_MAX_CH; i++) _us[i] = _pending[i] = 1500;

        gpio_init(_pin);
        gpio_set_dir(_pin, GPIO_OUT);
        idle();

        _self    = this;
        _phase   = 0;
        _idx     = 0;
        _running = true;
        _alarm   = add_alarm_in_us(1000, &PPMOut::trampoline, this, true);
    }

    void end() {
        if (!_running) return;
        _running = false;
        if (_alarm > 0) cancel_alarm(_alarm);
        _alarm = 0;
        idle();
    }

    bool running() const { return _running; }

    // Channel values in CRSF units; converted to microseconds here.
    void setChannels(const uint16_t ch[CRSF_NUM_CHANNELS]) {
        for (uint8_t i = 0; i < _num && i < CRSF_NUM_CHANNELS; i++) {
            int32_t us = map((int32_t)ch[i], CRSF_CHANNEL_MIN, CRSF_CHANNEL_MAX, 1000, 2000);
            _pending[i] = (uint16_t)constrain(us, 800, 2200);
        }
        _dirty = true;
    }

    // Stop pulsing without tearing the object down, so link loss can silence
    // the trainer output the same way it silences CRSF.
    void setEnabled(bool en) {
        _enabled = en;
        if (!en) idle();
    }

private:
    static PPMOut* _self;

    uint8_t  _pin   = 0;
    uint8_t  _num   = 8;
    uint16_t _frame = 22500;
    uint16_t _pulse = 300;
    bool     _invert = false;

    volatile uint16_t _us[PPM_OUT_MAX_CH]      = {};
    volatile uint16_t _pending[PPM_OUT_MAX_CH] = {};
    volatile bool     _dirty   = false;
    volatile bool     _enabled = true;
    volatile bool     _running = false;

    volatile uint8_t  _phase = 0;   // 0 = emit pulse, 1 = emit gap
    volatile uint8_t  _idx   = 0;
    volatile uint32_t _spent = 0;   // microseconds used so far this frame
    alarm_id_t        _alarm = 0;

    inline void idle()   { gpio_put(_pin, _invert ? 1 : 0); }
    inline void active() { gpio_put(_pin, _invert ? 0 : 1); }

    static int64_t trampoline(alarm_id_t, void* ud) {
        return ((PPMOut*)ud)->tick();
    }

    // Runs in interrupt context - keep it short.
    // Returning a negative value reschedules relative to the previous alarm,
    // which stops timing drift accumulating frame to frame.
    int64_t tick() {
        if (!_running) return 0;

        if (!_enabled) {                 // silent, but keep the timer alive
            idle();
            return -(int64_t)_frame;
        }

        if (_phase == 0) {
            // Start of a channel pulse
            if (_idx == 0 && _dirty) {   // adopt new values at frame start
                for (uint8_t i = 0; i < PPM_OUT_MAX_CH; i++) _us[i] = _pending[i];
                _dirty = false;
            }
            active();
            _phase = 1;
            _spent += _pulse;
            return -(int64_t)_pulse;
        }

        // Gap that completes this channel's slot
        idle();
        uint16_t slot = _us[_idx];
        uint32_t gap  = (slot > _pulse) ? (slot - _pulse) : 100;
        _spent += gap;
        _idx++;

        if (_idx >= _num) {
            // Final sync gap pads the frame to its full length
            _phase = 0;
            _idx   = 0;
            // _spent already includes this gap - adding it again shortened
            // every frame by one channel period, up to about 7% of the frame.
            uint32_t used = _spent;
            uint32_t sync = (_frame > used + 3000) ? (_frame - used) : 3000;
            _spent = 0;
            return -(int64_t)(gap + sync);
        }

        _phase = 0;
        return -(int64_t)gap;
    }
};
