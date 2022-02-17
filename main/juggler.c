#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"

#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"

#include "esp_http_client.h"

#include "roadhill.h"

#define MOUNT_POINT "/emmc"
#define CHUNK_FILE_SIZE (1024 * 1024)
#define TEMP_FILE_PATH "/emmc/temp"

#define FETCH_INPUT_QUEUE_SIZE (4)

// defined in sdmmc
extern esp_err_t esp_vfs_exfat_sdmmc_mount(
    const char *base_path, const sdmmc_host_t *host_config,
    const void *slot_config, const esp_vfs_fat_mount_config_t *mount_config,
    sdmmc_card_t **out_card);

// this is a thread-safe resource queue
QueueHandle_t play_context_queue;
// this is a messaging queue
QueueHandle_t juggler_queue;

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

/** TODO this function is not used anymore */
FRESULT prepare_chunk_file(const char *vfs_path) {
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

esp_err_t init_mmc() {
    const char* TAG = "init_mmc";
    esp_err_t err;

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

    return err;
}

/**
 * fetcher downloads track data and send them back to juggler.
 * fetcher reads mem_block_t object out of context.
 */
static void fetcher(void *arg) {
    const char* TAG = "fetcher"; // TODO add file name?
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
        xQueueReceive(ctx->input, &msg, portMAX_DELAY);
        if (msg.type == MSG_FETCH_ABORT) {
            message_t msg = {.type = MSG_FETCH_ABORTED, .from = ctx};
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            ESP_LOGI(TAG, "fetch aborted");
            xQueueSend(juggler_queue, &msg, portMAX_DELAY);
            vTaskDelete(NULL);
            return;
        }

        assert(msg.type == MSG_FETCH_MORE);
        assert(msg.value.mem_block.data);

        char *data = msg.value.mem_block.data;
        read_len = esp_http_client_read(client, data, MEM_BLOCK_SIZE);
        total_read_len += read_len;
        if (read_len > 0) {
            if (total_read_len > ctx->track_size) {
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
            message_t reply = {.type = MSG_FETCH_FINISH, .from = ctx};
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
 * juggler task
 */
void juggler(void *arg) {
    const char *TAG = "juggler";

    message_t msg;

    // TODO process error
    init_mmc();

    // initialize play_context_t objects
    play_context_queue = xQueueCreate(2, sizeof(play_context_t *));
    for (int i = 0; i < 2; i++) {
        play_context_t *p = (play_context_t *)malloc(sizeof(play_context_t));
        memset(p, 0, sizeof(play_context_t));
        xQueueSend(play_context_queue, &p, portMAX_DELAY);
    }

    // initialize fetch_context_t objects
    fetch_context_t *free_fetch_contexts[2] = {0};
    for (int i = 0; i < 2; i++) {
        free_fetch_contexts[i] =
            (fetch_context_t *)malloc(sizeof(fetch_context_t));
        memset(free_fetch_contexts[i], 0, sizeof(fetch_context_t));
    }
    int free_fetch_ctx_count = 2;

    char *free_mem_blocks[8] = {0};
    for (int i = 0; i < 8; i++) {
        free_mem_blocks[i] = (char *)malloc(MEM_BLOCK_SIZE);
        assert(free_mem_blocks[i]);
    }
    int free_mem_block_count = 8;

    for (int i = 0; i < 2; i++) {
        free_fetch_contexts[i]->input =
            xQueueCreate(FETCH_INPUT_QUEUE_SIZE, sizeof(message_t));
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

    play_context_t *play_context = NULL;

    while (xQueueReceive(juggler_queue, &msg, portMAX_DELAY)) {
        switch (msg.type) {
        case MSG_CMD_PLAY: {
            ESP_LOGI(TAG, "play request received");

            play_context = msg.value.play_context;

            // prepare context for new play
            // TODO grasp fetch_context from queue
            if (free_fetch_ctx_count == 0 || free_mem_block_count < 2) {
                // TODO
                ESP_LOGI(TAG, "no free context or mem blocks");
            } else {
                fetch_context_t *ctx =
                    free_fetch_contexts[free_fetch_ctx_count - 1];

                // TODO reduce mem
                strlcpy(ctx->url, play_context->tracks_url, 1024);
                ctx->digest = play_context->tracks[0].digest;
                track_url_strlcat(ctx->url, ctx->digest, 1024);
                ctx->track_size = play_context->tracks[0].size;
                ctx->play_started = false;

                if (pdPASS !=
                    xTaskCreate(fetcher, "fetcher", 8192, ctx, 6, NULL)) {
                    // TODO report error
                    ESP_LOGI(TAG, "failed to start fetcher");
                }

                message_t msg = {.type = MSG_FETCH_MORE};
                msg.value.mem_block.data =
                    free_mem_blocks[free_mem_block_count - 1];
                free_mem_block_count--;
                xQueueSend(ctx->input, &msg, portMAX_DELAY);

                msg.value.mem_block.data =
                    free_mem_blocks[free_mem_block_count - 1];
                free_mem_block_count--;

                xQueueSend(ctx->input, &msg, portMAX_DELAY);
                // where to retrieve mem_block? and how much?

                // file_size = ctx->track_size;
                file_read = 0;
                file_written = 0;
            }
        } break;

        case MSG_FETCH_MORE_DATA: {
            // write data to persitent store
            // and fetch more
            char *data = msg.value.mem_block.data;
            int length = msg.value.mem_block.length;
            fetch_context_t *ctx = msg.from;

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
                xQueueSend(ctx->input, &msg, portMAX_DELAY);
            }
        } break;

        case MSG_FETCH_FINISH: {
            char *data = msg.value.mem_block.data;
            int length = msg.value.mem_block.length;
            fetch_context_t *ctx = msg.from;

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
                // read more data and send to audible_queue

                assert(file_written == ftell(fp));

                fseek(fp, file_read, SEEK_SET);
                int to_read = file_written - file_read;
                if (to_read > MEM_BLOCK_SIZE)
                    to_read = MEM_BLOCK_SIZE;

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

