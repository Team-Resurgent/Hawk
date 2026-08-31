// Hawk — USB descriptors for the Xbox Communicator (Hawk / XBLC) emulation.
#pragma once
#include <stdint.h>

// One device, two vendor-class (0x78) interfaces, one iso endpoint each:
//   interface 0 = headphone: iso OUT EP 0x04 (Xbox -> device audio)
//   interface 1 = microphone: iso IN EP 0x85 (device -> Xbox audio)
#define HAWK_EP_HEADPHONE_OUT  0x04
#define HAWK_EP_MIC_IN         0x85

// wMaxPacketSize on both iso endpoints. 50 = 24 samples (24 kHz frame) + the
// extra sample that 11.025/22.05 kHz rates carry on some frames, matching the
// descriptor in Microsoft's own USB simulator of the device.
#define HAWK_ISO_MAXPKT        50

// The five sample rates the Hawk driver knows, selected by EP0 vendor
// SET_FEATURE with a rate INDEX into this table (see hawk_class.c).
#define HAWK_RATE_COUNT        5
typedef struct {
    uint16_t sample_rate;     // Hz
    uint8_t  bytes_per_frame; // payload bytes in a normal 1 ms USB frame
    uint8_t  extra_interval;  // every N frames carry one extra sample (0 = never)
} hawk_rate_info_t;
extern const hawk_rate_info_t hawk_rates[HAWK_RATE_COUNT];
#define HAWK_RATE_INDEX_DEFAULT 2   // 16 kHz — what a real XBLC defaults to

void hawk_get_descriptors(const void **dev, const uint8_t **cfg,
                          const char ***strs, int *nstr);
