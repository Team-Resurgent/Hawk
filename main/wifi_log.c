// Hawk — WiFi UDP debug logging (see wifi_log.h). Ported from Falcon; Talon's
// full wifi_net (NVS credentials, SoftAP provisioning portal, WPS, mDNS) is
// the plan once the web-UI milestone lands.
//
// Compiled only when CONFIG_HAWK_WIFI_LOG is set; the build system excludes
// this file otherwise, so wifi_creds.h is only needed with the feature on.
#include "sdkconfig.h"
#if defined(CONFIG_HAWK_WIFI_LOG)
#include <string.h>
#include <stdio.h>
#include "wifi_log.h"
#include "wifi_creds.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define HAWK_LOG_PORT 5556

static const char *TAG = "hawk.wifi";

static int                s_sock = -1;
static struct sockaddr_in s_bcast;
static vprintf_like_t     s_orig_vprintf;
static volatile bool      s_net_up;

// esp_log hook: keep UART output, and when the socket is up also UDP-broadcast it.
static int log_vprintf(const char *fmt, va_list ap) {
    if (s_sock >= 0 && s_net_up) {
        char buf[256];
        va_list ap2; va_copy(ap2, ap);
        int n = vsnprintf(buf, sizeof(buf), fmt, ap2);
        va_end(ap2);
        if (n > 0) {
            if (n > (int)sizeof(buf)) n = sizeof(buf);
            sendto(s_sock, buf, n, 0, (struct sockaddr *)&s_bcast, sizeof(s_bcast));
        }
    }
    return s_orig_vprintf ? s_orig_vprintf(fmt, ap) : 0;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_net_up = false;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        if (s_sock < 0) {
            s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            int yes = 1;
            setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
            memset(&s_bcast, 0, sizeof(s_bcast));
            s_bcast.sin_family = AF_INET;
            s_bcast.sin_port = htons(HAWK_LOG_PORT);
            s_bcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        }
        s_net_up = true;
        ESP_LOGI(TAG, "wifi up, ip=" IPSTR ", broadcasting logs on udp/%d",
                 IP2STR(&e->ip_info.ip), HAWK_LOG_PORT);
    }
}

void wifi_log_start(void) {
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL);

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, HAWK_WIFI_SSID, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, HAWK_WIFI_PASS, sizeof(wc.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_set_ps(WIFI_PS_NONE);   // no power-save: keep logs flowing promptly
    esp_wifi_start();

    ESP_LOGI(TAG, "wifi log starting, ssid=%s", HAWK_WIFI_SSID);
}

#endif // CONFIG_HAWK_WIFI_LOG
