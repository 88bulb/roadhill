#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include "esp_log.h"

#include "esp_system.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"

// #include "bulbcast.h"

static const char *TAG = "bulbcode";

EventGroupHandle_t ble_events;
QueueHandle_t ble_queue;

#define ADV_START_COMPLETE  (1 << 0)
#define ADV_STOP_COMPLETE   (1 << 1)
/*
static const esp_ble_scan_params_t scan_params_default = {
    .scan_type = BLE_SCAN_TYPE_PASSIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x50,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE};
*/

static const esp_ble_adv_data_t adv_data_default = {
    .set_scan_rsp = false,
    .include_name = false,
    .include_txpower = false,
    .min_interval = 0x0000,
    .max_interval = 0x0000,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static const esp_ble_adv_params_t adv_params_default = {
    .adv_int_min = 0x20, // 0.625msec * 32 = 20ms
    .adv_int_max = 0x20, // 0.625msec * 32 = 20ms
    .adv_type = ADV_TYPE_NONCONN_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

uint8_t hex_to_u8(char high, char low) {
    uint8_t u8;
    if (low >= '0' && low <= '9') {
        u8 = low - '0';
    } else {
        u8 = low - 'a' + 10;
    }

    if (high >= '0' && high <= '9') {
        u8 += 16 * (high - '0');
    } else {
        u8 += 16 * (high - 'a' + 10);
    }

    return u8;
}

/**
 *  b0 1b c0 de         magic
 *  00                  seq number
 *  00 00 00 00         group id
 *  00 00               bit mask
 *  00 00               command number (0xff00 ~ 0xffff are reserved)
 */
void handle_mfr_data(uint8_t *bda, uint8_t *data, size_t data_len) {
/**
    if (data_len != 26) return;

    if (data[0] != 0xb0 || data[1] != 0x1b || data[2] != 0xc0 ||
        data[3] != 0xde)
        return;

    if (data[5] != boot_params[0] || data[6] != boot_params[1] ||
        data[7] != boot_params[2] || data[8] != boot_params[3])
        return;

    if (!((data[9] * 256 + data[10]) & my_bit_field))
        return;

    uint16_t cmd = (uint16_t)data[11] * 256 + (uint16_t)data[12];
    handle_bulbcode(cmd, &data[13]);
*/
}

void esp_gap_cb(esp_gap_ble_cb_event_t event,
                       esp_ble_gap_cb_param_t *param) {
    // uint8_t *mfr_data;
    // uint8_t mfr_data_len;
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        esp_ble_gap_start_scanning(0);
        ESP_LOGI(TAG, "starting ble scan (permanently)");
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
/*
        esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
        switch (scan_result->scan_rst.search_evt) {
        case ESP_GAP_SEARCH_INQ_RES_EVT: {
            mfr_data = esp_ble_resolve_adv_data(
                scan_result->scan_rst.ble_adv,
                ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE, &mfr_data_len);

            if (mfr_data_len > 0) {
                // b01bca57 or testcast
                if (!is_bulbcast(mfr_data) && !is_testcast(mfr_data))
                    return;
                char *bda_str = u8_to_hex(scan_result->scan_rst.bda, 6, bda_buf,
                                          sizeof(bda_buf));
                char *mfr_str =
                    u8_to_hex(mfr_data, mfr_data_len, mfr_buf, sizeof(mfr_buf));
                if (is_bulbcast(mfr_data)) {
                    printf("bulbcast %s %s\n", bda_str, mfr_str);
                } else if (is_testcast(mfr_data)) {
                    printf("testcast %s %s\n", bda_str, mfr_str);
                }
            }
        } break;
        default:
            break;
        }
*/
    } break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        ESP_LOGI(TAG, "ble scan started");
        break;
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT: {
        esp_ble_adv_params_t adv_params = adv_params_default;
        ESP_ERROR_CHECK(esp_ble_gap_start_advertising(&adv_params));
    } break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        xEventGroupSetBits(ble_events, ADV_START_COMPLETE);
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        xEventGroupSetBits(ble_events, ADV_STOP_COMPLETE);
        break;
    default:
        ESP_LOGI(TAG, "unhandled ble event %d in esp_gap_cb()", event);
    }
}

void ble_adv_scan(void *args) {
    uint8_t code[26] = {0xb0, 0x1b, 0xc0, 0xde, 0x00};
    char hex[34] = {0};
    uint8_t mac[6];
    uint8_t seq = 0;

    ble_events = xEventGroupCreate();
    ble_queue = xQueueCreate(8, 34); // 34 (hex) chars -> 17 bytes

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    code[5] = mac[2];
    code[6] = mac[3];
    code[7] = mac[4];
    code[8] = mac[5];

/**
    esp_ble_scan_params_t scan_params = scan_params_default;
    ESP_ERROR_CHECK(esp_ble_gap_set_scan_params(&scan_params));
*/

    while (1) {
        if (xQueueReceive(ble_queue, hex, portMAX_DELAY) != pdTRUE) {
            ESP_LOGE(TAG, "Queue receive error");
            continue;
        }

        code[4] = seq++;

        for (int i = 0; i < 17; i++) {
            code[i + 9] = hex_to_u8(hex[2 * i], hex[2 * i + 1]);
        }

        esp_ble_adv_data_t adv_data = adv_data_default;
        adv_data.p_manufacturer_data = code;
        adv_data.manufacturer_len = 26;
        ESP_ERROR_CHECK(esp_ble_gap_config_adv_data(&adv_data));

        // wait advertising started
        xEventGroupWaitBits(ble_events, ADV_START_COMPLETE, pdFALSE, pdFALSE,
                            0);
        xEventGroupClearBits(ble_events, ADV_START_COMPLETE);
        ESP_LOGI(TAG, "advertising started");

        vTaskDelay(40 / portTICK_PERIOD_MS);

        ESP_ERROR_CHECK(esp_ble_gap_stop_advertising());

        // wait advertising started
        xEventGroupWaitBits(ble_events, ADV_STOP_COMPLETE, pdFALSE, pdFALSE, 0);
        xEventGroupClearBits(ble_events, ADV_STOP_COMPLETE);
        ESP_LOGI(TAG, "advertising stopped");
    }
}


