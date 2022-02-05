#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp_netif.h"

#include "esp_peripherals.h"

// #include "periph_wifi.h"

static const char *TAG = "roadhill_main";

static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;
static char test_token[32] = "roadhill_test";
static char prod_token[32] = "roadhill_prod";
static char *ap_tokens[2] = {test_token, prod_token};
static wifi_ap_record_t ap = {0};
static bool using_testing_ap = false;

esp_err_t wifi_scan(char **tokens, int num_of_tokens, int *token_index,
                    wifi_ap_record_t *ap) {
    esp_err_t err;
    static wifi_ap_record_t ap_record[20] = {};
    uint16_t max_num_of_ap_records = 20;
    uint16_t num_of_aps = 0;

    err = esp_wifi_scan_get_ap_records(&max_num_of_ap_records, ap_record);
    if (err != ESP_OK)
        return err;

    err = esp_wifi_scan_get_ap_num(&num_of_aps);
    if (err != ESP_OK)
        return err;

    *token_index = -1;
    for (int j = 0; j < num_of_tokens; j++) {
        for (uint16_t i = 0; i < num_of_aps; i++) {
            char *ssid_str = (char *)ap_record[i].ssid;
            if (strstr(ssid_str, tokens[j])) {
                *token_index = j;
                if (ap)
                    memcpy(ap, &ap_record[i], sizeof(wifi_ap_record_t));
                break;
            }
        }

        if (*token_index >= 0) {
            break;
        }
    }

    return ESP_OK;
}

void app_main(void) {
    esp_err_t err;

    // init nvs
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    // init event loop and wifi
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);

    ESP_LOGI(TAG, "size of index: %d", sizeof(index));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // loop forever if neither test or prod ap found
    while (1) {
        int token_index;
        memset(&ap, 0, sizeof(ap));
        ESP_ERROR_CHECK(wifi_scan(ap_tokens, 2, &token_index, &ap));
        if (token_index >= 0) {
            using_testing_ap = !!(token_index == 0);
            if (using_testing_ap) {
                ESP_LOGI(TAG, "found test ap: %s", (char *)ap.ssid);
            } else {
                ESP_LOGI(TAG, "found prod ap: %s", (char *)ap.ssid);
            }
            break;
        } else {
            ESP_LOGI(TAG, "neither test nor prod ap found.");
            vTaskDelay(3000 / portTICK_PERIOD_MS);
        }
    }

    // now we have the ssid

    while (1) {
        ESP_LOGI(TAG, "do nothing");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
