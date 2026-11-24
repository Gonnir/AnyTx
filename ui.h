#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include "oled_async.h"
#include "config.h"
#include "config_manager.h"
#include "crsf.h"
#include "input.h"
#include "teach.h"
#include "profiles.h"
#include "sound.h"

// ─── MENU PAGES ──────────────────────────────────────────────────────────────
enum class Page : uint8_t {
    STATUS = 0,
    MAIN_MENU,
    MODE_SELECT,
    MODE_CONFIRM,
    CHANNEL_LIST,
    CHANNEL_EDIT,     // base position + 3 mix slots
    SLOT_EDIT,        // one slot: source, weight, target, mode
    LEARN_WAIT,       // "move an axis or press a button"
    LEARN_SAVED,
    CHANNEL_CLEAR,
    CURVE_EDIT,       // 5-point curve editor
    TRIM_EDIT,        // trim value, trim buttons and step size
    CAL_MENU,         // calibration + deadband
    CAL_SWEEP,        // move everything to the extremes
    CAL_CENTER,       // let go, capture neutral
    SAFETY,           // startup throttle check
    FS_EDIT,          // per-channel failsafe
    FS_CAPTURE_ALL,   // snapshot every channel into failsafe
    PROFILES,
    PROFILE_ACTION,   // load or delete the selected profile
    PROFILE_DELETE,
    PROFILE_RENAME,   // edit the selected profile's name
    ELRS_LIST,        // dynamic - shows whatever the connected module reports
    ELRS_VALUE,       // TEXT_SELECTION: cycle through the module's own option list
    ELRS_NUM_EDIT,    // UINT8/INT8/UINT16/INT16/UINT32/INT32/FLOAT
    ELRS_STR_EDIT,    // STRING: character-wheel text entry
    ELRS_COMMAND,     // yes/no confirm, then live status while it runs
    ELRS_INFO,        // read-only INFO field
    LINK_SETUP,       // CRSF line polarity / half-duplex
    SAVE_CONFIRM,
    INPUT_INFO,       // diagnostics: live axis/button readout
    SOUND_LIST,       // Sound Setup top menu: Events / Triggers
    SOUND_EVENTS,     // list of built-in voice events
    SOUND_EVENT_EDIT, // one event's track number, previewed live
    SOUND_TRIG_LIST,  // list of button/channel sound triggers
    SOUND_TRIG_EDIT,  // one trigger: source, threshold/direction, track
    SF_HOME,          // Special Functions: "Timers" / "Functions"
    TIMER_LIST,       // create/delete/edit timer objects
    TIMER_EDIT,
    SF_LIST,          // create/delete/edit function rows
    SF_EDIT,          // one row: trigger, active/inactive, function, values
    SF_FUNC_SELECT,   // pick the Function-row's SFType from a list
    SF_DELETE_CONFIRM,
    TIMER_DELETE_CONFIRM,
    LINK_WIRE_SELECT, // pick the CRSF wiring mode from a list
    SAFETY_SUB,       // Settings > Safety
    CONTROL_MENU,     // Control Setup: Channels / Special Func / Calibration
};

// Four physical buttons only. "ok" and "back" each fire as a short-press
// event on release; if held past LONG_PRESS_MS they instead fire the
// matching *_long event once (while still held) and the short-press event
// is suppressed for that hold. Screens that need more than "select" and
// "back" use the long-press as a secondary action (edit / clear / confirm)
// or fall back to a dedicated yes/no dialog screen.
struct JoyEvent {
    bool up, down;          // navigate / adjust, with auto-repeat while held
    bool ok, ok_long;       // select, confirm, enter edit mode / secondary action
    bool back, back_long;   // cancel, back out / secondary action
    bool save;              // OK + BACK held together: save into the current profile
};

// ─── BUTTONS ─────────────────────────────────────────────────────────────────
class Joystick {
public:
    static const uint32_t LONG_PRESS_MS = 550;
    static const uint32_t CHORD_MS      = 400;   // shorter than LONG_PRESS_MS on purpose

    void begin() {
        const uint8_t pins[] = { PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_OK, PIN_BTN_BACK };
        for (uint8_t p : pins) pinMode(p, INPUT_PULLUP);
    }

    JoyEvent read() {
        uint32_t now = millis();
        JoyEvent e = {};

        uint8_t raw = 0;
        if (!digitalRead(PIN_BTN_UP))   raw |= 0x01;
        if (!digitalRead(PIN_BTN_DOWN)) raw |= 0x02;
        if (!digitalRead(PIN_BTN_OK))   raw |= 0x04;
        if (!digitalRead(PIN_BTN_BACK)) raw |= 0x08;

        if (raw != _unstable) { _unstable = raw; _debounce = now; }
        if (now - _debounce < 40) return e;

        uint8_t prev   = _stable;
        _stable        = raw;
        uint8_t rising = _stable & ~prev;
        uint8_t falling = prev & ~_stable;

        // auto-repeat on up/down while held
        uint8_t ud = _stable & 0x03;
        if (ud) {
            if (!_rep && now - _hold_start >= 450)      { rising |= ud; _rep = true; _rep_t = now; }
            else if (_rep && now - _rep_t >= 110)       { rising |= ud; _rep_t = now; }
        } else { _rep = false; _hold_start = now; }

        e.up    = rising & 0x01;
        e.down  = rising & 0x02;

        // OK: press-start / long-hold / short-release bookkeeping
        // OK + BACK held together = "save", emitted once after CHORD_MS. While
        // both are down (and until both are released) their own events are
        // swallowed so the chord never also acts as a tap or a long-press.
        bool both = (_stable & 0x0C) == 0x0C;
        if (both && (prev & 0x0C) != 0x0C) { _chord_t0 = now; _chord_fired = false; _chord_mute = true; }
        if (both && !_chord_fired && now - _chord_t0 >= CHORD_MS) { e.save = true; _chord_fired = true; }

        if (rising & 0x04)  { _ok_t0 = now; _ok_long_fired = false; }
        if ((_stable & 0x04) && !_ok_long_fired && now - _ok_t0 >= LONG_PRESS_MS) {
            e.ok_long = true; _ok_long_fired = true;
        }
        if ((falling & 0x04) && !_ok_long_fired) e.ok = true;

        // BACK: same pattern
        if (rising & 0x08)  { _back_t0 = now; _back_long_fired = false; }
        if ((_stable & 0x08) && !_back_long_fired && now - _back_t0 >= LONG_PRESS_MS) {
            e.back_long = true; _back_long_fired = true;
        }
        if ((falling & 0x08) && !_back_long_fired) e.back = true;

        if (_chord_mute) {
            e.ok = e.ok_long = e.back = e.back_long = false;
            if ((_stable & 0x0C) == 0) _chord_mute = false;
        }
        return e;
    }

private:
    uint8_t  _unstable = 0, _stable = 0;
    uint32_t _debounce = 0, _hold_start = 0, _rep_t = 0;
    bool     _rep = false;
    uint32_t _ok_t0 = 0, _back_t0 = 0;
    bool     _ok_long_fired = false, _back_long_fired = false;
    uint32_t _chord_t0 = 0;
    bool     _chord_fired = false, _chord_mute = false;
};

// ─── UI ENGINE ───────────────────────────────────────────────────────────────
class UIEngine {
public:
    TinyOled disp{OLED_WIDTH, OLED_HEIGHT, &Wire, -1};

    // Returns false if the OLED never ACKed on either address after all
    // retries - the caller can flag that visibly (e.g. on the status LED),
    // since with no screen there is otherwise no way to tell.
    bool begin(ConfigManager* cfg, CRSFEngine* crsf, InputManager* input,
               ProfileStore* profiles, ChannelMixer* mixer, DFPlayerVoice* voice);
    void setDevice(uint16_t vid, uint16_t pid) { _vid = vid; _pid = pid; }
    // The name of whichever profile is currently loaded - shown on the main
    // Status page the way EdgeTX shows the active model's name. Pass
    // nullptr (or an empty string) when no saved profile matched, e.g. a
    // brand new controller that just got channel defaults instead.
    // Which saved profile (vid/pid key) is currently "the one you're on":
    // OK+BACK saves back into it. vid 0 / pid 0 with no name = none.
    void setActiveProfile(uint16_t vid, uint16_t pid, const char* name) {
        _active_valid = (name && *name);
        _active_vid = vid; _active_pid = pid;
        setModelName(name);
        // Remember it across power cycles (written to flash lazily, like any
        // other setting change - never directly from here).
        if (_cfg && (_cfg->cfg.last_profile_valid != _active_valid ||
                     (_active_valid && (_cfg->cfg.last_profile_vid != vid || _cfg->cfg.last_profile_pid != pid)))) {
            _cfg->cfg.last_profile_valid = _active_valid;
            _cfg->cfg.last_profile_vid   = _active_valid ? vid : 0;
            _cfg->cfg.last_profile_pid   = _active_valid ? pid : 0;
            _cfg->markDirty();
        }
    }
    void setModelName(const char* n) {
        if (n && *n) { strncpy(_model_name, n, sizeof(_model_name) - 1); _model_name[sizeof(_model_name) - 1] = 0; }
        else           _model_name[0] = 0;
    }
    // ── AnyTxCompanion (serial settings link, see companion.h) ─────────────
    bool activeProfile(uint16_t& vid, uint16_t& pid) const {
        vid = _active_vid; pid = _active_pid; return _active_valid;
    }
    // Same as Menu > Profiles > Load, minus the Model ID push: the UART is
    // busy carrying the companion link, the module gets it on reconnect.
    bool companionLoadProfile(uint16_t vid, uint16_t pid) {
        if (!_prof) return false;
        char nm[PROFILE_NAME_LEN] = {};
        if (!_prof->load(vid, pid, _cfg->cfg.channels, _cfg->cfg.cal, &_cfg->cfg.model_id,
                         nm, sizeof(nm), _cfg->cfg.sf, _cfg->cfg.timers,
                         _cfg->cfg.timer_minute_call, &_cfg->cfg.trim_sound_off)) return false;
        resetTimers();
        _cfg->markDirty();
        refreshProfiles();
        setActiveProfile(vid, pid, nm);
        return true;
    }
    // Same as holding OK+BACK: live settings into the active profile.
    bool companionSaveActive() { saveActiveProfile(); return _active_valid; }
    // Call after profiles were created/renamed/deleted behind the menu's back.
    void companionProfilesChanged() {
        refreshProfiles();
        if (!_active_valid) return;
        for (uint8_t i = 0; i < _pcount; i++)
            if (_plist[i].vid == _active_vid && _plist[i].pid == _active_pid) {
                setActiveProfile(_active_vid, _active_pid, _plist[i].name);
                return;
            }
        setActiveProfile(0, 0, nullptr);   // the active one was deleted
    }
    // Set while the startup throttle check is still blocking output.
    void setThrottleWarn(bool w) { _thr_warn = w; }
    void setTxCut(bool c)        { _tx_cut = c; }
    void setSafeMode(bool m)     { _safe_mode = m; }
    // Called right after mixing, before the channels go out: forces any
    // OVERRIDE_CHANNEL special function's target channel while its trigger
    // is active. Needs the same input snapshot the rest of the frame uses.
    void applyOverrides(uint16_t ch[CRSF_NUM_CHANNELS], const InputState& in);
    void update(const uint16_t ch_out[CRSF_NUM_CHANNELS], const InputState& in);
    // False if disp.begin() never ACKed on either 0x3C or 0x3D. oledAddr()
    // reports whichever address it last tried/settled on either way, mainly
    // for the diagnostics page.
    bool    oledOk() const   { return _oled_ok; }
    uint8_t oledAddr() const { return _oled_addr; }

private:
    Joystick       _joy;
    bool           _oled_ok   = false;
    uint8_t        _oled_addr = 0;
    ConfigManager* _cfg   = nullptr;
    CRSFEngine*    _crsf  = nullptr;
    InputManager*  _input = nullptr;
    ProfileStore*  _prof  = nullptr;
    ChannelMixer*  _mixer = nullptr;
    DFPlayerVoice* _voice = nullptr;
    bool           _voice_link_was = false;   // for the link-up/lost transition chime
    bool           _link_was_found = false;   // for resending Model ID on module reconnect

    // Trim max/min/center voice cues - watches every channel's trim value
    // for a transition into +100/-100/0, however it got there.
    bool    _trim_init = false;
    int8_t  _last_trim[CRSF_NUM_CHANNELS] = {};

    // Inactivity alarm - last time any button or axis actually moved, and
    // when the alarm last fired so it can repeat at the configured interval.
    bool     _activity_init   = false;
    uint32_t _last_activity_ms = 0;
    uint32_t _last_inactivity_alarm_ms = 0;
    uint32_t _last_buttons_seen = 0;
    int16_t  _last_axes_seen[MAX_INPUT_AXES] = {};
    bool           _voice_thr_warn_was = false;
    uint8_t        _voice_sig_tier = 0;       // 0 normal, 1 low, 2 critical - with hysteresis
    bool           _trig_was[MAX_SOUND_TRIGGERS] = {};   // edge state per sound trigger slot

    // Sound Setup menu
    uint8_t  _sound_menu_sel  = 0;   // SOUND_LIST: 0 = Events, 1 = Triggers
    uint8_t  _sound_event_sel = 0;   // which SoundEvent is open in SOUND_EVENT_EDIT
    uint8_t  _sound_trig_sel  = 0;   // which triggers[] slot is open in SOUND_TRIG_EDIT

    // Special Functions
    uint8_t  _sf_sel    = 0;         // which sf[] slot SF_EDIT is working on
    uint8_t  _timer_sel = 0;         // which timers[] slot TIMER_EDIT is working on
    bool     _sf_was[MAX_SPECIAL_FUNCS] = {};   // edge state per function row

    // Timers (runtime only - never persisted, always start fresh on power-up)
    struct TimerRT {
        int32_t  value_s  = 0;    // current value, seconds
        uint32_t accum_ms = 0;    // sub-second accumulator
        bool     running  = false;
        uint16_t last_min = 0;    // last elapsed minute a count-up timer announced
    };
    TimerRT  _timer_rt[MAX_TIMERS];
    uint32_t _timer_last_ms = 0;
    uint8_t  _status_view   = 0;    // STATUS screen: 0 = channel bars, else 1-based timer slot
    uint16_t       _vid = 0, _pid = 0;      // currently attached USB device
    char           _model_name[PROFILE_NAME_LEN] = {};   // see setModelName()
    bool           _active_valid = false;                // see setActiveProfile()
    uint16_t       _active_vid = 0, _active_pid = 0;
    uint32_t       _toast_until = 0;                     // "saved" popup, see saveActiveProfile()
    char           _toast_l1[22] = {}, _toast_l2[22] = {};
    void saveActiveProfile();
public:
    // Re-arm every timer's running value from the current config (countdowns
    // back to their start time, count-ups to zero, all stopped). Called after a
    // profile brings its own timers in.
    void resetTimers() {
        for (uint8_t i = 0; i < MAX_TIMERS; i++) {
            _timer_rt[i] = TimerRT{};
            if (_cfg->cfg.timers[i].enabled && _cfg->cfg.timers[i].kind == TimerKind::COUNTDOWN)
                _timer_rt[i].value_s = _cfg->cfg.timers[i].start_s;
        }
    }
private:
    void drawToast();
    void adoptProfile(uint16_t vid, uint16_t pid);

    uint8_t     _slot = 0;                  // mix slot being edited
    bool        _thr_warn = false;
    bool        _tx_cut   = false;
    bool        _safe_mode = false;
    int16_t     _cmin[MAX_INPUT_AXES] = {}; // calibration sweep extremes
    int16_t     _cmax[MAX_INPUT_AXES] = {};
    uint8_t     _cpt = 0;                   // curve point being edited
    uint8_t     _psel = 0;                  // profile selected from the list

    // What the learn screen is currently binding: a mix slot, or one of the
    // two trim buttons.
    enum LearnWhat : uint8_t { LEARN_SLOT = 0, LEARN_TRIM_UP, LEARN_TRIM_DN, LEARN_SOUND_TRIG, LEARN_SF_TRIG };
    uint8_t     _learn_what = LEARN_SLOT;
    ProfileInfo _plist[MAX_PROFILES];
    uint8_t     _pcount = 0;

    // Set while a screen has "opened" the currently selected row for
    // adjustment with a ranged value (percentages, milliseconds, counts).
    // Entered with a short OK press on such a row; UP/DOWN then change the
    // value instead of moving the cursor, and OK or BACK closes it again.
    // Cleared automatically on every page change.
    bool     _editing = false;

    Page     _page      = Page::STATUS;
    Page     _last_page = Page::STATUS;   // used to clear _editing on any page change
    uint8_t  _cursor = 0;
    uint8_t  _sel_ch = 0;
    uint8_t  _yesno  = 0;
    uint8_t  _val    = 0;
    uint32_t _saved_at = 0;
    uint32_t _last_draw = 0;

    // ELRS Setup: which live field is open, and the folder-navigation stack
    // to get back out of it. _elrs_folder is the field_id of the folder
    // currently being browsed (0 = root, i.e. the top of the module's menu).
    uint8_t  _elrs_field  = 0;
    uint8_t  _elrs_folder = 0;
    uint8_t  _elrs_stack[4]        = {};
    uint8_t  _elrs_stack_cursor[4] = {};
    uint8_t  _elrs_stack_depth     = 0;
    int32_t  _num_val   = 0;              // working value, ELRS_NUM_EDIT
    char     _str_buf[17] = {};           // working text, ELRS_STR_EDIT
    uint8_t  _str_cursor  = 0;
    bool     _cmd_running = false;        // ELRS_COMMAND: confirming vs. showing live status
    uint8_t  _cmd_last_status = 0;        // for spotting the moment a command reaches "confirmed"

    TeachDetector _teach;
    bool          _learn_rev  = false;
    ButtonMode    _learn_mode = ButtonMode::MOMENTARY;

    const uint16_t* _live = nullptr;
    InputState      _in;

    // draw
    void drawStatus();      void drawMainMenu();
    // STATUS carousel page 0: EdgeTX-style main screen - model name, first
    // timer, four trims and two Mode 2 stick boxes, laid out in absolute
    // pixels to match the reference screenshot. No footer/disp.display() of
    // its own, same convention as drawDiagContent().
    void drawStatusMain();
    // Widgets for drawStatusMain(): 23x23 stick box with a ring marker, and
    // 43 px horizontal / vertical trim scales with a sliding grip marker.
    void drawStickBox(int16_t x, int16_t y, uint8_t ch_x, uint8_t ch_y);
    void drawTrimH(int16_t x0, int16_t y, int8_t trim);
    void drawTrimV(int16_t x, int16_t y0, int8_t trim);
    void drawModeSelect();  void drawModeConfirm();
    void drawChannelList(); void drawChannelEdit();
    void drawSlotEdit();    void drawLearnWait();
    void drawLearnSaved();  void drawChannelClear();
    void drawProfiles();    void drawProfileDelete(); void drawProfileRename();
    void drawProfileAction();
    void drawFsEdit();      void drawFsCaptureAll();
    void drawCalMenu();     void drawCalSweep();
    void drawCalCenter();   void drawSafety(); void drawSafetySub(); void drawControlMenu();
    void drawThrottleWarn(); void drawCurveEdit();
    void drawTrimEdit();
    void drawElrsList();    void drawElrsValue();
    void drawElrsCommand(); void drawSaveConfirm();
    void drawElrsNumEdit(); void drawElrsStrEdit();
    void drawElrsInfo();
    void drawLinkSetup();
    void drawInputInfo();
    void drawSoundList();     void drawSoundEvents();
    void drawSoundEventEdit(); void drawSoundTrigList();
    void drawSoundTrigEdit();
    void drawSFList(); void drawSFEdit();
    void drawSFDeleteConfirm(); void drawTimerDeleteConfirm();
    void drawSFHome(); void drawTimerList(); void drawTimerEdit();
    void drawSFFuncSelect();
    void drawLinkWireSelect();

    // handle
    void hStatus(const JoyEvent&);      void hMainMenu(const JoyEvent&);
    void hModeSelect(const JoyEvent&);  void hModeConfirm(const JoyEvent&);
    void hChannelList(const JoyEvent&); void hChannelEdit(const JoyEvent&);
    void hSlotEdit(const JoyEvent&);    void hLearnWait(const JoyEvent&);
    void hChannelClear(const JoyEvent&);void hElrsList(const JoyEvent&);
    void hProfiles(const JoyEvent&);    void hProfileDelete(const JoyEvent&); void hProfileRename(const JoyEvent&);
    void hProfileAction(const JoyEvent&);
    void hFsEdit(const JoyEvent&);      void hFsCaptureAll(const JoyEvent&);
    void hCalMenu(const JoyEvent&);     void hCalSweep(const JoyEvent&);
    void hCalCenter(const JoyEvent&);   void hSafety(const JoyEvent&); void hSafetySub(const JoyEvent&); void hControlMenu(const JoyEvent&);
    void hCurveEdit(const JoyEvent&);   void hTrimEdit(const JoyEvent&);
    void hElrsValue(const JoyEvent&);   void hElrsCommand(const JoyEvent&);
    void hElrsNumEdit(const JoyEvent&); void hElrsStrEdit(const JoyEvent&);
    void hElrsInfo(const JoyEvent&);
    void hSaveConfirm(const JoyEvent&); void hInputInfo(const JoyEvent&);
    void hLinkSetup(const JoyEvent&);
    void hSoundList(const JoyEvent&);     void hSoundEvents(const JoyEvent&);
    void hSoundEventEdit(const JoyEvent&); void hSoundTrigList(const JoyEvent&);
    void hSoundTrigEdit(const JoyEvent&);
    void hSFList(const JoyEvent&); void hSFEdit(const JoyEvent&);
    void hSFDeleteConfirm(const JoyEvent&); void hTimerDeleteConfirm(const JoyEvent&);
    void hSFHome(const JoyEvent&); void hTimerList(const JoyEvent&); void hTimerEdit(const JoyEvent&);
    void hSFFuncSelect(const JoyEvent&);
    void hLinkWireSelect(const JoyEvent&);

    // helpers
    void        header(const char* t);    // Bottom row, three zones: what OK does on the left, a long-press-OK
    // hint (edit / clear / secondary action) in the middle, BACK on the
    // right. No button names - the position says which is which. Any zone
    // can be left empty when that key does nothing here.
    void        footer(const char* ok_fn,
                       const char* back_fn = nullptr,
                       const char* hold_fn = nullptr);
    void        headerBar(float v);   // live bar inside the header strip

    // Move a list cursor, wrapping at both ends: up from the first item lands
    // on the last, down from the last returns to the first. Saves scrolling
    // the whole way back on a sixteen-channel list.
    static void wrapCursor(uint8_t& cur, uint8_t count, bool up, bool down) {
        if (!count) return;
        if (up)   cur = (cur == 0)         ? (uint8_t)(count - 1) : (uint8_t)(cur - 1);
        if (down) cur = (cur >= count - 1) ? 0                    : (uint8_t)(cur + 1);
    }
    void        row(uint8_t r, bool sel, const char* text);
    void        bar(int16_t x, int16_t y, int16_t w, int16_t h, float v, uint16_t col);
    void        yesNo(uint8_t sel);
    void        scrollbar(uint8_t total, uint8_t vis, uint8_t scroll);
    // Live children of a folder, in discovery order, skipping hidden fields.
    // Returns how many field_ids were written into `out` (capped at `max`).
    uint8_t     elrsChildren(uint8_t folder, uint8_t* out, uint8_t max);
    // Plays event `se` using whatever track config has it assigned to right
    // now (0 = disabled). Every voice trigger in the UI goes through this,
    // never a raw track number, so Menu > Sound Setup's edits take effect
    // immediately with no other code to touch.
    void        playEvent(SoundEvent se) {
        if (_voice && se < SOUND_EVENT_COUNT) {
            uint16_t t = _cfg->cfg.sound.event_track[se];
            if (t) _voice->play(t);
        }
    }
    // Fires whichever configured button/axis triggers just became active,
    // edge-triggered so a held button or a value sitting past its threshold
    // doesn't repeat every frame.
    void        checkSoundTriggers(const InputState& in);
    // Watches every channel's trim for a transition into max/min/center.
    void        checkTrimEvents();
    // Tracks button/axis activity and fires the inactivity alarm on schedule.
    void        checkInactivity(const InputState& in, uint32_t now);
    // Level (not edge) state of a special function's trigger - "Active".
    bool        sfTriggerActive(const SpecialFunction& f, const InputState& in, const uint16_t* chv = nullptr) const;
    // Does the trigger's current state match what this row is set to fire on
    // (Active or Inactive)?
    bool        sfConditionMet(const SpecialFunction& f, const InputState& in, const uint16_t* chv = nullptr) const;
    // Which channels (bit i = channel i) an Override Channel row is forcing
    // right now - drives the "o" marker under a bar on the Status screen.
    uint16_t    overriddenChannelMask() const;
    // Steps every non-override function row (edge-triggered) each frame.
    // Override Channel is handled separately, continuously, in applyOverrides().
    void        tickSpecialFunctions(const InputState& in);
    void        tickTimers(uint32_t now);
    // How many timers[] slots are in use, and their slot indices - what
    // STATUS's UP/DOWN cycles through.
    uint8_t     timerSlots(uint8_t* out, uint8_t max) const;
    // How many diagnostics pages exist right now (USB info, raw report,
    // then one page per 4 axes) - also part of STATUS's UP/DOWN carousel.
    uint8_t     diagPageCount() const;
    // Draws one diagnostics page's header+body only - no footer, no
    // disp.display(). Shared by the standalone Input Info menu page and the
    // inline copy shown in STATUS's carousel.
    void        drawDiagContent(uint8_t page);
    const char* optionAt(const char* list, uint8_t idx, char* buf, uint8_t bufsz);
    void        slotLabel(const MixSource& m, char* buf, uint8_t sz);
    void        chanLabel(const ChannelConfig& c, char* buf, uint8_t sz);
    void        refreshProfiles();
    // Finds the first unused named-slot number and saves the current
    // channel mix/calibration there - "Save as new slot" in Menu > Profiles.
    void        saveAsNewSlot();
    void        captureFailsafe(bool all);
};
