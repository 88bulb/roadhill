#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp_netif.h"

#include "esp_peripherals.h"
// #include "periph_wifi.h"

static const char *TAG = "roadhill_main";

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());

    while (1) {
        ESP_LOGI(TAG, "do nothing");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
