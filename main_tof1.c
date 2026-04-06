/**
 * @file    main_anchor.c
 * @brief   ESP32-S3 · WiFi FTM (802.11mc) Anchor Node -> MQTT
 *
 * Flow for Multiple Anchors:
 * 1. Connect to ROOT_AP (for MQTT backhaul) with a unique static IP.
 * 2. Every FTM_SCAN_INTERVAL_MS, scan for the mobile TARGET_SSID.
 * 3. If found, perform a WiFi FTM session against the target's MAC.
 * 4. Publish the exact requested schema to the MQTT broker.
 */

 #include <stdio.h>
 #include <stdlib.h>
 #include <string.h>
 #include <math.h>
 #include <stdbool.h>
 #include <arpa/inet.h>
 
 #include "freertos/FreeRTOS.h"
 #include "freertos/task.h"
 #include "freertos/event_groups.h"
 
 #include "nvs_flash.h"
 #include "esp_log.h"
 #include "esp_event.h"
 #include "esp_netif.h"
 #include "esp_wifi.h"
 #include "esp_timer.h"
 
 #include "lwip/sockets.h"
 #include "lwip/netdb.h"
 #include "lwip/err.h"
 
 /* ═══════════════════════════════════════════════════════════════════════════
  * ⚠️ CHANGE THESE VALUES FOR EACH PHYSICAL ANCHOR ⚠️
  * ═══════════════════════════════════════════════════════════════════════════ */
 #define DEVICE_ID               "TOF_ANCHOR_01"          /* Change to ANCHOR_02, etc. */
 #define STATIC_IP_ADDR          "192.168.1.61"       /* Must be unique per anchor */
 
 /* Known physical location of THIS anchor in the room (metres) */
 #define ANCHOR_X_M              -4.05f                 
 #define ANCHOR_Y_M              6.8f                 
 
 /* ═══════════════════════════════════════════════════════════════════════════
  * GLOBAL CONFIGURATION (Same across all anchors)
  * ═══════════════════════════════════════════════════════════════════════════ */
 /* Network & Target */
 #define WIFI_SSID               "ESP_RECEIVER"
 #define WIFI_PASS               ".esp_receiver_capstone_32"
 #define TARGET_SSID             "TOF_TAG-1"    /* The mobile tag being tracked */
 
 /* IP Gateway/Mask */
 #define STATIC_GATEWAY          "192.168.1.2"
 #define STATIC_NETMASK          "255.255.255.0"
 #define STATIC_DNS_PRIMARY      "192.168.1.2"
 
 /* MQTT Broker */
 #define MQTT_BROKER_HOST        "192.168.1.157"
 #define MQTT_BROKER_PORT        1883
 #define MQTT_TOPIC              "tof/measurements"
 #define MQTT_KEEPALIVE_SEC      60
 #define MQTT_SOCKET_TIMEOUT_SEC 5
 
 /* Location Metadata */
 #define LOCATION_ID             "LOC_01"
 #define FLOOR_ID                "FLOOR_01"
 #define ROOM_ID                 "ROOM_01"
 
 /* FTM Parameters */
 #define FTM_NUM_FRAMES          16
 #define FTM_BURST_PERIOD_MS     2
 #define FTM_SCAN_INTERVAL_MS    1500  /* Stagger this slightly across anchors */
 #define FTM_TIMEOUT_MS          5000
 
 #define SPEED_OF_LIGHT_MPS      299792458.0
 static const char *TAG = "TOF_ANCHOR";
 
 /* ═══════════════════════════════════════════════════════════════════════════
  * GLOBALS
  * ═══════════════════════════════════════════════════════════════════════════ */
 static EventGroupHandle_t   s_wifi_eg;
 static EventGroupHandle_t   s_ftm_eg;
 
 #define WIFI_CONNECTED_BIT      BIT0
 #define FTM_DONE_BIT            BIT0
 #define FTM_FAIL_BIT            BIT1
 
 static volatile uint32_t        s_scan_num    = 0;
 static int                      s_mqtt_sock   = -1;
 static uint16_t                 s_pkt_id      = 0;
 
 typedef struct {
     float   distance_m;
     float   confidence;
     int8_t  rssi;
     uint8_t bssid[6];
 } ftm_result_t;
 
 static ftm_result_t s_ftm_result;
 
 /* ═══════════════════════════════════════════════════════════════════════════
  * POSITIONING STUB
  * ═══════════════════════════════════════════════════════════════════════════ */
 static void compute_position(float dist_m, float *out_x, float *out_y)
 {
     /* Projects distance along the X axis from the anchor for the CSV payload */
     *out_x = ANCHOR_X_M + dist_m;
     *out_y = ANCHOR_Y_M;
 }
 
 /* ═══════════════════════════════════════════════════════════════════════════
  * EVENT HANDLERS
  * ═══════════════════════════════════════════════════════════════════════════ */
 static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
 {
     if (base == WIFI_EVENT) {
         switch (id) {
             case WIFI_EVENT_STA_START:
                 esp_wifi_connect();
                 break;
             case WIFI_EVENT_STA_DISCONNECTED: {
                 wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
                 ESP_LOGW(TAG, "STA disconnected (reason %d) - retrying", d->reason);
                 xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT);
                 esp_wifi_connect();
                 break;
             }
             case WIFI_EVENT_FTM_REPORT: {
                 wifi_event_ftm_report_t *r = (wifi_event_ftm_report_t *)data;
                 if (r->status == FTM_STATUS_SUCCESS) {
                     double rtt_ps = (double)r->rtt_est;
                     double dist   = (rtt_ps * 1e-12 * SPEED_OF_LIGHT_MPS) / 2.0;
 
                     s_ftm_result.distance_m = (float)dist;
                     s_ftm_result.confidence = (float)r->ftm_report_num_entries / 64.0f;
                     if (s_ftm_result.confidence > 1.0f) s_ftm_result.confidence = 1.0f;
 
                     xEventGroupSetBits(s_ftm_eg, FTM_DONE_BIT);
                 } else {
                     xEventGroupSetBits(s_ftm_eg, FTM_FAIL_BIT);
                 }
                 break;
             }
         }
     } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
         xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
     }
 }
 
 /* ═══════════════════════════════════════════════════════════════════════════
  * WiFi INIT
  * ═══════════════════════════════════════════════════════════════════════════ */
 static void wifi_init_sta(void)
 {
     s_wifi_eg = xEventGroupCreate();
     ESP_ERROR_CHECK(esp_netif_init());
     ESP_ERROR_CHECK(esp_event_loop_create_default());
 
     esp_netif_t *sta = esp_netif_create_default_wifi_sta();
     ESP_ERROR_CHECK(esp_netif_dhcpc_stop(sta));
 
     esp_netif_ip_info_t ip = {
         .ip.addr      = inet_addr(STATIC_IP_ADDR),
         .gw.addr      = inet_addr(STATIC_GATEWAY),
         .netmask.addr = inet_addr(STATIC_NETMASK),
     };
     ESP_ERROR_CHECK(esp_netif_set_ip_info(sta, &ip));
 
     wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
     ESP_ERROR_CHECK(esp_wifi_init(&cfg));
 
     ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
     ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
 
     wifi_config_t wc = {
         .sta = {
             .ssid     = WIFI_SSID,
             .password = WIFI_PASS,
             .threshold.authmode = WIFI_AUTH_WPA2_PSK,
         },
     };
 
     ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
     ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
     ESP_ERROR_CHECK(esp_wifi_start());
     xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
 }
 
 /* ═══════════════════════════════════════════════════════════════════════════
  * MQTT RAW TCP SOCKET
  * ═══════════════════════════════════════════════════════════════════════════ */
 static int mqtt_encode_remlen(uint8_t *buf, int len) {
     int pos = 0;
     do {
         uint8_t b = len % 128;
         len /= 128;
         if (len > 0) b |= 0x80;
         buf[pos++] = b;
     } while (len > 0 && pos < 4);
     return pos;
 }
 
 static bool mqtt_connect_broker(void) {
     if (s_mqtt_sock >= 0) { close(s_mqtt_sock); s_mqtt_sock = -1; }
     s_mqtt_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
     if (s_mqtt_sock < 0) return false;
 
     struct timeval tv = { .tv_sec = MQTT_SOCKET_TIMEOUT_SEC, .tv_usec = 0 };
     setsockopt(s_mqtt_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
     setsockopt(s_mqtt_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
 
     struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(MQTT_BROKER_PORT), .sin_addr.s_addr = inet_addr(MQTT_BROKER_HOST) };
     if (connect(s_mqtt_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
         close(s_mqtt_sock); s_mqtt_sock = -1; return false;
     }
 
     uint8_t vh[] = { 0x00, 0x04, 'M', 'Q', 'T', 'T', 0x04, 0x02, (uint8_t)((MQTT_KEEPALIVE_SEC >> 8) & 0xFF), (uint8_t)(MQTT_KEEPALIVE_SEC & 0xFF) };
     int cid_len = strlen(DEVICE_ID);
     int rem_len = sizeof(vh) + 2 + cid_len;
     
     uint8_t pkt[128]; int pos = 0;
     pkt[pos++] = 0x10;
     pos += mqtt_encode_remlen(&pkt[pos], rem_len);
     memcpy(&pkt[pos], vh, sizeof(vh)); pos += sizeof(vh);
     pkt[pos++] = (cid_len >> 8) & 0xFF; pkt[pos++] = cid_len & 0xFF;
     memcpy(&pkt[pos], DEVICE_ID, cid_len); pos += cid_len;
 
     if (send(s_mqtt_sock, pkt, pos, 0) != pos) { close(s_mqtt_sock); s_mqtt_sock = -1; return false; }
     
     uint8_t connack[4] = {0};
     if (recv(s_mqtt_sock, connack, sizeof(connack), 0) < 4 || connack[0] != 0x20 || connack[3] != 0x00) {
         close(s_mqtt_sock); s_mqtt_sock = -1; return false;
     }
     return true;
 }
 
 static bool mqtt_publish_raw(const char *topic, const char *payload) {
     if (s_mqtt_sock < 0) { if (!mqtt_connect_broker()) return false; }
     
     int topic_len = strlen(topic);
     int payload_len = strlen(payload);
     s_pkt_id++;
 
     int rem_len = 2 + topic_len + 2 + payload_len;
     uint8_t pkt[512]; int pos = 0;
 
     pkt[pos++] = 0x32;
     pos += mqtt_encode_remlen(&pkt[pos], rem_len);
     pkt[pos++] = (topic_len >> 8) & 0xFF; pkt[pos++] = topic_len & 0xFF;
     memcpy(&pkt[pos], topic, topic_len); pos += topic_len;
     pkt[pos++] = (s_pkt_id >> 8) & 0xFF; pkt[pos++] = s_pkt_id & 0xFF;
     memcpy(&pkt[pos], payload, payload_len); pos += payload_len;
 
     if (send(s_mqtt_sock, pkt, pos, 0) != pos) {
         close(s_mqtt_sock); s_mqtt_sock = -1; return false;
     }
     uint8_t puback[4] = {0};
     recv(s_mqtt_sock, puback, sizeof(puback), 0);
     return true;
 }
 
 /* ═══════════════════════════════════════════════════════════════════════════
  * FTM SESSION 
  * ═══════════════════════════════════════════════════════════════════════════ */
 static bool ftm_run_session(void)
 {
     wifi_scan_config_t sc = { .ssid = (uint8_t *)TARGET_SSID, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
     if (esp_wifi_scan_start(&sc, true) != ESP_OK) return false;
 
     uint16_t n = 0;
     esp_wifi_scan_get_ap_num(&n);
     if (n == 0) return false;
 
     wifi_ap_record_t *list = malloc(sizeof(wifi_ap_record_t) * n);
     esp_wifi_scan_get_ap_records(&n, list);
 
     wifi_ftm_initiator_cfg_t ftm_cfg = {0};
     bool found = false;
 
     for (int i = 0; i < n; i++) {
         if (strcmp((char *)list[i].ssid, TARGET_SSID) == 0) {
             memcpy(ftm_cfg.resp_mac, list[i].bssid, 6);
             ftm_cfg.channel = list[i].primary;
             s_ftm_result.rssi = list[i].rssi;
             found = true;
             break;
         }
     }
     free(list);
     if (!found) return false;
 
     ftm_cfg.frm_count = FTM_NUM_FRAMES;
     ftm_cfg.burst_period = FTM_BURST_PERIOD_MS;
     xEventGroupClearBits(s_ftm_eg, FTM_DONE_BIT | FTM_FAIL_BIT);
 
     if (esp_wifi_ftm_initiate_session(&ftm_cfg) != ESP_OK) return false;
 
     EventBits_t bits = xEventGroupWaitBits(s_ftm_eg, FTM_DONE_BIT | FTM_FAIL_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(FTM_TIMEOUT_MS));
     return (bits & FTM_DONE_BIT) != 0;
 }
 
 /* ═══════════════════════════════════════════════════════════════════════════
  * MAIN TASK
  * ═══════════════════════════════════════════════════════════════════════════ */
 static void tof_task(void *pv)
 {
     s_ftm_eg = xEventGroupCreate();
 
     while (1) {
         s_scan_num++;
         bool ok = ftm_run_session();
         
         float x = ANCHOR_X_M;
         float y = ANCHOR_Y_M;
 
         if (ok) {
             /* Map the distance into x/y coordinates */
             compute_position(s_ftm_result.distance_m, &x, &y);
         } else {
             /* Mark failed measurement with sentinel values */
             s_ftm_result.distance_m = -1.0f;
             s_ftm_result.confidence = 0.0f;
             s_ftm_result.rssi = -100;
         }
 
         int64_t ts_ms = (int64_t)(esp_timer_get_time() / 1000LL);
         
         /* _id: deterministic per-device per-scan identifier */
         char _id[48]; 
         snprintf(_id, sizeof(_id), "%s_%06lu", DEVICE_ID, (unsigned long)s_scan_num);
         
         char rssi_vec[32]; 
         snprintf(rssi_vec, sizeof(rssi_vec), "[%d]", (int)s_ftm_result.rssi);
 
         /* Build CSV payload matching your EXACT schema:
            _id,device_id,location_id,floor_id,room_id,timestamp,confidence,rssi_vector,x,y,scan_number 
         */
         char payload[320];
         snprintf(payload, sizeof(payload),
                  "%s,%s,%s,%s,%s,%lld,%.4f,%s,%.4f,%.4f,%lu",
                  _id, 
                  DEVICE_ID, 
                  LOCATION_ID, 
                  FLOOR_ID, 
                  ROOM_ID,
                  (long long)ts_ms, 
                  s_ftm_result.confidence, 
                  rssi_vec,
                  x, 
                  y, 
                  (unsigned long)s_scan_num);
 
         mqtt_publish_raw(MQTT_TOPIC, payload);
         vTaskDelay(pdMS_TO_TICKS(FTM_SCAN_INTERVAL_MS));
     }
 }
 
 void app_main(void)
 {
     esp_err_t ret = nvs_flash_init();
     if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
         ESP_ERROR_CHECK(nvs_flash_erase());
         nvs_flash_init();
     }
 
     wifi_init_sta();
     xTaskCreate(tof_task, "tof_task", 8192, NULL, 5, NULL);
 }