/**
 * @file    main.c
 * @brief   ESP32-S3 · FTM Tag firmware
 */

/* --- Standard -------------------------------------------------------------- */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* --- FreeRTOS -------------------------------------------------------------- */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* --- ESP-IDF --------------------------------------------------------------- */
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"

/* ===========================================================================
 * USER-CONFIGURABLE
 * =========================================================================== */
#define TAG_DEVICE_ID       "TOF_TAG-1"
#define TAG_SSID            "TOF_TAG-1"
#define TAG_CHANNEL         11      // MUST MATCH ANCHOR/ROUTER CHANNEL
#define TAG_MAX_CONNECTIONS 4
#define TAG_BEACON_MS       100

static const char *LOG = "FTM_TAG";

/* ===========================================================================
 * WIFI EVENT HANDLER
 * =========================================================================== */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_AP_START:
                ESP_LOGI(LOG, "SoftAP started — broadcasting SSID: %s ch: %d", TAG_SSID, TAG_CHANNEL);
                break;
            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(LOG, "Anchor/Station connected");
                break;
            default:
                break;
        }
    }
}

/* ===========================================================================
 * WIFI INIT — FTM responder active
 * =========================================================================== */
static void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid            = TAG_SSID,
            .ssid_len        = (uint8_t)strlen(TAG_SSID),
            .channel         = TAG_CHANNEL,
            .authmode        = WIFI_AUTH_OPEN,
            .max_connection  = TAG_MAX_CONNECTIONS,
            .beacon_interval = TAG_BEACON_MS,
            .ftm_responder   = 1,  // 🚀 HARDWARE FTM RESPONDED ENABLED
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ===========================================================================
 * APP ENTRY POINT
 * =========================================================================== */
void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Print device identity */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    ESP_LOGI(LOG, "=============================================");
    ESP_LOGI(LOG, "  FTM Tag ID: %s", TAG_DEVICE_ID);
    ESP_LOGI(LOG, "  BSSID     : %02x:%02x:%02x:%02x:%02x:%02x", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(LOG, "=============================================");

    wifi_init_softap();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(LOG, "Tag heartbeat - SSID: %s", TAG_SSID);
    }
}
