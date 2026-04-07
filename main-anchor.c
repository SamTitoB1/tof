#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "esp_mac.h"

// --- CONFIG ---
#define DEVICE_ID        "TOF_ANCHOR_04"
#define ANCHOR_X_M       4.05f                 
#define ANCHOR_Y_M       6.8f                 
#define WIFI_SSID        "ESP_RECEIVER"
#define WIFI_PASS        ".esp_receiver_capstone_32"
#define TARGET_SSID      "TOF_TAG-1"
#define MQTT_BROKER_HOST  "192.168.1.157"
#define MQTT_BROKER_PORT  1883
#define MQTT_TOPIC        "tof/measurements"
#define LOCATION_ID       "LOC_01"
#define FLOOR_ID          "FLOOR_01"
#define ROOM_ID           "ROOM_01"
#define FTM_SCAN_INTERVAL_MS 1500  
#define FTM_TIMEOUT_MS        5000
#define SPEED_OF_LIGHT_MPS    299792458.0

static const char *TAG = "TOF_ANCHOR";
static EventGroupHandle_t s_wifi_eg, s_ftm_eg;
#define WIFI_CONNECTED_BIT BIT0
#define FTM_DONE_BIT       BIT0
#define FTM_FAIL_BIT       BIT1

static volatile uint32_t s_scan_num = 0;
static esp_mqtt_client_handle_t s_mqtt_client = NULL;

typedef struct {
    float distance_m;
    float confidence;
    int8_t rssi;
} ftm_result_t;

static ftm_result_t s_ftm_result;

// --- POSITIONING ---
static void compute_position(float dist_m, float *out_x, float *out_y) {
    *out_x = ANCHOR_X_M + dist_m;
    *out_y = ANCHOR_Y_M;
}

// --- HANDLERS ---
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            ESP_LOGI(TAG, "Wi-Fi connecting...");
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT);
            ESP_LOGW(TAG, "Wi-Fi lost, retrying...");
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_FTM_REPORT) {
            wifi_event_ftm_report_t *r = (wifi_event_ftm_report_t *)data;
            if (r->status == FTM_STATUS_SUCCESS) {
                s_ftm_result.distance_m = (r->rtt_est * 1e-12 * SPEED_OF_LIGHT_MPS) / 2.0;
                s_ftm_result.confidence = fminf(r->ftm_report_num_entries / 64.0f, 1.0f);
                xEventGroupSetBits(s_ftm_eg, FTM_DONE_BIT);
            } else {
                xEventGroupSetBits(s_ftm_eg, FTM_FAIL_BIT);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi Connected! IP Received.");
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

// --- INIT ---
static void wifi_init_sta(void) {
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
    wifi_config_t wc = { .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // 🚀 FIX: Don't wait forever. Wait 10 seconds, then move on.
    xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
}

static bool ftm_run_session(void) {
    wifi_scan_config_t sc = { .ssid = (uint8_t *)TARGET_SSID, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return false;
    
    uint16_t n = 1; wifi_ap_record_t ap;
    if (esp_wifi_scan_get_ap_records(&n, &ap) != ESP_OK || n == 0) {
        ESP_LOGE(TAG, "Target %s not found in scan", TARGET_SSID);
        return false;
    }
    
    wifi_ftm_initiator_cfg_t f_cfg = { .frm_count = 16, .burst_period = 2, .channel = ap.primary };
    memcpy(f_cfg.resp_mac, ap.bssid, 6);
    s_ftm_result.rssi = ap.rssi;
    
    xEventGroupClearBits(s_ftm_eg, FTM_DONE_BIT | FTM_FAIL_BIT);
    if (esp_wifi_ftm_initiate_session(&f_cfg) != ESP_OK) return false;
    
    EventBits_t b = xEventGroupWaitBits(s_ftm_eg, FTM_DONE_BIT | FTM_FAIL_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(FTM_TIMEOUT_MS));
    return (b & FTM_DONE_BIT) != 0;
}

static void tof_task(void *pv) {
    s_ftm_eg = xEventGroupCreate();
    ESP_LOGI(TAG, "TOF Task Started. Scanning for %s...", TARGET_SSID);
    
    while (1) {
        s_scan_num++;
        float x = ANCHOR_X_M, y = ANCHOR_Y_M;
        
        if (ftm_run_session()) {
            compute_position(s_ftm_result.distance_m, &x, &y);
            ESP_LOGI(TAG, "Session #%lu SUCCESS: %.2fm", s_scan_num, s_ftm_result.distance_m);
        } else {
            s_ftm_result.distance_m = -1.0f; 
            s_ftm_result.confidence = 0.0f; 
            s_ftm_result.rssi = -100; 
            ESP_LOGE(TAG, "Session #%lu FAILED", s_scan_num);
        }

        vTaskDelay(pdMS_TO_TICKS(FTM_SCAN_INTERVAL_MS));
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_flash_init();
    }
    
    wifi_init_sta();

    // 🚀 MQTT still bypassed to isolate the Wi-Fi/FTM problem.
    // mqtt_app_start(); 

    ESP_LOGI(TAG, "Starting TOF Task...");
    xTaskCreate(tof_task, "tof_task", 8192, NULL, 5, NULL);
}
