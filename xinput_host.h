#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  Xbox 360 / Xbox One host support — TinyUSB application class driver
//
//  Xbox controllers use USB vendor class (0xFF), so the HID driver ignores them.
//  Opening their endpoints by hand from tuh_mount_cb does not work: the transfer
//  is accepted but never scheduled, because the host stack has no interface
//  bound to it.
//
//  The supported route is to register an application class driver through
//  usbh_app_driver_get_cb().  TinyUSB then calls open() with the interface
//  descriptor, calls set_config() when the device is ready, and routes every
//  endpoint completion into xfer_cb() — the stack owns the bookkeeping.
//
//    Xbox 360 : class FF / sub 5D / proto 01   (XInput report format)
//    Xbox One : class FF / sub 47 / proto D0   (GIP report format)
// ═══════════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include "host/usbh_pvt.h"     // usbh_edpt_xfer, usbh_driver_set_config_complete
#include "input.h"
#include "usb_diag.h"

extern InputManager g_input;

#define XINPUT_MAX_DEV 2

// Xbox One wake-up packet — without it the pad never reports.
static const uint8_t GIP_HELLO[]  = { 0x05, 0x20, 0x00, 0x01, 0x00 };
// Xbox 360 player-slot LED: 0x06 = quadrant 1 solid.
static const uint8_t X360_LED_P1[] = { 0x01, 0x03, 0x06 };

typedef struct {
    uint8_t  daddr;      // 0 = slot free
    uint8_t  itf_num;
    uint8_t  ep_in;
    uint8_t  ep_out;
    uint16_t mps_in;     // request exactly this many bytes per transfer
    bool     gip;
    uint8_t  buf_in[64];
    uint8_t  buf_out[32];
} xinput_dev_t;

static xinput_dev_t _xdev[XINPUT_MAX_DEV];

// ─── SLOT HELPERS ────────────────────────────────────────────────────────────
static xinput_dev_t* xinput_alloc(void) {
    for (uint8_t i = 0; i < XINPUT_MAX_DEV; i++)
        if (_xdev[i].daddr == 0) return &_xdev[i];
    return nullptr;
}
static xinput_dev_t* xinput_find(uint8_t daddr) {
    for (uint8_t i = 0; i < XINPUT_MAX_DEV; i++)
        if (_xdev[i].daddr == daddr) return &_xdev[i];
    return nullptr;
}

// ─── DRIVER CALLBACKS ────────────────────────────────────────────────────────
static bool xinputh_init(void) {
    memset(_xdev, 0, sizeof(_xdev));
    return true;
}

static bool xinputh_deinit(void) {
    memset(_xdev, 0, sizeof(_xdev));
    return true;
}

// Returns the number of descriptor bytes claimed (0 = this driver declines).
static uint16_t xinputh_open(uint8_t rhport, uint8_t daddr,
                              tusb_desc_interface_t const* itf, uint16_t max_len) {
    (void)rhport;
    if (itf->bInterfaceClass != 0xFF) return 0;

    bool gip;
    if (itf->bInterfaceSubClass == 0x5D && itf->bInterfaceProtocol == 0x01)
        gip = false;                                    // Xbox 360 / XInput
    else if (itf->bInterfaceSubClass == 0x47 && itf->bInterfaceProtocol == 0xD0)
        gip = true;                                     // Xbox One / GIP
    else
        return 0;

    xinput_dev_t* d = xinput_alloc();
    if (!d) return 0;

    // Walk this interface's descriptors. Xbox pads insert a vendor-specific
    // 0x21 block between the interface and its endpoints, so we can't just
    // assume interface + N endpoints - we measure what we actually consume.
    uint8_t const* start = (uint8_t const*)itf;
    uint8_t const* end   = start + max_len;
    uint8_t const* p     = tu_desc_next(start);

    uint8_t  ep_in = 0, ep_out = 0;
    uint16_t mps   = 0;

    while (p < end) {
        uint8_t t = tu_desc_type(p);
        if (t == TUSB_DESC_INTERFACE) break;            // next interface begins
        if (t == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const* ep = (tusb_desc_endpoint_t const*)p;
            if (ep->bmAttributes.xfer == TUSB_XFER_INTERRUPT) {
                if (!tuh_edpt_open(daddr, ep)) return 0;
                if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) {
                    ep_in = ep->bEndpointAddress;
                    uint16_t m = 0;
                    memcpy(&m, &ep->wMaxPacketSize, sizeof(uint16_t));
                    mps = m & 0x07FF;
                } else {
                    ep_out = ep->bEndpointAddress;
                }
            }
        }
        p = tu_desc_next(p);
    }

    if (!ep_in) return 0;

    d->daddr   = daddr;
    d->itf_num = itf->bInterfaceNumber;
    d->ep_in   = ep_in;
    d->ep_out  = ep_out;
    d->mps_in  = mps ? mps : (gip ? 64 : 20);
    d->gip     = gip;

    g_diag.xinput_open = true;
    if (ep_out) g_diag.xinput_out = true;
    g_input.setPresent(true);

    return (uint16_t)(p - start);   // bytes consumed
}

static bool xinputh_set_config(uint8_t daddr, uint8_t itf_num) {
    xinput_dev_t* d = xinput_find(daddr);
    if (d && d->itf_num == itf_num) {
        // Wake-up / player-LED packet
        if (d->ep_out) {
            const uint8_t* pkt = d->gip ? GIP_HELLO           : X360_LED_P1;
            uint16_t       n   = d->gip ? sizeof(GIP_HELLO)   : sizeof(X360_LED_P1);
            memcpy(d->buf_out, pkt, n);
            usbh_edpt_xfer(daddr, d->ep_out, d->buf_out, n);
        }
        // First input request. Length must equal the endpoint max packet size:
        // a transfer only ends early on a SHORT packet, and the 360 sends
        // exactly 20 bytes on a 20-byte endpoint.
        if (usbh_edpt_xfer(daddr, d->ep_in, d->buf_in, d->mps_in)) g_diag.arm_ok++;
        else                                                        g_diag.arm_fail++;
    }
    usbh_driver_set_config_complete(daddr, itf_num);
    return true;
}

static bool xinputh_xfer_cb(uint8_t daddr, uint8_t ep_addr,
                             xfer_result_t result, uint32_t xferred) {
    xinput_dev_t* d = xinput_find(daddr);
    if (!d) return false;

    if (ep_addr == d->ep_in) {
        if (result == XFER_RESULT_SUCCESS && xferred >= 14) {
            const uint8_t* r   = d->buf_in;
            uint16_t       len = (uint16_t)xferred;
            g_diag.xinput_rpts++;
            g_diag.last_rpt_len = (uint8_t)len;

            // Stage only; Core0 decodes. See InputManager::processUSB().
            if (d->gip) {
                // 0x20 = input report; other IDs are status / battery
                if (r[0] == 0x20)
                    g_input.submitReport(InputManager::RPT_GIP, r, len);
            } else {
                // 0x00 0x14 = controller state; 0x01/0x02/0x03 are LED ack,
                // battery and announce messages which must not be parsed.
                if (r[0] == 0x00 && r[1] == 0x14)
                    g_input.submitReport(InputManager::RPT_XINPUT, r, len);
            }
        } else if (result != XFER_RESULT_SUCCESS) {
            g_diag.xfer_fail++;
        }
        // Re-arm for the next report
        if (usbh_edpt_xfer(daddr, d->ep_in, d->buf_in, d->mps_in)) g_diag.arm_ok++;
        else                                                        g_diag.arm_fail++;
    }
    return true;
}

static void xinputh_close(uint8_t daddr) {
    xinput_dev_t* d = xinput_find(daddr);
    if (d) {
        memset(d, 0, sizeof(*d));
        g_diag.xinput_open = false;
        g_diag.xinput_out  = false;
        g_input.notifyDisconnect();
    }
}

// ─── DRIVER REGISTRATION ─────────────────────────────────────────────────────
// TinyUSB calls this during host init to collect application class drivers.
static usbh_class_driver_t const _xinput_driver = {
#if (TUSB_VERSION_NUMBER >= 1700) || (CFG_TUSB_DEBUG >= 2)
    .name       = "XINPUT",
#endif
    .init       = xinputh_init,
#if TUSB_VERSION_NUMBER >= 1700
    .deinit     = xinputh_deinit,
#endif
    .open       = xinputh_open,
    .set_config = xinputh_set_config,
    .xfer_cb    = xinputh_xfer_cb,
    .close      = xinputh_close,
};

extern "C" usbh_class_driver_t const*
usbh_app_driver_get_cb(uint8_t* driver_count) {
    *driver_count = 1;
    return &_xinput_driver;
}

// ─── COMPATIBILITY SHIM ──────────────────────────────────────────────────────
// The class driver handles everything now; these keep the sketch unchanged.
namespace XInputHost {
    inline void onMount(uint8_t)   {}
    inline void onUnmount(uint8_t) {}
    inline void poll()             {}
    inline bool active() {
        for (uint8_t i = 0; i < XINPUT_MAX_DEV; i++)
            if (_xdev[i].daddr) return true;
        return false;
    }
}
