#pragma once
#include <Arduino.h>
#include "config.h"

// ─── CRSF ADDRESSES ──────────────────────────────────────────────────────────
#define CRSF_ADDR_BROADCAST  0x00
#define CRSF_ADDR_FC         0xC8
#define CRSF_ADDR_HANDSET    0xEA
#define CRSF_ADDR_TX_MODULE  0xEE

// ─── FRAME TYPES ─────────────────────────────────────────────────────────────
#define CRSF_FRAMETYPE_RC_CHANNELS_PACKED  0x16
#define CRSF_FRAMETYPE_LINK_STATISTICS     0x14
#define CRSF_FRAMETYPE_OPENTX_SYNC         0x10
#define CRSF_FRAMETYPE_DEVICE_PING         0x28
#define CRSF_FRAMETYPE_DEVICE_INFO         0x29
#define CRSF_FRAMETYPE_PARAM_SETTINGS_ENTRY 0x2B
#define CRSF_FRAMETYPE_PARAM_READ          0x2C
#define CRSF_FRAMETYPE_PARAM_WRITE         0x2D
#define CRSF_FRAMETYPE_ELRS_STATUS         0x2E
// The module sends this to tell the handset when to transmit. EdgeTX feeds it
// into its module scheduler; without honouring it the radio talks whenever it
// likes and the module's replies collide with the next RC frame.
#define CRSF_FRAMETYPE_RADIO_ID            0x3A
#define CRSF_SUBTYPE_TIMING_CORRECTION     0x10

// ─── ELRS PARAMETER INDICES (legacy hardcoded set - superseded, see below) ───
#define ELRS_PARAM_PACKET_RATE    1
#define ELRS_PARAM_TELEM_RATIO    2
#define ELRS_PARAM_TX_POWER       3
#define ELRS_PARAM_DYNAMIC_POWER  4
#define ELRS_PARAM_BIND           5
#define ELRS_PARAM_WIFI_UPDATE    6

struct LinkStats {
    uint8_t lq         = 0;      // uplink link quality, %
    int8_t  rssi1      = -127;   // uplink RSSI, antenna 1, dBm
    int8_t  rssi2      = -127;   // uplink RSSI, antenna 2, dBm
    int8_t  snr        = 0;      // uplink SNR, dB
    uint8_t antenna    = 0;      // active diversity antenna, 0 or 1
    uint8_t rf_mode    = 0;      // raw index - meaning is target/version specific
    uint8_t tx_pwr_idx = 0;      // raw index - meaning is target/version specific
    uint8_t dl_lq      = 0;      // downlink link quality, %
    int8_t  dl_rssi    = -127;   // downlink RSSI, dBm
    int8_t  dl_snr     = 0;      // downlink SNR, dB
    bool    valid      = false;
};

// ─── LIVE PARAMETER TREE ─────────────────────────────────────────────────────
// A full walk of whatever the connected module actually reports, the same
// way elrs.lua builds its menu: DEVICE_INFO says how many fields exist,
// then each one is fetched with PARAMETER_READ and arrives as a
// PARAMETER_SETTINGS_ENTRY carrying its own id, its parent folder's id, its
// type, and a name - so nothing here is ever guessing which physical
// setting a number refers to. This is what makes it correct across ELRS
// versions even when a firmware update reorders or adds fields, unlike a
// fixed index table.
enum class ParamType : uint8_t {
    P_UINT8 = 0, P_INT8 = 1, P_UINT16 = 2, P_INT16 = 3,
    P_UINT32 = 4, P_INT32 = 5, P_UINT64 = 6, P_INT64 = 7,
    P_FLOAT = 8, P_TEXT_SELECTION = 9, P_STRING = 10,
    P_FOLDER = 11, P_INFO = 12, P_COMMAND = 13, P_VTX = 14,
    P_OUT_OF_RANGE = 127,
};

#define MAX_LIVE_PARAMS 40   // generous headroom over any known target's field count
#define LIVE_NAME_LEN   20
#define LIVE_TEXT_LEN   48   // options string / string value / command status text / units

// One node of the live tree. `text` is reused for whatever the type needs:
// the pipe-of-options for TEXT_SELECTION, the current value for STRING, the
// status message for COMMAND, or an optional units suffix for numeric types.
struct LiveParam {
    uint8_t   field_id   = 0;
    uint8_t   parent     = 0;   // 0 = root
    ParamType type       = ParamType::P_OUT_OF_RANGE;
    bool      hidden     = false;
    char      name[LIVE_NAME_LEN] = {};
    char      text[LIVE_TEXT_LEN] = {};
    int32_t   value = 0, vmin = 0, vmax = 0, step = 0;
    uint8_t   decimals   = 0;   // FLOAT only
    uint8_t   cmd_status = 0;   // COMMAND only: 0 ready,1 start,2 progress,3 confirmed,4 cancelled
};

class CRSFEngine {
public:
    // single_pin: transmit and receive on PIN_CRSF_TX alone, leaving GP1 free.
    // The hardware UART drives the pin; a PIO receiver samples the same pad.
    void begin(HardwareSerial& serial, uint32_t baud,
               bool invert, bool half_duplex, bool single_pin = false);
    void update();

    // Applied immediately, no reboot required
    void setInvert(bool inv);
    void setHalfDuplex(bool hd);

    // Try both line polarities until frames start arriving, then stop. Reading
    // inverted data as plain UART produces framing errors that decode as a
    // flood of 0xFF, so a wrong guess looks like noise rather than an obvious
    // failure - which makes it worth searching for automatically.
    void setAutoPolarity(bool en) { _auto_pol = en; }
    bool polarityFound() const    { return _echo_pass; }
    bool rxInverted()    const    { return _rx_invert; }

    // True once one of our own transmissions has been read back and passed its
    // CRC. That single fact proves the receiver works, the baud rate is right
    // and the receive polarity is right - all without the module having to say
    // anything. If this is true and no module frames ever arrive, the fault is
    // at the module end, not ours.
    bool echoVerified()  const    { return _echo_pass; }
    bool synced()        const    { return _synced; }

    // True once we've transmitted but never had a reply — on a single-wire
    // module with no telemetry return path this stays true forever, which is
    // normal, so the UI shows "TX only" rather than an error.
    bool txOnly() const { return !_device_found && millis() > 4000; }

    void setChannels(const uint16_t ch[CRSF_NUM_CHANNELS]);

    // When disabled, RC channel frames stop going out. The ELRS module sees no
    // input and drops its own transmission, so the receiver registers a real
    // link loss instead of being fed synthetic values.
    void setRCEnabled(bool en) { _rc_enabled = en; }
    bool rcEnabled() const     { return _rc_enabled; }
    void writeParamCommand(uint8_t elrs_idx);

    // ELRS Model ID / Model Match - see the comment on DeviceProfile in
    // config.h for what this does and why it's a CRSF PARAMETER_WRITE with
    // field id 0 rather than a normal named parameter.
    void sendModelId(uint8_t id);
    void pingDevice();

    // Full parameter-tree discovery, the same job elrs.lua does when you
    // open it. Safe to call again any time - e.g. every time the ELRS Setup
    // menu is opened, so it always reflects whatever module is connected
    // right now rather than something cached from an earlier session or an
    // earlier firmware version.
    void            beginDiscovery();
    bool            discoveryBusy()  const { return _disc_active; }
    uint8_t         discoveryTotal() const { return _field_count; }
    uint8_t         discoveryDone()  const { return _disc_field > _field_count ? _field_count : _disc_field; }

    uint8_t          liveCount() const { return _live_count; }
    const LiveParam* liveAt(uint8_t i) const { return (i < _live_count) ? &_live[i] : nullptr; }
    const LiveParam* liveByField(uint8_t field_id) const;

    // Encodes to whatever width/signedness the field's own type calls for
    // and sends it as a standard PARAMETER_WRITE. No-op for types this
    // doesn't apply to (folders, commands, strings, info).
    void writeParamValue(const LiveParam& p, int32_t value);
    void writeParamString(const LiveParam& p, const char* str);

    // ── AnyTxCompanion hooks (see companion.h) ─────────────────────────────
    // The CRSF UART doubles as the settings link when a USB-serial adapter is
    // plugged in where the ELRS module normally goes. The tap sees every
    // received byte so the HELLO handshake can be spotted; a session then
    // takes the UART over completely (plain polarity, TX driven, no RC
    // frames) until endSession().
    void setRxTap(void (*fn)(uint8_t)) { _tap = fn; }
    void beginSession();
    void endSession();
    bool inSession() const { return _suspended; }

    bool        deviceFound() const { return _device_found; }
    const char* deviceName()  const { return _device_name;  }
    LinkStats   linkStats()   const { return _link;         }

    // Raw receive counters. Bytes arriving with no good frames means something
    // is reaching the pin but the polarity or baud rate is wrong; no bytes at
    // all means nothing is getting through electrically.
    uint32_t rxBytes()  const { return _rx_bytes; }
    uint32_t rxFrames() const { return _rx_frames; }
    uint32_t rxBad()    const { return _rx_bad; }
    // Snapshot of the most recent frame that failed its CRC, held until the
    // next failure so it can actually be read on screen. Idle line noise
    // would otherwise overwrite it many times a second.
    const uint8_t* badSnap()    const { return _bad_snap; }
    uint8_t        badCrcCalc() const { return _bad_calc; }
    uint8_t        badCrcRecv() const { return _bad_recv; }
    bool           haveBad()    const { return _have_bad; }
    uint8_t        rxLastByte() const { return _rx_last_b; }
    uint8_t        skipLast() const { return _skip_last; }
    uint8_t        skipCnt()  const { return _skip_cnt; }
    uint8_t        diInfoCount() const { return _di_count; }
    uint8_t        diLen()       const { return _di_len; }
    uint8_t        diLastType()  const { return _last_type; }

private:
    HardwareSerial* _serial = nullptr;
    void (*_tap)(uint8_t) = nullptr;
    bool     _suspended = false;
    bool     _sess_invert = false, _sess_rx_invert = false;
    uint16_t _ch[CRSF_NUM_CHANNELS] = {};
    uint32_t _last_rc = 0, _last_sync = 0, _last_ping = 0, _last_rx = 0;

    static const uint32_t RC_INTERVAL_MS   = 4;   // default until the module says otherwise
    static const uint32_t SYNC_INTERVAL_MS = 200;
    static const uint32_t PING_INTERVAL_MS = 1500;

    bool      _rc_enabled   = true;
    bool      _device_found = false;
    // Transmit and receive polarity are tracked separately. The module beeping
    // in and out proved why: flipping the transmit polarity to hunt for a
    // receive setting tears down a link that was already working, so only the
    // receive side is ever searched.
    bool      _invert       = false;   // transmit, user-controlled
    bool      _rx_invert    = false;   // receive, searched automatically
    bool      _half_duplex  = true;
    bool      _single_pin   = false;
    Stream*   _rx           = nullptr;   // where replies are read from
    void      acquireTX();
    void      releaseTX();
    void      applyOverrides();   // re-assert polarity after any funcsel change

    // Half duplex releases the line only for a short window after each frame,
    // then drives it idle again. A permanently floating pin behaves like an
    // aerial: it collects enough noise to fake start bits, and the receiver
    // turns that into a torrent of junk bytes.
    bool     _released    = false;
    uint32_t _released_at = 0;
    static const uint32_t RX_WINDOW_US = 2200;

    bool     _auto_pol    = false;
    uint32_t _pol_since   = 0;
    static const uint32_t POL_TRY_MS = 1500;

    // Loopback self-test: occasionally let our own transmission through to the
    // parser instead of draining it, and see whether it decodes.
    bool     _selftest    = false;
    bool     _echo_pass   = false;

    // A module has to be asked before it says anything, and then it has to be
    // given room to answer. Sending RC frames every 4 ms leaves no usable gap:
    // the reply is either talked over or blocked by us still driving the wire.
    // After each request the transmitter goes quiet for this long.
    uint32_t _quiet_until = 0;
    uint32_t _rc_interval = RC_INTERVAL_MS;   // updated from the module's sync frame
    bool     _synced      = false;
    static const uint32_t QUIET_MS = 15;
    char      _device_name[24] = {};
    LinkStats _link;

    uint8_t  _rx_buf[64] = {};
    uint8_t  _rx_len    = 0;
    uint32_t _rx_bytes  = 0;
    uint32_t _rx_frames = 0;
    uint32_t _rx_bad    = 0;
    uint8_t  _bad_snap[3] = {};   // address, length, type
    uint8_t  _bad_calc    = 0;
    uint8_t  _bad_recv    = 0;
    bool     _have_bad    = false;
    uint8_t  _rx_last_b   = 0;

    uint8_t crsf_crc8(const uint8_t* d, uint8_t l);
    void    sendFrame(uint8_t dest, uint8_t type, const uint8_t* pl, uint8_t plen);
    void    sendRCChannels();
    void    sendOpenTXSync();
    void    parseFrame(const uint8_t* f, uint8_t total);
    void    resync();   // drop one byte and rescan after a bad frame

    // Parameter-tree discovery: one PARAMETER_READ in flight at a time,
    // paced the same way writes are so it never competes with RC channel
    // frames for the wire. A field whose entry is bigger than one frame
    // arrives in multiple chunks, reassembled here before being parsed.
    void        sendParamReadChunk(uint8_t field_id, uint8_t chunk);
    void        handleSettingsEntry(const uint8_t* pl, uint8_t pl_len);
    void        finishAssembly();
    LiveParam&  liveSlot(uint8_t field_id);   // find existing or allocate new

    LiveParam _live[MAX_LIVE_PARAMS];
    uint8_t   _live_count  = 0;
    uint8_t   _skip_last = 0, _skip_cnt = 0;
    uint8_t   _di_count = 0, _di_len = 0, _last_type = 0;
    uint8_t   _field_count = 0;   // from DEVICE_INFO; 0 until known

    bool     _disc_active   = false;
    uint8_t  _disc_field    = 1;
    uint8_t  _disc_chunk    = 0;
    bool     _disc_sent     = false;
    bool     _disc_got      = false;
    uint32_t _disc_sent_at  = 0;
    uint32_t _disc_wait_since = 0;   // waiting on DEVICE_INFO's field count
    static const uint32_t DISC_TIMEOUT_MS     = 300;
    static const uint32_t DISC_GIVE_UP_MS     = 3000;

    uint8_t   _asm_buf[240] = {};
    uint16_t  _asm_len      = 0;
    uint8_t   _asm_field_id = 0;
    uint8_t   _asm_parent   = 0;
    ParamType _asm_type     = ParamType::P_OUT_OF_RANGE;
    bool      _asm_hidden   = false;
    bool      _asm_more_chunks = false;
};

// Shared CRC-8 (DVB-S2 poly 0xD5) used by CRSF and the input parsers
uint8_t crsfCRC8(const uint8_t* d, uint8_t len);
