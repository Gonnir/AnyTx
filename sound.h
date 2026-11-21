#pragma once
#include <Arduino.h>
#include "config.h"

// ═══════════════════════════════════════════════════════════════════════════════
//  DFPlayer voice announcements
//
//  Same module, same wiring convention, and the same SD card as the OpenI6X
//  firmware's DFPlayer mod (github.com/OpenI6X/opentx, see wiki "Modifications
//  > DFPlayer - Voice announcements"): a DFPlayer Mini-compatible board with
//  its files copied to the SD card's root, numbered so the copy order matches
//  the numeric prefix - the DFPlayer plays "track N" as the Nth file written
//  to the card, not by parsing the filename, so getting that order right is
//  what actually matters, the prefix is just there to keep humans (and file
//  managers that re-sort on copy) from scrambling it by accident.
//
//  OpenI6X reserves nine slots for user-recorded phrases:
//      0271_custm1.wav .. 0279_custm9.wav
//  TinyTx uses four of those (see the SND_* constants in config.h) for
//  events OpenI6X's own stock pack has no phrase for (bind, saved, mode
//  changed, calibration); everything else - startup, link up/lost, the
//  throttle-not-idle warning, and link-quality tiers - plays the matching
//  file straight out of that stock pack, confirmed against its actual file
//  listing rather than guessed. This driver plays either the same way, by
//  track number, so an SD card already set up for an OpenI6X radio works
//  here unmodified.
//
//  Only a one-way link is used, exactly like OpenI6X: commands go out over
//  UART, and the module's BUSY output is read back on a plain GPIO pin
//  (LOW while a track is playing) since it responds far faster than reading
//  UART replies and needs no serial RX wiring at all.
// ═══════════════════════════════════════════════════════════════════════════════

class DFPlayerVoice {
public:
    void begin(bool enabled, uint8_t volume) {
        _enabled = enabled;
        _volume  = constrain((int)volume, 0, 30);

        pinMode(PIN_DFPLAYER_BUSY, INPUT_PULLUP);

        Serial2.setTX(PIN_DFPLAYER_TX);
        Serial2.setRX(PIN_DFPLAYER_RX);   // unused, but the UART needs a pin assigned
        Serial2.begin(DFPLAYER_BAUD);

        // The module needs time to mount its SD card before it will reliably
        // obey commands; queue the reset/volume/first-track sequence rather
        // than firing it immediately into a module that isn't listening yet.
        _boot_at = millis();
        _state   = State::BOOTING;
    }

    void setEnabled(bool en) { _enabled = en; }
    bool enabled() const     { return _enabled; }

    void setVolume(uint8_t v) {
        uint8_t nv = (uint8_t)constrain((int)v, 0, 30);
        // Skip the command if nothing actually changed - matters a lot once a
        // Special Function ties this continuously to a live axis position,
        // where it would otherwise fire every frame.
        if (nv == _volume && _state == State::READY) return;
        _volume = nv;
        if (_state == State::READY) sendCmd(0x06, _volume);
    }
    uint8_t volume() const { return _volume; }

    // Play track N, exactly as it would be addressed on an OpenI6X SD card:
    // the Nth file by copy order, 1-based. Ignored while disabled. Queues
    // up to a few tracks (e.g. a spoken number followed by "minutes") and
    // plays them back to back; if the queue is already full the request is
    // just dropped - voice prompts are advisory, not something worth ever
    // blocking on.
    void play(uint16_t track) {
        if (!_enabled || track == 0) return;
        uint8_t next = (uint8_t)((_q_tail + 1) % QUEUE_LEN);
        if (next == _q_head) return;   // full - drop rather than stall
        _queue[_q_tail] = track;
        _q_tail = next;
    }

    bool isBusy() const { return digitalRead(PIN_DFPLAYER_BUSY) == LOW; }

    // Ask for the module's settings to be re-sent shortly (volume), without
    // interrupting anything. This is a one-way link, so a module that browned
    // out and restarted - e.g. when a controller is plugged in and its start-up
    // current pulls the 5V rail down - would otherwise come back at its own
    // default (maximum) volume and TinyTx would never know. Called on every
    // controller connection.
    void refresh() { _refresh_at = millis() + REFRESH_DELAY_MS; _refresh_due = true; }

    void update() {
        uint32_t now = millis();
        switch (_state) {
            case State::BOOTING:
                if (now - _boot_at >= BOOT_DELAY_MS) {
                    sendCmd(0x0C, 0);              // reset
                    _reset_at = now;
                    _state    = State::RESETTING;
                }
                break;

            case State::RESETTING:
                if (now - _reset_at >= RESET_DELAY_MS) {
                    sendCmd(0x09, 0x0002);         // select TF/SD card
                    sendCmd(0x06, _volume);
                    _state = State::READY;
                }
                break;

            case State::READY:
                if (isBusy()) { _busy_seen = true; _awaiting_busy = false; _retries = 0; }

                // The module acknowledges a play command by pulling BUSY low. If it
                // never does (and BUSY has worked before, so it is wired), the
                // command was lost or the module has restarted: bring it up again
                // from scratch and retry the track, up to twice.
                if (_awaiting_busy && _busy_seen && now - _play_sent_at >= BUSY_TIMEOUT_MS) {
                    _awaiting_busy = false;
                    if (_retries < 2) {
                        _retries++;
                        _q_head = (uint8_t)((_q_head + QUEUE_LEN - 1) % QUEUE_LEN);   // put the track back
                        _boot_at = now;
                        _state   = State::BOOTING;
                        break;
                    }
                    _retries = 0;
                }

                if (_refresh_due && (int32_t)(now - _refresh_at) >= 0) {
                    _refresh_due = false;
                    sendCmd(0x06, _volume);
                    _cmd_at = now;
                }

                // Next track only once the previous command has had time to take
                // effect: BUSY lags the command by a few tens of ms, and a second
                // command sent inside that gap cuts the first one off mid-start,
                // which is heard as a click or noise instead of the sound.
                if (_q_head != _q_tail && !isBusy() && !_awaiting_busy &&
                    now - _cmd_at >= MIN_CMD_GAP_MS) {
                    sendCmd(0x03, _queue[_q_head]);   // "specify playback" - play track N
                    _q_head = (uint8_t)((_q_head + 1) % QUEUE_LEN);
                    _play_sent_at = now;
                    _cmd_at       = now;
                    _awaiting_busy = _busy_seen;
                }
                break;
        }
    }

private:
    enum class State { BOOTING, RESETTING, READY };

    void sendCmd(uint8_t cmd, uint16_t param) {
        uint8_t p_h = (uint8_t)(param >> 8), p_l = (uint8_t)param;
        // Feedback byte is 0 (no ACK requested) - matches OpenI6X's one-way
        // wiring, which never reads the module's UART TX at all.
        uint16_t sum = 0xFF + 0x06 + cmd + 0x00 + p_h + p_l;
        uint16_t chk = (uint16_t)(0 - sum);
        uint8_t frame[10] = {
            0x7E, 0xFF, 0x06, cmd, 0x00, p_h, p_l,
            (uint8_t)(chk >> 8), (uint8_t)chk, 0xEF
        };
        Serial2.write(frame, sizeof(frame));
    }

    bool     _enabled = true;
    uint8_t  _volume  = 20;
    State    _state   = State::BOOTING;
    uint32_t _boot_at = 0, _reset_at = 0;
    static const uint32_t BOOT_DELAY_MS  = 400;    // let the module's own power-on settle
    static const uint32_t RESET_DELAY_MS = 2200;   // SD card mount time after a reset

    static const uint32_t MIN_CMD_GAP_MS   = 150;   // between any two commands
    static const uint32_t BUSY_TIMEOUT_MS  = 700;   // play sent, BUSY still high
    static const uint32_t REFRESH_DELAY_MS = 800;   // after a controller connects
    uint32_t _play_sent_at = 0, _cmd_at = 0, _refresh_at = 0;
    bool     _awaiting_busy = false, _busy_seen = false, _refresh_due = false;
    uint8_t  _retries = 0;

    static const uint8_t QUEUE_LEN = 4;
    uint16_t _queue[QUEUE_LEN] = {};
    uint8_t  _q_head = 0, _q_tail = 0;
};
