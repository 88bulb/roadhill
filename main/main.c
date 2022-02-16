#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"

#include "cJSON.h"

#include "esp_log.h"

#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_host.h"

#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_rom_md5.h"
#include "esp_vfs_fat.h"
#include "esp_rom_md5.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"

#include "esp_debug_helpers.h"

#include "esp_netif.h"
#define MOUNT_POINT "/emmc"
#define CHUNK_DIR(x) "/emmc/00/" x
#define CHUNK_FILE_SIZE (1024 * 1024)

#define TEMP_FILE_PATH "/emmc/temp"

#define MEM_BLOCK_SIZE (32768)

#include "roadhill.h"

#define TCP_PORT (6015)

extern esp_err_t esp_vfs_exfat_sdmmc_mount(
    const char *base_path, const sdmmc_host_t *host_config,
    const void *slot_config, const esp_vfs_fat_mount_config_t *mount_config,
    sdmmc_card_t **out_card);

extern void audible(void* arg); 

static const char *TAG = "roadhill";
const char hex_char[16] = "0123456789abcdef";

#define EB_STA_GOT_IP ((EventBits_t)(1 << 0))
#define EB_OTA_REQUESTED ((EventBits_t)(1 << 1))
static EventGroupHandle_t event_bits;

static QueueHandle_t http_ota_queue;

QueueHandle_t tcp_send_queue;
QueueHandle_t juggler_queue;
QueueHandle_t audible_queue;

static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;

static esp_event_handler_instance_t instance_any_wifi_event;
static esp_event_handler_instance_t instance_any_ip_event;

// static wifi_ap_record_t ap_record[20] = {0};
static wifi_ap_record_t* ap_record = NULL;

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

static char* tx_buf = NULL;
static int tx_len = 0;
static char* rx_buf = NULL;

#define LINE_LENGTH (256 * 1024)

static char *line;
static int llen = 0;

static const char rev_token[] = "**";
static const char mac_token[] = "FF:FF:FF:FF:FF:FF";
static const char ver_token[] = "00000000";
static const char sha_token[] = "0000000000000000";
static const char device_info_tmpl[] =
    "{\"type\":\"DEVICE_INFO\",\"hardware\":{\"codename\":\"roadhill\","
    "\"revision\":\"**\",\"mac_addr\":\"FF:FF:FF:FF:FF:FF\"},\"firmware\":{"
    "\"version\":\"00000000\",\"sha256\":"
    "\"0000000000000000000000000000000000000000000000000000000000000000\"}}\n";

typedef enum {
    CMD_UNDEFINED = 0,
    CMD_OTA,
    CMD_SET_SESSION,
    CMD_PLAY,
    CMD_STOP,
} command_type_t;

static ota_command_data_t *ota_command_data;

typedef enum { BULB_NOTFOUND = 0, BULB_INVITING, BULB_READY } bulb_state_t;

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
md5_digest_t track_name_to_digest(char *name) {

    md5_digest_t digest;
    for (int i = 0; i < 16; i++) {
        uint8_t u8;
        char high = name[2 * i];
        char low = name[2 * i + 1];

        if (high >= 'a' && high <= 'f') {
            u8 = (high - 'a' + 10) << 4;
        } else {
            u8 = (high - '0') << 4;
        }

        if (low >= 'a' && low <= 'f') {
            u8 += low - 'a' + 10;
        } else {
            u8 += low - '0';
        }

        digest.bytes[i] = u8;
    }
    return digest;
}

void track_url_strlcat(char *tracks_url, md5_digest_t digest, size_t size) {
    char str[40] = {0};

    for (int i = 0; i < 16; i++) {
        str[2 * i] = hex_char[digest.bytes[i] / 16];
        str[2 * i + 1] = hex_char[digest.bytes[i] % 16];
    }
    str[32] = '.';
    str[33] = 'm';
    str[34] = 'p';
    str[35] = '3';
    str[36] = '\0';
    strlcat(tracks_url, "/", size);
    strlcat(tracks_url, str, size);
}

static void prepare_device_info() {
    char *str;
    strcpy(tx_buf, device_info_tmpl);

    str = strstr(tx_buf, rev_token);
    str[0] = 'a';
    str[1] = '0';

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    str = strstr(tx_buf, mac_token);
    str[0x00] = hex_char[mac[0] / 16]; 
    str[0x01] = hex_char[mac[0] % 16];
    str[0x02] = ':';
    str[0x03] = hex_char[mac[1] / 16];
    str[0x04] = hex_char[mac[1] % 16];
    str[0x05] = ':';
    str[0x06] = hex_char[mac[2] / 16];
    str[0x07] = hex_char[mac[2] % 16];
    str[0x08] = ':';
    str[0x09] = hex_char[mac[3] / 16];
    str[0x0a] = hex_char[mac[3] % 16];
    str[0x0b] = ':';
    str[0x0c] = hex_char[mac[4] / 16];
    str[0x0d] = hex_char[mac[4] % 16];
    str[0x0e] = ':';
    str[0x0f] = hex_char[mac[5] / 16];
    str[0x10] = hex_char[mac[5] % 16];

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

        char *url = cJSON_GetObjectItem(root, "url")->valuestring;
        if (url == NULL) {
            ESP_LOGI(TAG, "ota command without url property");
            err = -1;
            goto finish;
        }

        // alternatively, sizeof((ota_command_data_t *)0)->url)
        if (!(strlen(url) < sizeof(ota_command_data->url))) {
            ESP_LOGI(TAG, "ota command url too long");
            err = -1;
            goto finish;
        }
        strcpy(ota_command_data->url, url);

        // actually, any message will do.
        message_t msg = {};
        msg.type = MSG_CMD_OTA;
        if (pdTRUE != xQueueSend(http_ota_queue, &msg, 0)) {
            err = -1;
            ESP_LOGI(TAG, "failed to enqueue ota request");
            goto finish;
        }

        data = NULL;
        ESP_LOGI(TAG, "ota request queued, url: %s", url);
    } else if (0 == strcmp(cmd, "PLAY")) {
        cmd_type = CMD_PLAY;

        // TODO change to read from queue (allocated by juggler)
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

        const cJSON *tracks = cJSON_GetObjectItem(root, "tracks");
        if (!cJSON_IsArray(tracks)) {
            err = -1;
            ESP_LOGI(TAG, "tracks is not an array");
            goto finish;
        }

        p->tracks_array_size = cJSON_GetArraySize(tracks);
        p->tracks = (track_t *)malloc(p->tracks_array_size * sizeof(track_t));
        if (p->tracks == NULL) {
            err = -1;
            ESP_LOGI(TAG, "failed to allocate memory for tracks");
            goto finish;
        }

        // TODO validation
        for (int i = 0; i < p->tracks_array_size; i++) {
            cJSON *item = cJSON_GetArrayItem(tracks, i);
            p->tracks[i].digest = track_name_to_digest(
                cJSON_GetObjectItem(item, "name")->valuestring);
            p->tracks[i].size = cJSON_GetObjectItem(item, "size")->valueint;
        }

        const cJSON *blinks = cJSON_GetObjectItem(root, "blinks");
        if (!cJSON_IsArray(blinks)) {
            err = -1;
            ESP_LOGI(TAG, "blinks is not an array");
            goto finish;
        }

        p->blinks_array_size = cJSON_GetArraySize(blinks);
        p->blinks = (blink_t *)malloc(p->blinks_array_size * sizeof(blink_t));
        if (p->blinks == NULL) {
            err = -1;
            ESP_LOGI(TAG, "failed to allocate memory for blinks");
            goto finish;
        }

        for (int i = 0; i < p->blinks_array_size; i++) {
            cJSON *item = cJSON_GetArrayItem(blinks, i);
            p->blinks[i].time = cJSON_GetObjectItem(item, "time")->valueint;
            const char* mask = cJSON_GetObjectItem(item, "mask")->valuestring;
            const char* code = cJSON_GetObjectItem(item, "code")->valuestring;
            p->blinks[i].code[0] = mask[0];
            p->blinks[i].code[1] = mask[1];
            p->blinks[i].code[2] = mask[2];
            p->blinks[i].code[3] = mask[3];
            for (int j = 0; j < 30; j++) {
                p->blinks[i].code[j + 4] = code[j];
            }
        }

        message_t msg = {};
        msg.type = MSG_CMD_PLAY;
        msg.data = p;
        if (pdTRUE != xQueueSend(juggler_queue, &msg, 0)) {
            err = -1;
            ESP_LOGI(TAG, "failed to enqueue play request");
            goto finish;
        }

        data = NULL;
        ESP_LOGI(TAG, "play request enqueued");
    }

finish:
    if (data)
        free(data); // TODO not fully freed!
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
    message_t msg;
    while (xQueueReceive(tcp_send_queue, &msg, portMAX_DELAY)) {
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
    message_t msg;

    xQueueReceive(http_ota_queue, &msg, portMAX_DELAY);

    esp_http_client_config_t config = {
        .url = ota_command_data->url,
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

/*8
typedef struct {
    char path[8];
} chunk_file_path_t;
*/

/**
 * fetcher downloads media file and write to chunk file.
 *
 * is responsible for freeing config (arg) pointer, but
 * not queue.
 */
static void fetcher(void *arg) {
    fetch_context_t *ctx = (fetch_context_t *)arg;
    esp_err_t err;

    ESP_LOGI(TAG, "fetching: %s", ctx->url);

    esp_http_client_config_t config = {
        .url = ctx->url,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if ((err = esp_http_client_open(client, 0)) != ESP_OK) {
        ESP_LOGI(TAG, "fetch error, failed to open http client (%d, %s)", err,
                 esp_err_to_name(err));

        message_t msg = {.type = MSG_FETCH_ERROR,
                         .from = ctx,
                         .value = {.fetch_error = {.err = err, .data = NULL}}};
        xQueueSend(juggler_queue, &msg, portMAX_DELAY);
        vTaskDelete(NULL);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    // ULLONG_MAX in limits.h
    if (content_length == -1) {
        ESP_LOGI(TAG, "server does not respond content-length in header");
        content_length = ctx->track_size;
    } else if (content_length != ctx->track_size) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        ESP_LOGI(TAG, "fetch error, size mismatch, expected: %d, actual: %d",
                 ctx->track_size, content_length);

        message_t msg = {
            .type = MSG_FETCH_ERROR,
            .from = ctx,
            .value = {.fetch_error = {.err = 0xF001, // define ERROR in header
                                      .data = NULL}}};
        xQueueSend(juggler_queue, &msg, portMAX_DELAY);
        vTaskDelete(NULL);
        return;
    } else {
        ESP_LOGI(TAG, "server responds a content-length of %d bytes",
                 content_length);
    }

    int total_read_len = 0, read_len;
    while (1) {
        message_t msg;
        xQueueReceive(ctx->fetch_chunk_in, &msg, portMAX_DELAY);
        if (msg.type == MSG_FETCH_ABORT) {
            message_t msg = { .type = MSG_FETCH_ABORTED, .from = ctx };
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            ESP_LOGI(TAG, "fetch aborted");
            xQueueSend(juggler_queue, &msg, portMAX_DELAY);
            vTaskDelete(NULL);
            return;
        }

        if (msg.type != MSG_FETCH_MORE) {
            ESP_LOGI(TAG, "message type: %d", msg.type);
        }

        assert(msg.type == MSG_FETCH_MORE);
        assert(msg.value.mem_block.data);

        // msg.type supposed to be MSG_FETCH_MORE
        char* data = msg.value.mem_block.data;
        read_len = esp_http_client_read(client, data, MEM_BLOCK_SIZE);
        total_read_len += read_len;
        if (read_len > 0) {
            if (total_read_len > ctx-> track_size) {
                // TODO oversize error
            } else if (total_read_len == ctx->track_size) {
                message_t reply = {0};
                reply.type = MSG_FETCH_FINISH;
                reply.from = ctx;
                reply.value.mem_block.length = read_len;
                reply.value.mem_block.data = data;
                xQueueSend(juggler_queue, &reply, portMAX_DELAY);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);

                ESP_LOGI(
                    TAG,
                    "fetch finished with last block of data size: %d bytes",
                    read_len);

                vTaskDelete(NULL);
                return;
            } else {
                message_t reply = {0};
                reply.type = MSG_FETCH_MORE_DATA;
                reply.from = ctx;
                reply.value.mem_block.length = read_len;
                reply.value.mem_block.data = data;
                xQueueSend(juggler_queue, &reply, portMAX_DELAY);
            }
        } else if (read_len == 0) {
            message_t reply = { .type = MSG_FETCH_FINISH, .from = ctx };
            reply.value.mem_block.length = 0;
            reply.value.mem_block.data = data; // don't forget this
            xQueueSend(juggler_queue, &reply, portMAX_DELAY);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            ESP_LOGI(TAG, "fetch finished without extra data");

            vTaskDelete(NULL);
            return;
        } else {
            // TODO
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "finished");

    vTaskDelete(NULL);
}

/**
 * Print sdmmc info for given card
 */
static void sdmmc_card_info(const sdmmc_card_t *card) {
    bool print_scr = true;
    bool print_csd = true;
    const char *type;

    char *TAG = pcTaskGetName(NULL);

    ESP_LOGI(TAG, "sdmmc name: %s", card->cid.name);
    if (card->is_sdio) {
        type = "SDIO";
        print_scr = true;
        print_csd = true;
    } else if (card->is_mmc) {
        type = "MMC";
        print_csd = true;
    } else {
        type = (card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC";
    }
    ESP_LOGI(TAG, "sdmmc type: %s", type);
    if (card->max_freq_khz < 1000) {
        ESP_LOGI(TAG, "sdmmc speed: %d kHz", card->max_freq_khz);
    } else {
        ESP_LOGI(TAG, "sdmmc speed: %d MHz%s", card->max_freq_khz / 1000,
                 card->is_ddr ? ", DDR" : "");
    }
    ESP_LOGI(TAG, "sdmmc size: %lluMB",
             ((uint64_t)card->csd.capacity) * card->csd.sector_size /
                 (1024 * 1024));

    if (print_csd) {
        ESP_LOGI(
            TAG,
            "sdmmc csd: ver=%d, sector_size=%d, capacity=%d read_bl_len=%d",
            card->csd.csd_ver, card->csd.sector_size, card->csd.capacity,
            card->csd.read_block_len);
    }
    if (print_scr) {
        ESP_LOGI(TAG, "sdmmc scr: sd_spec=%d, bus_width=%d", card->scr.sd_spec,
                 card->scr.bus_width);
    }
}


static FRESULT prepare_chunk_file(const char *vfs_path) {
    FRESULT fr;
    FILINFO fno;
    const char *path = vfs_path + strlen(MOUNT_POINT) + 1;

    fr = f_stat(path, &fno);
    // fatfs returns FR_DENIED when f_open a directory
    if (fr == FR_OK && (fno.fattrib & AM_DIR))
        return FR_DENIED;

    if (fr == FR_OK && fno.fsize == CHUNK_FILE_SIZE) {
        return FR_OK;
    }

    if (fr == FR_OK || fr == FR_NO_FILE) {
        FIL f;
        fr = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE | FA_READ);
        if (fr != FR_OK)
            return fr;
        fr = f_expand(&f, CHUNK_FILE_SIZE, 1);
        if (fr != FR_OK) {
            f_close(&f);
            return fr;
        }
        return f_close(&f);
    }

    return fr;
}

/**
 *
 */
static void juggler(void *arg) {
    const char *TAG = "juggler";
    esp_err_t err;
    message_t msg;
   
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 64,
        .allocation_unit_size = 64 * 1024};

    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(TAG, "intializing SD card");

    // Use settings defined above to initialize SD card and mount FAT
    // filesystem. Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience
    // functions. Please check its source code and implement error recovery when
    // developing production applications.
    ESP_LOGI(TAG, "Using SDMMC peripheral");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    // This initializes the slot without card detect (CD) and write protect (WP)
    // signals. Modify slot_config.gpio_cd and slot_config.gpio_wp if your board
    // has these signals.
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    // To use 1-line SD mode, change this to 1:
    slot_config.width = 1;

    // Enable internal pullups on enabled pins. The internal pullups
    // are insufficient however, please make sure 10k external pullups are
    // connected on the bus. This is for debug / example purpose only.
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "mounting exfat on sdmmc");
    err = esp_vfs_exfat_sdmmc_mount(mount_point, &host, &slot_config,
                                    &mount_config, &card);
    if (err == ESP_OK) {
        sdmmc_card_info(card);
    } else {
        ESP_LOGE(TAG,
                 "Failed to initialize the card (%s). "
                 "Make sure SD card lines have pull-up resistors in place.",
                 esp_err_to_name(err));
    }

    fetch_context_t* free_fetch_contexts[2] = {0};
    for (int i = 0; i < 2; i++) {
        free_fetch_contexts[i] =
            (fetch_context_t *)malloc(sizeof(fetch_context_t));
        memset(free_fetch_contexts[i], 0, sizeof(fetch_context_t));
    }
    int free_fetch_ctx_count = 2;

    char* free_mem_blocks[8] = {0};
    for (int i = 0; i < 8; i++) {
        free_mem_blocks[i] = (char*)malloc(MEM_BLOCK_SIZE);
        assert(free_mem_blocks[i]);
    }
    int free_mem_block_count = 8;

    for (int i = 0; i < 2; i++) {
        free_fetch_contexts[i]->fetch_chunk_in =
            xQueueCreate(16, sizeof(message_t));
    }

    FILE *fp;
    fp = fopen(TEMP_FILE_PATH, "w");
    if (!fp) {
        ESP_LOGI(TAG, "failed to open (w) temp file (%d)", errno);
    }

    fclose(fp);

    fp = fopen(TEMP_FILE_PATH, "r+");
    if (!fp) {
        ESP_LOGI(TAG, "failed to open (r+) temp file (%d)", errno);
    }

    // TODO  
    fseek(fp, 0L, SEEK_SET);

    int file_written = 0;
    int file_read = 0;
    int file_size = 0;

    fetch_context_t* playing_fetch_context = NULL;

    while (xQueueReceive(juggler_queue, &msg, portMAX_DELAY)) {
        switch (msg.type) {
        case MSG_CMD_PLAY: {
            ESP_LOGI(TAG, "play request received");

            play_command_data_t *data = msg.data;

            // prepare context for new play
            // TODO grasp fetch_context from queue
            if (free_fetch_ctx_count == 0 || free_mem_block_count < 2) {
                // TODO
                ESP_LOGI(TAG, "no free context or mem blocks");
            } else {
                fetch_context_t *ctx = 
                    free_fetch_contexts[free_fetch_ctx_count - 1];

                // TODO reduce mem
                strlcpy(ctx->url, data->tracks_url, 1024);
                ctx->digest = data->tracks[0].digest;
                track_url_strlcat(ctx->url, ctx->digest, 1024);
                ctx->track_size = data->tracks[0].size;
                ctx->play_started = false;

                if (pdPASS !=
                    xTaskCreate(fetcher, "fetcher", 8192, ctx, 6, NULL)) {
                    // TODO report error
                    ESP_LOGI(TAG, "failed to start fetcher");
                }

                playing_fetch_context = ctx;

                message_t msg = {.type = MSG_FETCH_MORE };
                msg.value.mem_block.data =
                    free_mem_blocks[free_mem_block_count - 1];
                free_mem_block_count--;
                xQueueSend(ctx->fetch_chunk_in, &msg, portMAX_DELAY);

                msg.value.mem_block.data =
                    free_mem_blocks[free_mem_block_count - 1];
                free_mem_block_count--;

                xQueueSend(ctx->fetch_chunk_in, &msg, portMAX_DELAY);
                // where to retrieve mem_block? and how much?

                file_size = ctx->track_size;
                file_read = 0;
                file_written = 0;

            }
        } break;

        case MSG_FETCH_MORE_DATA: {
            char* data = msg.value.mem_block.data;
            int length = msg.value.mem_block.length;
            fetch_context_t* ctx = msg.from;

            // TODO error
            fwrite(data, sizeof(char), length, fp);
            fflush(fp);
            file_written += msg.value.mem_block.length;
            
            if (ctx->play_started == false) {
                ctx->play_started = true;
                msg.type = MSG_AUDIO_DATA;
                msg.from = NULL;
                xQueueSend(audible_queue, &msg, portMAX_DELAY);
                file_read += length;
            } else {
                msg.type = MSG_FETCH_MORE;
                xQueueSend(ctx->fetch_chunk_in, &msg, portMAX_DELAY);
            }
        } break;

        case MSG_FETCH_FINISH: {
            char* data = msg.value.mem_block.data;
            int length = msg.value.mem_block.length;
            fetch_context_t* ctx = msg.from;

            if (length > 0) {
                // TODO error
                fwrite(data, sizeof(char), length, fp);
                fflush(fp);
                file_written += length;

                if (ctx->play_started == false) {
                    ctx->play_started = true;
                    msg.type = MSG_AUDIO_DATA;
                    msg.from = NULL;
                    xQueueSend(audible_queue, &msg, portMAX_DELAY);
                    file_read += length;
                } else {
                    free_mem_blocks[free_mem_block_count++] = data;
                }
            } else {
                free_mem_blocks[free_mem_block_count++] = data;
            }
        } break;

        case MSG_AUDIO_DONE: {
            if (file_read < file_written) {
                assert(file_written == ftell(fp));

                fseek(fp, file_read, SEEK_SET);
                int to_read = file_written - file_read;
                if (to_read > MEM_BLOCK_SIZE) to_read = MEM_BLOCK_SIZE;

                char *data = msg.value.mem_block.data;
                int length = fread(data, sizeof(char), to_read, fp);
                file_read += length;

                fseek(fp, file_written, SEEK_SET);
                
                msg.type = MSG_AUDIO_DATA;
                msg.from = NULL;  
                msg.value.mem_block.length = length;

                xQueueSend(audible_queue, &msg, portMAX_DELAY);
            } else {
                // starving !!! TODO 
                // it seems that we need a way to record fetch context related
                // to current play.
                // playing_fetch_context->
            }
        } break;

        default:
            ESP_LOGI(TAG, "message received");
            break;
        }
    }
}

extern void ble_adv_scan(void *args);
extern void esp_gap_cb(esp_gap_ble_cb_event_t event,
    esp_ble_gap_cb_param_t *param); 

void app_main(void) {
    // esp_err_t is typedef-ed int, so it could be used with lwip/sockets
    // api, but the value should be interpretted differently.
    esp_err_t err;
    int i, j;

    line = (char *)malloc(LINE_LENGTH);
    memset(line, 0, LINE_LENGTH);

    tx_buf = (char *)malloc(4096);
    memset(tx_buf, 0, 4096);
    rx_buf = (char *)malloc(4096);
    memset(rx_buf, 0, 4096);
    ap_record = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * 20);
    memset(ap_record, 0, sizeof(wifi_ap_record_t) * 20);

    ota_command_data = (ota_command_data_t *)malloc(sizeof(ota_command_data_t));

    event_bits = xEventGroupCreate();

    /** this could be created on demand */
    http_ota_queue = xQueueCreate(1, sizeof(message_t));
    xTaskCreate(http_ota, "http_ota", 16384, NULL, 11, NULL);

    juggler_queue = xQueueCreate(20, sizeof(message_t));
    if (errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY ==
        xTaskCreate(juggler, "juggler", 16384, NULL, 11, NULL)) {
        ESP_LOGI(TAG, "failed to create juggler task for memory constraint");
    }

    tcp_send_queue = xQueueCreate(20, sizeof(message_t));
    xTaskCreate(tcp_send, "tcp_send", 4096, NULL, 9, NULL);

    xTaskCreate(tcp_receive, "tcp_receive", 4096, NULL, 15, NULL);
    
    audible_queue = xQueueCreate(8, sizeof(message_t));
    xTaskCreatePinnedToCore(audible, "audible", 4096, NULL, 18, NULL, 1);

    // init nvs
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(esp_gap_cb));

    xTaskCreate(ble_adv_scan, "ble_adv", 4096, NULL, 16, NULL); 

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
