#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

// #include "lwip/err.h"
#include "lwip/sockets.h"

#include "cJSON.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_rom_md5.h"
#include "nvs_flash.h"

#include "esp_netif.h"

#include "esp_peripherals.h"

// #include "periph_wifi.h"

#define TCP_PORT (6015)

static const char *TAG = "roadhill:main";

const char hex_char[16] = "0123456789abcdef";

#define EB_STA_GOT_IP ((EventBits_t)(1 << 0))
#define EB_OTA_REQUESTED ((EventBits_t)(1 << 1))
static EventGroupHandle_t event_bits;

static QueueHandle_t http_ota_queue;
static QueueHandle_t tcp_send_queue;
static QueueHandle_t juggle_jobs_queue;

static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;

static esp_event_handler_instance_t instance_any_wifi_event;
static esp_event_handler_instance_t instance_any_ip_event;

static wifi_ap_record_t ap_record[20] = {0};

static char test_token[32] = "juwanke-test";
static char prod_token[32] = "juwanke";
static wifi_ap_record_t ap = {0};
static bool using_test_ap = false;
static uint8_t sta_mac[6] = {0};
static uint8_t ap_mac[6] = {0};
static bool wifi_scanning = false;

extern const wifi_config_t sta_config_test_default;
extern const wifi_config_t sta_config_prod_default;
extern const wifi_config_t ap_config_default;

static char tx_buf[4096] = {0};
static int tx_len = 0;
static char rx_buf[4096] = {0};

#define LINE_LENGTH (256 * 1024)

static char *line;
static int llen = 0;

static const char rev_token[] = "**";
static const char ver_token[] = "00000000";
static const char sha_token[] = "0000000000000000";
static const char device_info_tmpl[] =
    "{\"type\":\"DEVICE_INFO\",\"hardware\":{\"codename\":\"roadhill\","
    "\"revision\":\"**\"},\"firmware\":{\"version\":\"00000000\",\"sha256\":"
    "\"0000000000000000000000000000000000000000000000000000000000000000\"}}\n";

typedef enum {
    CMD_UNDEFINED = 0,
    CMD_OTA,
    CMD_SET_SESSION,
    CMD_PLAY,
    CMD_STOP,
} command_type_t;

#define OTA_URL_STRING_BUFFER_SIZE (1024)
typedef struct {
    char url[OTA_URL_STRING_BUFFER_SIZE];
} ota_command_data_t;

typedef struct {
    uint8_t bytes[16];
} track_digest_t;

#define URL_BUFFER_SIZE (1024)
#define TRACK_NAME_LENGTH (32)
#define FILENAME_BUFFER_SIZE (40)
typedef struct {
    char tracks_url[URL_BUFFER_SIZE];
    track_digest_t current_track;
    int next_tracks_num;
    char next_tracks[16][FILENAME_BUFFER_SIZE];
} play_command_data_t;

typedef enum { BULB_NOTFOUND = 0, BULB_INVITING, BULB_READY } bulb_state_t;

typedef struct {
    uint8_t addr;
    uint16_t bitmask;
} bulb_t;

static bool is_semver(const char *str) {
    if (strlen(str) != 8)
        return false;

    for (int i = 0; i < 8; i++) {
        if (str[i] >= '0' && str[i] <= '9')
            continue;
        if (str[i] >= 'a' && str[i] <= 'f')
            continue;
        if (str[i] >= 'A' && str[i] <= 'F')
            continue;
        return false;
    }

    if (str[6] == 'd' || str[6] == 'e' || str[6] == 'f')
        return false;
    if (str[6] == 'D' || str[6] == 'E' || str[6] == 'F')
        return false;
    return true;
}

static void prepare_device_info() {
    char *str;
    strcpy(tx_buf, device_info_tmpl);

    str = strstr(tx_buf, rev_token);
    str[0] = 'a';
    str[1] = '0';

    const esp_app_desc_t *app_desc = esp_ota_get_app_description();
    if (is_semver(app_desc->version)) {
        str = strstr(tx_buf, ver_token);
        for (int i = 0; i < 8; i++) {
            *str++ = app_desc->version[i];
        }
    }

    uint8_t sha[32];
    const esp_partition_t *part = esp_ota_get_running_partition();
    if (part) {
        esp_err_t err = esp_partition_get_sha256(part, sha);
        if (err == ESP_OK) {
            str = strstr(tx_buf, sha_token);
            for (int i = 0; i < 32; i++) {
                str[2 * i] = hex_char[sha[i] / 16];
                str[2 * i + 1] = hex_char[sha[i] % 16];
            }
        }
    }
    tx_len = strlen(tx_buf);
}

bool is_valid_track_name(char *name) {
    if (strlen(name) != 36)
        return false;
    if (0 != strcmp(&name[32], ".mp3"))
        return false;
    for (int i = 0; i < 32; i++) {
        if (name[i] >= '0' && name[i] <= '9')
            continue;
        if (name[i] >= 'a' && name[i] <= 'f')
            continue;
        return false;
    }
    return true;
}

/**
 * name must be a 32-character hex string [0-9a-f], there is no check inside the
 * function.
 */
track_digest_t track_name_to_digest(char *name) {
    track_digest_t digest;
    for (int i = 0; i < 16; i++) {
        uint8_t u8;
        char high = name[2 * i];
        char low = name[2 * i + 1];

        if (high >= 'a' && high <= 'f') {
            u8 = (high - 'a') << 4;
        } else {
            u8 = (high - '0') << 4;
        }

        if (low >= 'a' && low <= 'f') {
            u8 += low - 'a';
        } else {
            u8 += low - '0';
        }

        digest.bytes[i] = u8;
    }
    return digest;
}

static int process_line() {
    command_type_t cmd_type;
    void *data = NULL;
    int err = 0;

    ESP_LOGI(TAG, "process line: %s", line);
    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        ESP_LOGI(TAG, "failed to parse json");
        err = -1;
        goto finish;
    }

    char *cmd = cJSON_GetObjectItem(root, "cmd")->valuestring;
    if (cmd == NULL) {
        ESP_LOGI(TAG, "no cmd property");
        err = -1;
        goto finish;
    }

    if (0 == strcmp(cmd, "OTA")) {
        cmd_type = CMD_OTA;
        data = malloc(sizeof(ota_command_data_t));
        if (data == NULL) {
            err = -1;
            ESP_LOGI(TAG, "no memory");
            goto finish;
        }

        ota_command_data_t *p = (ota_command_data_t *)data;

        char *url = cJSON_GetObjectItem(root, "url")->valuestring;
        if (url == NULL) {
            ESP_LOGI(TAG, "ota command without url property");
            err = -1;
            goto finish;
        }

        if (!(strlen(url) < OTA_URL_STRING_BUFFER_SIZE)) {
            ESP_LOGI(TAG, "ota command url too long");
            err = -1;
            goto finish;
        }
        strcpy(p->url, url);

        if (pdTRUE != xQueueSend(http_ota_queue, &p, 0)) {
            err = -1;
            ESP_LOGI(TAG, "failed to enqueue ota request");
            goto finish;
        }

        data = NULL;
        ESP_LOGI(TAG, "ota request queued, url: %s", url);
    } else if (0 == strcmp(cmd, "PLAY")) {
        cmd_type = CMD_PLAY;
        data = malloc(sizeof(play_command_data_t));
        if (data == NULL) {
            err = -1;
            ESP_LOGI(TAG, "no memory");
            goto finish;
        }

        play_command_data_t *p = (play_command_data_t *)data;

        char *tracks_url = cJSON_GetObjectItem(root, "tracks_url")->valuestring;
        if (tracks_url == NULL) {
            err = -1;
            ESP_LOGI(TAG, "play command has no tracks_url");
            goto finish;
        }
        if (!(strlen(tracks_url) + TRACK_NAME_LENGTH < URL_BUFFER_SIZE)) {
            err = -1;
            ESP_LOGI(TAG, "play command tracks_url too long");
            goto finish;
        }
        strcpy(p->tracks_url, tracks_url);

        char *current_track =
            cJSON_GetObjectItem(root, "current_track")->valuestring;
        if (current_track == NULL) {
            err = -1;
            ESP_LOGI(TAG, "play command has no current_track");
            goto finish;
        }
        if (!is_valid_track_name(current_track)) {
            err = -1;
            ESP_LOGI(TAG, "play command current_track invalid");
            goto finish;
        }
        p->current_track = track_name_to_digest(current_track);

        if (pdTRUE != xQueueSend(juggle_jobs_queue, &p, 0)) {
            err = -1;
            ESP_LOGI(TAG, "failed to enqueue play request");
            goto finish;
        }

        data = NULL;
        ESP_LOGI(TAG, "play request enqueued");
    }

finish:
    if (data)
        free(data);
    if (root)
        cJSON_Delete(root);
    llen = 0;
    return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (wifi_scanning)
        return;

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
            xEventGroupSetBits(event_bits, EB_STA_GOT_IP);
            break;
        case IP_EVENT_STA_LOST_IP:
            xEventGroupClearBits(event_bits, EB_STA_GOT_IP);
            break;
        default:
            break;
        }
    }
}

static void tcp_send(void *arg) {
    void *handle = NULL;

    while (xQueueReceive(tcp_send_queue, &handle, portMAX_DELAY)) {
        free(handle);
        handle = NULL;
    }
}

static void tcp_receive(void *arg) {
    esp_err_t err;

    // (re-)connection in loop
    while (1) {
        xEventGroupWaitBits(event_bits, EB_STA_GOT_IP, pdFALSE, pdFALSE,
                            portMAX_DELAY);

        esp_netif_ip_info_t ip_info;
        err = esp_netif_get_ip_info(sta_netif, &ip_info);
        if (err != ESP_OK) {
            ESP_LOGI(TAG, "failed to get ip info");
            goto closed;
        }

        int tcp_port = 8080;

        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = ip_info.gw.addr;
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(tcp_port);
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGI(TAG, "failed to create socket");
            goto closed;
        }

        err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGI(TAG, "failed to connect to tcp server (%d)", errno);
            goto closing;
        } else {
            ESP_LOGI(TAG, "connected to tcp server");
        }

        prepare_device_info();

        if (tx_len) {
            int start = 0;
            while (1) {
                int sent = send(sock, &tx_buf[start], tx_len - start, 0);
                if (sent < 0) {
                    ESP_LOGI(TAG, "send error (%d)", errno);
                    goto closing;
                }
                start += sent;
                if (start < tx_len) {
                    vTaskDelay(0);
                } else {
                    break;
                }
            };
        }

        while (1) {
            int len = recv(sock, rx_buf, sizeof(rx_buf), 0);
            if (len < 0) {
                ESP_LOGI(TAG, "recv error (%d)", errno);
                goto closing;
            }

            for (int i = 0; i < len; i++) {
                if (rx_buf[i] == '\r' || rx_buf[i] == '\n') {
                    if (llen > 0) {
                        line[llen] = '\0';
                        // TODO
                        if (process_line()) {
                            goto closing;
                        }

                        llen = 0;
                    }
                } else {
                    line[llen++] = rx_buf[i];
                    if (llen >= LINE_LENGTH - 1) {
                        ESP_LOGI(TAG, "received line too long");
                        goto closing;
                    }
                }
            }
        }

    closing:
        // shutdown(sock, 0);
        close(sock);
        llen = 0;
    closed:
        vTaskDelay(8000 / portTICK_PERIOD_MS);
    }
}

esp_err_t ota_http_event_handler(esp_http_client_event_t *evt) {
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
    ota_command_data_t *cmd;
    xQueueReceive(http_ota_queue, &cmd, portMAX_DELAY);

    esp_http_client_config_t config = {
        .url = cmd->url,
        //        .cert_pem = (char *)server_cert_pem_start,
        .event_handler = ota_http_event_handler,
        .keep_alive_enable = true,
    };

    esp_err_t err = esp_https_ota(&config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ota succeeded");
    } else {
        ESP_LOGI(TAG, "ota failed, %s", esp_err_to_name(err));
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_restart();
}

static void fetch_track(void *arg) {}

static void juggle_jobs(void *arg) {
    play_command_data_t *cmd;
    while (xQueueReceive(juggle_jobs_queue, &cmd, portMAX_DELAY)) {
        ESP_LOGI(TAG, "play request received");
    }
}

void app_main(void) {
    // esp_err_t is typedef-ed int, so it could be used with lwip/sockets
    // api, but the value should be interpretted differently.
    esp_err_t err;
    int i, j;

    line = (char *)malloc(LINE_LENGTH);
    memset(line, 0, LINE_LENGTH);

    event_bits = xEventGroupCreate();

    http_ota_queue = xQueueCreate(1, sizeof(void *));
    xTaskCreate(http_ota, "http_ota", 65536, NULL, 11, NULL);

    juggle_jobs_queue = xQueueCreate(2, sizeof(void *));
    xTaskCreate(juggle_jobs, "juggle_jobs", 65536, NULL, 11, NULL);

    tcp_send_queue = xQueueCreate(20, sizeof(void *));
    xTaskCreate(tcp_send, "tcp_send", 4096, NULL, 9, NULL);
    xTaskCreate(tcp_receive, "tcp_receive", 4096, NULL, 15, NULL);

    // init nvs
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    // init event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // this operations can not be done
    // otherwise it competes with scanning
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
        &instance_any_wifi_event));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL,
        &instance_any_ip_event));

    // init sta interface and mac
    ESP_ERROR_CHECK(esp_netif_init());
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    // init ap interface and mac
    ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);

    // init wifi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // get mac and log
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, sta_mac));
    ESP_LOGI(TAG, "sta mac: %02x:%02x:%02x:%02x:%02x:%02x", sta_mac[0],
             sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, ap_mac));
    ESP_LOGI(TAG, "ap mac: %02x:%02x:%02x:%02x:%02x:%02x", ap_mac[0], ap_mac[1],
             ap_mac[2], ap_mac[3], ap_mac[4], ap_mac[5]);

    // start wifi for scanning
    wifi_scanning = true;
    ESP_ERROR_CHECK(esp_wifi_start());

    // loop forever if neither test or prod ap found
    while (1) {
        uint16_t max_num_of_ap_records = 20;
        uint16_t num_of_aps = 0;

        memset(&ap, 0, sizeof(ap));
        ESP_ERROR_CHECK(esp_wifi_scan_start(NULL, true));
        ESP_ERROR_CHECK(
            esp_wifi_scan_get_ap_records(&max_num_of_ap_records, ap_record));
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&num_of_aps));

        for (i = 0; i < num_of_aps; i++) {
            ESP_LOGI(TAG, "ap[%d].ssid: %s", i, (char *)ap_record[i].ssid);
        }

        // find test ap token
        for (i = 0; i < num_of_aps; i++) {
            char *ssid_str = (char *)ap_record[i].ssid;
            if (strstr(ssid_str, test_token)) {
                memcpy(&ap, &ap_record[i], sizeof(wifi_ap_record_t));
                using_test_ap = true;
                break;
            }
        }

        if (i < num_of_aps)
            break;

        // find prod ap (exact match)
        for (i = 0; i < num_of_aps; i++) {
            char *ssid_str = (char *)ap_record[i].ssid;
            if (0 == strcmp(ssid_str, prod_token)) {
                memcpy(&ap, &ap_record[i], sizeof(wifi_ap_record_t));
                break;
            }
        }

        if (i < num_of_aps)
            break;

        ESP_LOGI(TAG, "neither test nor prod ap found.");
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }

    ESP_ERROR_CHECK(esp_wifi_stop());
    wifi_scanning = false;

    if (using_test_ap) {
        ESP_LOGI(TAG, "found test ap: %s", (char *)ap.ssid);
    } else {
        ESP_LOGI(TAG, "found prod ap: %s", (char *)ap.ssid);
    }

    // now we have the ssid, try to establish tcp connection with server
    // in case of prod ap, use ????
    // in case of test ap, use port 6015
    wifi_config_t sta_cfg;
    if (using_test_ap) {
        sta_cfg = sta_config_test_default;
    } else {
        sta_cfg = sta_config_prod_default;
    }
    strlcpy((char *)sta_cfg.sta.ssid, (char *)ap.ssid,
            sizeof(sta_cfg.sta.ssid));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    wifi_config_t ap_cfg = ap_config_default;
    strlcpy((char *)ap_cfg.ap.ssid, "juwanke-soundbar-",
            sizeof(ap_cfg.ap.ssid));

    j = strlen("juwanke-soundbar-");
    for (i = 0; i < 6; i++) {
        ap_cfg.ap.ssid[j++] = hex_char[sta_mac[i] / 16];
        ap_cfg.ap.ssid[j++] = hex_char[sta_mac[i] % 16];
    }

    ESP_LOGI(TAG, "ap ssid: %s", (char *)ap_cfg.ap.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    vTaskDelay(portMAX_DELAY);
}
