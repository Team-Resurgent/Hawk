// Hawk — ESP32-S3 emulator of the Xbox Communicator ("Hawk"), the controller
// headset. The native USB-OTG port is the communicator; console/flash is on
// the UART bridge. Milestone 1 "speaks" a recognizable tone into the mic
// endpoint and measures whatever the Xbox plays back to the headphone
// endpoint, so the HawkLoopback Xbox app (xbox/HawkLoopback) closes the loop
// end to end: tone -> Xbox mic XMO -> TV speakers + headphone XMO -> here.
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "sdkconfig.h"
#include "usb_descriptors.h"
#if defined(CONFIG_HAWK_WIFI_LOG)
#include "wifi_log.h"
#endif

static const char *TAG = "hawk";

extern volatile uint32_t g_hawk_reset, g_hawk_open, g_hawk_ctrl_rate, g_hawk_ctrl_agc;
extern volatile uint32_t g_hawk_mic_pkts, g_hawk_mic_bytes, g_hawk_mic_err, g_hawk_mic_sfail;
extern volatile uint32_t g_hawk_out_pkts, g_hawk_out_bytes, g_hawk_out_err;
extern volatile uint32_t g_hawk_out_energy, g_hawk_out_samples, g_hawk_out_zc;
extern volatile int16_t  g_hawk_out_peak;
void hawk_class_get_state(uint8_t *rate_index, uint8_t *mic_open, uint8_t *out_open);

void app_main(void) {
    // WiFi logging is OFF by default (Kconfig: HAWK_WIFI_LOG). Falcon showed the
    // radio's CPU/interrupt load can break the Xbox's timing-strict USB
    // enumeration, so enable it only for bench debugging (needs main/wifi_creds.h).
    #if defined(CONFIG_HAWK_WIFI_LOG)
    wifi_log_start();
    #endif
    ESP_LOGI(TAG, "boot: reset_reason=%d", (int)esp_reset_reason());
    ESP_LOGI(TAG, "Hawk: Xbox Communicator emulator starting");

    // esp_tinyusb builds its tud_descriptor_*_cb from the pointers passed here
    // (passing NULL would substitute its own default descriptors). The class
    // driver itself registers via usbd_app_driver_get_cb (hawk_class.c).
    const void *dev; const uint8_t *cfg; const char **strs; int nstr;
    hawk_get_descriptors(&dev, &cfg, &strs, &nstr);
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor        = (const tusb_desc_device_t *)dev,
        .string_descriptor        = strs,
        .string_descriptor_count  = nstr,
        .configuration_descriptor = cfg,
        .external_phy             = false,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB device installed - waiting for the Xbox to enumerate");

    // Heartbeat: everything a serial monitor needs to follow the negotiation
    // and judge the headphone-return audio (level + zero-crossing frequency).
    uint32_t last_mic = 0, last_out = 0;
    for (uint32_t t = 0;; t += 2) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        uint8_t rate_idx, mic_open, out_open;
        hawk_class_get_state(&rate_idx, &mic_open, &out_open);

        // Snapshot + reset the audio accumulators for a per-interval reading.
        uint32_t energy  = g_hawk_out_energy;  g_hawk_out_energy = 0;
        uint32_t samples = g_hawk_out_samples; g_hawk_out_samples = 0;
        uint32_t zc      = g_hawk_out_zc;      g_hawk_out_zc = 0;
        int16_t  peak    = g_hawk_out_peak;    g_hawk_out_peak = 0;
        uint32_t rate    = hawk_rates[rate_idx].sample_rate;
        uint32_t avg     = samples ? energy / samples : 0;
        uint32_t freq    = samples ? (uint32_t)((uint64_t)zc * rate / (2 * samples)) : 0;

        ESP_LOGI(TAG, "HB t=%lus mnt=%d rst=%lu open=%lu rate=%luHz agcset=%lu "
                      "mic[%c pkts=%lu(+%lu/s) err=%lu sfail=%lu] "
                      "hp[%c pkts=%lu(+%lu/s) err=%lu avg=%lu peak=%d f~%luHz]",
                 (unsigned long)t, tud_mounted() ? 1 : 0,
                 (unsigned long)g_hawk_reset, (unsigned long)g_hawk_open,
                 (unsigned long)rate, (unsigned long)g_hawk_ctrl_agc,
                 mic_open ? '+' : '-', (unsigned long)g_hawk_mic_pkts,
                 (unsigned long)((g_hawk_mic_pkts - last_mic) / 2),
                 (unsigned long)g_hawk_mic_err, (unsigned long)g_hawk_mic_sfail,
                 out_open ? '+' : '-', (unsigned long)g_hawk_out_pkts,
                 (unsigned long)((g_hawk_out_pkts - last_out) / 2),
                 (unsigned long)g_hawk_out_err, (unsigned long)avg, (int)peak,
                 (unsigned long)freq);
        last_mic = g_hawk_mic_pkts;
        last_out = g_hawk_out_pkts;
    }
}
