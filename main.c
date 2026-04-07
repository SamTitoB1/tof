/**
 * @file    main.c
 * @brief   ESP32-S3 · WiFi FTM Responder (The Mobile Target Tag) - ESP-IDF v5/v6
 */

 #include <string.h>
 #include "freertos/FreeRTOS.h"
 #include "freertos/task.h"
 #include "esp_system.h"
 #include "esp_mac.h"
 #include "esp_wifi.h"
 #include "esp_event.h"
 #include "esp_log.h"
 #include "nvs_flash.h"
 
 #define TARGET_SSID             "TOF_TAG-1"
 #define TARGET_CHANNEL          6  
 #define MAX_STA_CONN            4
 static const char *TAG = "TOF_TARGET";
 
 static void wifi_init_softap_and_ftm(void)
 {
     ESP_ERROR_CHECK(esp_netif_init());
     ESP_ERROR_CHECK(esp_event_loop_create_default());
     esp_netif_create_default_wifi_ap();
 
     wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
     ESP_ERROR_CHECK(esp_wifi_init(&cfg));
 
     /* ── Configure the Access Point WITH FTM Responder baked in ── */
     wifi_config_t wifi_config = {
         .ap = {
             .ssid           = TARGET_SSID,
             .ssid_len       = strlen(TARGET_SSID),
             .channel        = TARGET_CHANNEL,
             .authmode       = WIFI_AUTH_OPEN,     
             .max_connection = MAX_STA_CONN,
             /* ⚠️ This one boolean replaces all the old FTM enable code! */
             .ftm_responder  = true    
         },
     };
 
     ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
     ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
     ESP_ERROR_CHECK(esp_wifi_start());
 
     ESP_LOGI(TAG, "=========================================");
     ESP_LOGI(TAG, "Target AP Started Successfully");
     ESP_LOGI(TAG, "SSID:      %s", TARGET_SSID);
     ESP_LOGI(TAG, "Channel:   %d", TARGET_CHANNEL);
     ESP_LOGI(TAG, "FTM Mode:  Hardware Responder ENABLED");
     ESP_LOGI(TAG, "=========================================");
 }
 
 void app_main(void)
 {
     ESP_LOGI(TAG, "Booting ESP32-S3 ToF Target Node...");
 
     /* Initialize NVS */
     esp_err_t ret = nvs_flash_init();
     if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
         ESP_ERROR_CHECK(nvs_flash_erase());
         ret = nvs_flash_init();
     }
     ESP_ERROR_CHECK(ret);
 
     /* Start the AP (which automatically arms the FTM hardware) */
     wifi_init_softap_and_ftm();
 
     while (1) {
         vTaskDelay(pdMS_TO_TICKS(10000));
         ESP_LOGI(TAG, "Target is active. Awaiting FTM bursts...");
     }
 }