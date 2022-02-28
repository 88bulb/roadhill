#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"

#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

#include "esp_http_client.h"

#include "roadhill.h"

#define FETCH_INPUT_QUEUE_SIZE (4)

#define ALLOC_MAP_SIZE (BLOCK_NUM_BOUND * BLOCK_NUM_FRACT)

extern void pacman(void* arg);
extern void picman(void* arg);

// uint8_t alloc_map[ALLOC_MAP_SIZE / sizeof(uint8_t)] = {};
uint8_t *alloc_map = NULL;

int block_size = 0;

// this is a thread-safe resource queue
QueueHandle_t play_context_queue;
// this is a messaging queue
QueueHandle_t juggler_queue;

extern esp_err_t init_mmcfs();

int size_in_blocks(uint32_t size) {
    return size / block_size + (size % block_size) ? 1 : 0;
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

/**
 * fetcher downloads track data and send them back to juggler.
 * fetcher reads mem_block_t object out of context.
 */
/**
static void fetcher(void *arg) {
    const char *TAG = "fetcher"; // TODO add file name?
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

        msg.value.mem_block.play_index = ctx->play_index;
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
} */

void die_another_day() {
    // 24 hours
    vTaskDelay(24 * 60 * 60 * 1000 / portTICK_PERIOD_MS);
    esp_restart();
}

/**
 * juggler task
 */
void juggler(void *arg) {
    const char *TAG = "juggler";
    esp_err_t err;
    message_t msg;

    err = init_mmcfs();
    if (err) {
        ESP_LOGW(TAG, "failed to initialize mmcfs");
        die_another_day();
    }

    QueueHandle_t pcm_in = xQueueCreate(8, sizeof(pacman_inmsg_t));
    QueueHandle_t pcm_out = xQueueCreate(8, sizeof(pacman_outmsg_t)); 
    pacman_context_t pacman_ctx = {
        .in = pcm_in,
        .out = pcm_out,
    };

    xTaskCreate(pacman, "pacman", 4096, &pacman_ctx, 11, NULL); 

    QueueHandle_t pic_in = xQueueCreate(8, sizeof(picman_inmsg_handle_t));
    QueueHandle_t pic_out = xQueueCreate(8, sizeof(picman_outmsg_t));
    picman_context_t picman_ct = {
        .in = pic_in,
        .out = pic_out,
    };

    xTaskCreate(picman, "picman", 4096, &picman_ct, 12, NULL);

    vTaskDelay(portMAX_DELAY);

    while (xQueueReceive(juggler_queue, &msg, portMAX_DELAY)) {
        switch (msg.type) {
        case MSG_CMD_PLAY: {
/*
            ESP_LOGI(TAG, "play command received");

            playing_index = msg.value.play_data.index;

            if (playing_tracks_url)
                free(playing_tracks_url);
            playing_tracks_url = msg.value.play_data.tracks_url;

            if (msg.value.play_data.tracks_array_size) {
                playing_track = msg.value.play_data.tracks[0];
            } else {
                playing_track.size = 0;
            }

            // TODO
            free(msg.value.play_data.tracks);

            // from old code
            fetch_context_t *ctx =
                free_fetch_contexts[free_fetch_ctx_count - 1];

            ctx->play_index = playing_index;

            // TODO reduce mem
            strlcpy(ctx->url, playing_tracks_url, URL_BUFFER_SIZE);
            ctx->digest = playing_track.digest;
            track_url_strlcat(ctx->url, ctx->digest, URL_BUFFER_SIZE);
            ctx->track_size = playing_track.size;
            ctx->play_started = false;
*/
/**
            if (pdPASS != xTaskCreate(fetcher, "fetcher", 8192, ctx, 6, NULL)) {
                // TODO report error
                ESP_LOGI(TAG, "failed to start fetcher");
            } else {
                ESP_LOGI(TAG, "new fetcher");
            }
*/
/*
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
*/
        } break;

        case MSG_FETCH_MORE_DATA: {
/*
            // write data to persitent store
            // and fetch more
            char *data = msg.value.mem_block.data;
            int length = msg.value.mem_block.length;
            fetch_context_t *ctx = msg.from;

            // fwrite(data, sizeof(char), length, fp);
            // fflush(fp);
            file_written += msg.value.mem_block.length;

            if (ctx->play_started == false) {
                ctx->play_started = true;
                msg.type = MSG_AUDIO_DATA;
                msg.from = NULL;

                xQueueSend(audio_queue, &msg, portMAX_DELAY);
                file_read += length;
            } else {
                msg.type = MSG_FETCH_MORE;
                xQueueSend(ctx->input, &msg, portMAX_DELAY);
            }

            // ESP_LOGI(TAG, "MSG_FETCH_MORE_DATA");
*/
        } break;

        case MSG_FETCH_FINISH: {
/*
            char *data = msg.value.mem_block.data;
            int length = msg.value.mem_block.length;
            fetch_context_t *ctx = msg.from;

            if (length > 0) {
                // TODO error
                // fwrite(data, sizeof(char), length, fp);
                // fflush(fp);
                file_written += length;

                if (ctx->play_started == false) {
                    ctx->play_started = true;
                    msg.type = MSG_AUDIO_DATA;
                    msg.from = NULL;
                    xQueueSend(audio_queue, &msg, portMAX_DELAY);
                    file_read += length;
                } else {
                    free_mem_blocks[free_mem_block_count++] = data;
                }
            } else {
                free_mem_blocks[free_mem_block_count++] = data;
            }
*/
        } break;

        case MSG_AUDIO_DONE: {
/**
            if (file_read < file_written) {
                // read more data and send to audio_queue

                // assert(file_written == ftell(fp));
                // fseek(fp, file_read, SEEK_SET);
                int to_read = file_written - file_read;
                if (to_read > MEM_BLOCK_SIZE)
                    to_read = MEM_BLOCK_SIZE;

                char *data = msg.value.mem_block.data;
                // int length = fread(data, sizeof(char), to_read, fp);
                int length = 0;
                file_read += length;

                // fseek(fp, file_written, SEEK_SET);

                msg.type = MSG_AUDIO_DATA;
                msg.from = NULL;
                msg.value.mem_block.length = length;

                xQueueSend(audio_queue, &msg, portMAX_DELAY);
            } else {
                // starving !!! TODO
                // it seems that we need a way to record fetch context related
                // to current play.
                // playing_fetch_context->
            }
*/
        } break;

        default:
            ESP_LOGI(TAG, "message received");
            break;
        }
    }
}
