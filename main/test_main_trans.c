#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"

#include "esp_rom_md5.h"

#include "message.h"
#include "tools.h"
#include "wget.h"
#include "transcode.h"

#define TEST_WIFI_SSID "juwanke-test-a5a5a5a5"
#define TEST_WIFI_PASSWORD "6ul6600t"

static const char *TAG = "testing_trans_main";

static const char *test_md5hex = "894d204819a508c893912166d8746338";
static const size_t test_file_size = 1009228;
static const char *test_base_url = "http://10.42.0.1:8080/files/album000001";

static uint8_t sta_mac[6] = {0};
static esp_netif_t *sta_netif = NULL;
static esp_event_handler_instance_t instance_any_wifi_event;
static esp_event_handler_instance_t instance_any_ip_event;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);

void app_main(void) {
  create_msg_queues();

  // init nvs
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
    // NVS partition was truncated and needs to be erased
    // Retry nvs_flash_init
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }

  // create_wget_task(); TODO

  // init event loop
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  sta_netif = esp_netif_create_default_wifi_sta();
  assert(sta_netif);

  // init wifi
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // get mac and log
  ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, sta_mac));
  ESP_LOGI(TAG, "sta mac: %02x:%02x:%02x:%02x:%02x:%02x", sta_mac[0],
           sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);

  // register handlers
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
      &instance_any_wifi_event));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
      &instance_any_ip_event));

  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = TEST_WIFI_SSID, .password = TEST_WIFI_PASSWORD,
              // .threshold.authmode =
              //              ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD, .sae_pwe_h2e =
              //              2,
          },
  };

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  while (1) {
    /* There was once a stupid bug in this loop.
     * Only one msg_t is used for both incoming and outgoing message.
     * free() twice corrupts heap.
     */
    msg_t in_msg = {};
    msg_t out_msg = {};

    recv_msg(MAIN_QUEUE, &in_msg, true);
    switch (in_msg.type) {
    case STA_GOT_IP: {
      ESP_LOGI(TAG, "sta got ip");
      assert(in_msg.data == NULL);

      wget_t *data = (wget_t *)malloc(sizeof(wget_t));
      size_t url_size = strlen(test_base_url) + 1;
      data->base_url = (char *)malloc(url_size);
      strlcpy(data->base_url, test_base_url, url_size);
      data->size = test_file_size;
      data->md5 = make_md5_digest(test_md5hex);

      out_msg.type = WGET;
      out_msg.data = data;

      // how about queue full?
      send_msg(WGET_QUEUE, &out_msg);
      break;
    }

    case STA_LOST_IP: {
      ESP_LOGI(TAG, "sta lost ip");
      assert(in_msg.data == NULL);
      break;
    }

    case WGET_DATA: {
      chunk_t *chunk = (chunk_t *)in_msg.data;
      ESP_LOGI(TAG, "chunk length: %d", chunk->len);
      assert(in_msg.data != NULL);
      free(chunk->data);
      free(chunk);
      break;
    }
    default:
      break;
    }
  }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  msg_t msg;

  if (event_base == WIFI_EVENT) {
    switch (event_id) {
    case WIFI_EVENT_STA_START:
      ESP_LOGI(TAG, "wifi event: sta_start, connecting to ap");
      esp_wifi_connect();
      break;
    case WIFI_EVENT_STA_DISCONNECTED:
      ESP_LOGI(TAG, "wifi event: sta_disconnected, reconnecting to ap");
      esp_wifi_connect();
    default:
      break;
    }
  }

  if (event_base == IP_EVENT) {
    switch (event_id) {
    case IP_EVENT_STA_GOT_IP:
      msg.type = STA_GOT_IP;
      msg.data = NULL;
      send_msg(MAIN_QUEUE, &msg);
      break;
    case IP_EVENT_STA_LOST_IP:
      msg.type = STA_LOST_IP;
      msg.data = NULL;
      send_msg(MAIN_QUEUE, &msg);
      break;
    default:
      break;
    }
  }
}
