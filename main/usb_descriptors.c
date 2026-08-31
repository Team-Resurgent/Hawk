// Hawk — USB descriptors, byte-faithful to the Xbox Communicator ("Hawk").
//
// Sources: the Hawk class driver in the leaked XDK tree (matches interface
// class 0x78, finds one iso endpoint per interface, direction decides
// microphone vs headphone), Microsoft's own USB simulator of the device
// (usbsim IsocDevice: the exact config-descriptor bytes served below), and
// xemu's usb-xblc device (VID/PID/bcdDevice of the retail unit).
#include "tusb.h"
#include "usb_descriptors.h"

#define HAWK_VID 0x045E   // Microsoft
#define HAWK_PID 0x0283   // Xbox Communicator

// The Hawk driver's own sample-rate table (hawk2.cpp sm_HawkSampleRates).
// 11.025/22.05 kHz don't divide into 1 ms frames evenly: every extra_interval
// frames the packet carries one extra 16-bit sample.
const hawk_rate_info_t hawk_rates[HAWK_RATE_COUNT] = {
    {  8000, 16,  0 },
    { 11025, 22, 40 },
    { 16000, 32,  0 },
    { 22050, 44, 20 },
    { 24000, 48,  0 },
};

// ---- Device descriptor (18 bytes) ----------------------------------------
// bcdUSB 1.10, class 0 (per-interface), EP0 max packet 8 (pinned in
// tusb_config.h; the descriptor tracks the compiled size so they can't skew).
static const uint8_t s_device_desc[18] = {
    18, TUSB_DESC_DEVICE, 0x10, 0x01, 0x00, 0x00, 0x00, CFG_TUD_ENDPOINT0_SIZE,
    (uint8_t)(HAWK_VID & 0xFF), (uint8_t)(HAWK_VID >> 8),
    (uint8_t)(HAWK_PID & 0xFF), (uint8_t)(HAWK_PID >> 8),
    0x10, 0x01,          // bcdDevice 1.10 (retail unit)
    1, 2, 0,             // iManufacturer, iProduct, iSerial
    1                    // bNumConfigurations
};

// ---- Configuration descriptor (45 bytes total) ---------------------------
// Two interfaces of class 0x78 subclass 0x02, one iso endpoint each, with
// audio-style 9-byte endpoint descriptors (bRefresh/bSynchAddress = 0).
// bmAttributes 0x05 = isochronous, asynchronous. bMaxPower 0x32 = 100 mA.
static const uint8_t s_config_desc[] = {
    0x09, 0x02, 0x2D, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,
    // interface 0: headphone (host -> device)
    0x09, 0x04, 0x00, 0x00, 0x01, 0x78, 0x02, 0x00, 0x00,
    0x09, 0x05, HAWK_EP_HEADPHONE_OUT, 0x05, HAWK_ISO_MAXPKT, 0x00, 0x01, 0x00, 0x00,
    // interface 1: microphone (device -> host)
    0x09, 0x04, 0x01, 0x00, 0x01, 0x78, 0x02, 0x00, 0x00,
    0x09, 0x05, HAWK_EP_MIC_IN, 0x05, HAWK_ISO_MAXPKT, 0x00, 0x01, 0x00, 0x00,
};

// ---- Strings (the Xbox host never reads them, but PCs do) ----------------
static const char *const s_strings[] = {
    (const char[]){ 0x09, 0x04 },   // 0: LangID 0x0409
    "Microsoft",                    // 1: iManufacturer
    "Xbox Communicator",            // 2: iProduct
};

void hawk_get_descriptors(const void **dev, const uint8_t **cfg,
                          const char ***strs, int *nstr) {
    *dev  = s_device_desc;
    *cfg  = s_config_desc;
    *strs = (const char **)s_strings;
    *nstr = (int)(sizeof(s_strings) / sizeof(s_strings[0]));
}
