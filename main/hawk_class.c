// Hawk — custom TinyUSB application class driver for the Xbox Communicator.
//
// Owns both vendor-class (0x78) interfaces. There are no alt-settings. The iso
// endpoints are armed LAZILY -- only once the Xbox's vendor SET_FEATURE(sample
// rate) request has landed on EP0 (see the s_stream_enable note below), never
// at SET_CONFIGURATION time. Then:
//   - mic IN (EP 0x85): continuously offers tone packets sized to the current
//     sample rate's per-frame cadence (see hawk_rates[]).
//   - headphone OUT (EP 0x04): continuously receives whatever the Xbox plays
//     back and folds it into level/frequency stats so the PC-side monitor can
//     verify the full Xbox -> device path without a speaker.
//
// EP0 vendor control ("Talon Specification" in Microsoft's driver comments):
//   SET_FEATURE (bmRequestType 0x41, bRequest 0x03)
//     wIndex 0: wValue = 0x0100 | rate_index  -> select sample rate
//     wIndex 1: wValue = 0/1                  -> AGC off/on
// The requests are interface-directed with the FEATURE in wIndex, so TinyUSB
// routes wIndex 0 to interface 0 and wIndex 1 to interface 1 — both are ours.
#include <string.h>
#include "tusb.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"
#include "esp_log.h"
#include "usb_descriptors.h"
#include "tone.h"

static const char *TAG = "hawk.cls";

// Vendor SET_FEATURE selectors (wIndex), from the leaked Hawk driver.
#define HAWK_VENDOR_FEATURE_SAMPLE_RATE 0
#define HAWK_VENDOR_FEATURE_AGC         1

// Diagnostic counters (read by the heartbeat logger in hawk_main.c).
volatile uint32_t g_hawk_reset, g_hawk_open, g_hawk_ctrl_rate, g_hawk_ctrl_agc;
volatile uint32_t g_hawk_mic_pkts, g_hawk_mic_bytes, g_hawk_mic_err, g_hawk_mic_sfail;
volatile uint32_t g_hawk_out_pkts, g_hawk_out_bytes, g_hawk_out_err;
volatile uint32_t g_hawk_out_energy;   // running sum of |sample|, reset by reader
volatile uint32_t g_hawk_out_samples;  // samples behind g_hawk_out_energy
volatile uint32_t g_hawk_out_zc;       // zero crossings behind g_hawk_out_samples
volatile int16_t  g_hawk_out_peak;
volatile uint8_t  g_hawk_rate_index = HAWK_RATE_INDEX_DEFAULT;
volatile uint8_t  g_hawk_agc = 1;

static uint8_t  s_rhport;
static bool     s_mic_open, s_out_open;

// Keeping the iso IN endpoint armed while the host is NOT polling makes the
// dwc2 core raise "incomplete ISO IN" every frame (a ~500/s error storm that
// can wedge EP0 handling in slave mode). The Xbox always sends the vendor
// SET_FEATURE(sample rate) right before it opens the iso pipe, so gate BOTH
// iso endpoints on that request; a long mic error streak (host stopped
// polling) disarms again. This also keeps the shared RX FIFO clear for EP0
// SETUP reception during the post-config vendor-request phase.
static bool     s_stream_enable;
static uint32_t s_mic_err_streak;
#define HAWK_MIC_ERR_STREAK_MAX 4000

// --- microphone (iso IN): double-buffered single-packet pump ---------------
static uint8_t s_mic_buf[2][HAWK_ISO_MAXPKT] CFG_TUSB_MEM_ALIGN;
static uint8_t s_mic_idx;
static volatile bool s_mic_inflight;
static uint16_t s_extra_countdown;     // frames until the +1-sample packet

// --- headphone (iso OUT): one receive buffer in flight ---------------------
static uint8_t s_out_buf[HAWK_ISO_MAXPKT] CFG_TUSB_MEM_ALIGN;
static volatile bool s_out_inflight;
static int16_t s_out_last_sample;

// Next mic packet size in bytes for the current rate (advances the
// extra-sample cadence for 11.025/22.05 kHz).
static uint16_t mic_next_packet_bytes(void) {
    const hawk_rate_info_t *r = &hawk_rates[g_hawk_rate_index];
    uint16_t bytes = r->bytes_per_frame;
    if (r->extra_interval) {
        if (++s_extra_countdown >= r->extra_interval) {
            s_extra_countdown = 0;
            bytes += 2;
        }
    }
    return bytes;
}

static void mic_submit_next(uint8_t rhport) {
    uint8_t *buf = s_mic_buf[s_mic_idx];
    s_mic_idx ^= 1;
    uint16_t bytes = mic_next_packet_bytes();
    tone_fill((int16_t *)buf, bytes / 2);
    if (usbd_edpt_xfer(rhport, HAWK_EP_MIC_IN, buf, bytes, false)) {
        g_hawk_mic_pkts++;
        g_hawk_mic_bytes += bytes;
        s_mic_inflight = true;
    } else {
        g_hawk_mic_sfail++;
        s_mic_inflight = false;
    }
}

static void out_submit_next(uint8_t rhport) {
    s_out_inflight = usbd_edpt_xfer(rhport, HAWK_EP_HEADPHONE_OUT,
                                    s_out_buf, HAWK_ISO_MAXPKT, false);
}

// Fold a received headphone packet into the level/zero-crossing stats. A pure
// tone of frequency f produces ~2f zero crossings per second, so the PC-side
// monitor can recognize the loopback tone numerically.
static void out_account(uint32_t bytes) {
    const int16_t *s = (const int16_t *)s_out_buf;
    uint32_t n = bytes / 2;
    int16_t prev = s_out_last_sample;
    for (uint32_t i = 0; i < n; i++) {
        int16_t v = s[i];
        g_hawk_out_energy += (v < 0) ? -v : v;
        if (v > g_hawk_out_peak) g_hawk_out_peak = v;
        if ((prev < 0 && v >= 0) || (prev >= 0 && v < 0)) g_hawk_out_zc++;
        prev = v;
    }
    s_out_last_sample = prev;
    g_hawk_out_samples += n;
    g_hawk_out_pkts++;
    g_hawk_out_bytes += bytes;
}

static void hawk_set_rate_index(uint8_t idx) {
    if (idx >= HAWK_RATE_COUNT) return;
    g_hawk_rate_index = idx;
    s_extra_countdown = 0;
    tone_reset(hawk_rates[idx].sample_rate);
    g_hawk_ctrl_rate++;
    ESP_LOGI(TAG, "sample rate -> %u Hz (index %u)",
             hawk_rates[idx].sample_rate, idx);
    // The Xbox sets the rate right before opening the iso pipe: start offering
    // mic data now.
    s_stream_enable = true;
    s_mic_err_streak = 0;
    usbd_sof_enable(s_rhport, SOF_CONSUMER_USER, true);  // pace the iso pipes
    if (s_mic_open && !s_mic_inflight) mic_submit_next(s_rhport);
    if (s_out_open && !s_out_inflight) out_submit_next(s_rhport);
}

// ---- usbd_class_driver_t callbacks ----------------------------------------

static void hawk_init(void) {
    tone_reset(hawk_rates[g_hawk_rate_index].sample_rate);
}

static void hawk_reset(uint8_t rhport) {
    g_hawk_reset++;
    s_mic_open = s_out_open = false;
    s_mic_inflight = s_out_inflight = false;
    s_stream_enable = false;
    s_mic_err_streak = 0;
    s_extra_countdown = 0;
    usbd_sof_enable(rhport, SOF_CONSUMER_USER, false);
}

// Claim an interface (called once per interface during SET_CONFIGURATION).
// Each interface is 9 (interface) + 9 (audio-style endpoint) descriptor bytes.
static uint16_t hawk_open(uint8_t rhport, tusb_desc_interface_t const *itf,
                          uint16_t max_len) {
    if (itf->bInterfaceClass != 0x78 || max_len < 18) return 0;
    s_rhport = rhport;

    const uint8_t *ep_desc = (const uint8_t *)itf + 9;
    uint8_t ep_addr = ep_desc[2];

    usbd_edpt_iso_alloc(rhport, ep_addr, 64);
    usbd_edpt_iso_activate(rhport, (const tusb_desc_endpoint_t *)ep_desc);

    // Arm NEITHER endpoint here. In dwc2 SLAVE mode an armed OUT endpoint (the
    // headphone, EP 0x04) shares the single RX FIFO with EP0, and the Xbox
    // sends the vendor SET_FEATURE(sample rate) on EP0 right after this
    // SET_CONFIGURATION -- before it ever opens the iso pipes. Arming the OUT
    // endpoint now starved EP0's SETUP reception, so that vendor SETUP never
    // reached the core and the Xbox saw a STALL. Both directions are armed
    // lazily once the rate request lands (hawk_set_rate_index), by which point
    // the control phase is done.
    if (ep_addr == HAWK_EP_MIC_IN) {
        s_mic_open = true;
        s_mic_inflight = false;
    } else if (ep_addr == HAWK_EP_HEADPHONE_OUT) {
        s_out_open = true;
        s_out_inflight = false;
    }

    g_hawk_open++;
    ESP_LOGI(TAG, "open iface %u (ep 0x%02X)", itf->bInterfaceNumber, ep_addr);
    return 18;
}

// Standard interface requests (SET/GET_INTERFACE) reach the class driver via
// usbd's recipient router. VENDOR requests do NOT -- usbd short-circuits every
// vendor-type control request to tud_vendor_control_xfer_cb (below), before the
// recipient switch, so the communicator's SET_FEATURE lands there, not here.
static bool hawk_control_xfer(uint8_t rhport, uint8_t stage,
                              tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) return true;

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
        switch (request->bRequest) {
            case TUSB_REQ_SET_INTERFACE:   // no alt-settings; alt 0 only
                return request->wValue == 0 && tud_control_status(rhport, request);
            case TUSB_REQ_GET_INTERFACE: {
                static uint8_t alt0 = 0;
                return tud_control_xfer(rhport, request, &alt0, 1);
            }
            default: return false;
        }
    }
    return false;
}

// Vendor EP0 handler. usbd.c routes ALL vendor-type control requests straight
// here (bypassing the class driver + recipient routing), so this is where the
// communicator's vendor SET_FEATURE arrives:
//   bmRequestType 0x41 (OUT/vendor/interface), bRequest 0x03 (SET_FEATURE),
//   wIndex 0: wValue = 0x0100 | rate_index  -> sample rate
//   wIndex 1: wValue = 0/1                  -> AGC off/on
// Both are zero-length control writes: ACK at the SETUP stage.
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) return true;

    if (request->bmRequestType_bit.direction == TUSB_DIR_OUT &&
        request->bRequest == TUSB_REQ_SET_FEATURE) {
        switch (request->wIndex) {
            case HAWK_VENDOR_FEATURE_SAMPLE_RATE:  // 0
                hawk_set_rate_index((uint8_t)(request->wValue & 0xFF));
                return tud_control_status(rhport, request);
            case HAWK_VENDOR_FEATURE_AGC:          // 1
                g_hawk_agc = (uint8_t)(request->wValue & 1);
                g_hawk_ctrl_agc++;
                ESP_LOGI(TAG, "AGC -> %u", g_hawk_agc);
                return tud_control_status(rhport, request);
            default: break;
        }
    }
    ESP_LOGW(TAG, "vendor ctrl UNHANDLED bmReq=%02X bReq=%02X wVal=%04X wIdx=%04X",
             request->bmRequestType, request->bRequest, request->wValue, request->wIndex);
    return false;
}

static bool hawk_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result,
                         uint32_t xferred) {
    if (ep_addr == HAWK_EP_MIC_IN) {
        s_mic_inflight = false;
        if (result != XFER_RESULT_SUCCESS) {
            g_hawk_mic_err++;
            if (++s_mic_err_streak >= HAWK_MIC_ERR_STREAK_MAX) {
                s_stream_enable = false;   // host went away; stop the storm
                ESP_LOGW(TAG, "mic stream disarmed (error streak)");
            }
        } else {
            s_mic_err_streak = 0;
        }
        if (s_mic_open && s_stream_enable) mic_submit_next(rhport);
    } else if (ep_addr == HAWK_EP_HEADPHONE_OUT) {
        s_out_inflight = false;
        if (result == XFER_RESULT_SUCCESS) out_account(xferred);
        else                               g_hawk_out_err++;
        if (s_out_open && s_stream_enable) out_submit_next(rhport);
    }
    return true;
}

// SOF (once per 1 ms USB frame) paces the iso pipes: an iso IN packet must be
// sitting in the TX FIFO for EVERY host poll or the dwc2 flags "incomplete ISO
// IN" and sends nothing (mic silence). Re-arming only on completion can't keep
// up, so re-arm here every frame.
//
// This is gated on s_stream_enable, which only goes true once the vendor
// SET_FEATURE(rate) request has been answered -- so during the post-config
// vendor-request phase SOF is a no-op and cannot crowd EP0. (An earlier build
// disabled SOF entirely to protect EP0, but the real EP0 problem was that
// vendor requests were never dispatched; that is fixed in
// tud_vendor_control_xfer_cb, so SOF pacing is safe again.)
static void hawk_sof(uint8_t rhport, uint32_t frame_count) {
    (void)frame_count;
    if (!s_stream_enable) return;
    if (s_mic_open && !s_mic_inflight) mic_submit_next(rhport);
    if (s_out_open && !s_out_inflight) out_submit_next(rhport);
}

void hawk_class_kick(void) { /* SOF drives arming now; nothing to do here */ }

static const usbd_class_driver_t s_hawk_driver = {
    .name            = "hawk-comm",
    .init            = hawk_init,
    .deinit          = NULL,
    .reset           = hawk_reset,
    .open            = hawk_open,
    .control_xfer_cb = hawk_control_xfer,
    .xfer_cb         = hawk_xfer_cb,
    .xfer_isr        = NULL,
    .sof             = hawk_sof,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &s_hawk_driver;
}

void hawk_class_get_state(uint8_t *rate_index, uint8_t *mic_open, uint8_t *out_open) {
    if (rate_index) *rate_index = g_hawk_rate_index;
    if (mic_open)   *mic_open   = s_mic_open ? 1 : 0;
    if (out_open)   *out_open   = s_out_open ? 1 : 0;
}
