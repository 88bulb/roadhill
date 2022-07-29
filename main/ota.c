#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_http_client.h"
// #include "esp_ota_ops.h"
#include "esp_https_ota.h"

#include "message.h"
#include "ota.h"

static const char *TAG = "ota";

static esp_err_t ota_http_event_handler(esp_http_client_event_t *evt);
static void http_ota(void *arg);

void create_ota_task() {
  xTaskCreate(http_ota, "http_ota", 8192, NULL, 11, NULL);
}

static esp_err_t ota_http_event_handler(esp_http_client_event_t *evt) {
  switch (evt->event_id) {
  case HTTP_EVENT_ERROR:
    ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
    break;
  case HTTP_EVENT_HEADER_SENT:
    ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
    break;
  case HTTP_EVENT_ON_HEADER:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key,
             evt->header_value);
    break;
  case HTTP_EVENT_ON_DATA:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
    break;
  case HTTP_EVENT_ON_FINISH:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
    break;
  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
    break;
  }
  return ESP_OK;
}

static void http_ota(void *arg) {
  msg_t msg;
  result_t ota_done;

  recv_msg(OTA_QUEUE, &msg, true);
  ota_t *ota = msg.data;

  esp_http_client_config_t config = {
      .url = ota->url,
      //        .cert_pem = (char *)server_cert_pem_start,
      .event_handler = ota_http_event_handler,
      .keep_alive_enable = true,
  };

  ota_done.result = esp_https_ota(&config);
  free(ota->url);

  if (ota_done.result == ESP_OK) {
    ESP_LOGI(TAG, "ota succeeded");
  } else {
    ESP_LOGI(TAG, "ota failed, %s", esp_err_to_name(ota_done.result));
  }

  msg.type = OTA_DONE;
  msg.data = &ota_done;
  send_msg(MAIN_QUEUE, &msg);
  vTaskDelay(portMAX_DELAY);
}
