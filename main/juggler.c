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

#include "mmcfs.h"
#include "roadhill.h"

#define FETCH_INPUT_QUEUE_SIZE (4)

static const char* TAG = "juggler";

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



void die_another_day() {
    // 24 hours
    vTaskDelay(24 * 60 * 60 * 1000 / portTICK_PERIOD_MS);
    esp_restart();
}

/**
 * juggler task
 */
void juggler(void *arg) {
    esp_err_t err;
    message_t msg;

    md5_digest_t md5;
    md5_digest_t *fetching = NULL;
    md5_digest_t *decoding = NULL;

    frame_request_t *req = NULL;
    frame_request_t *req_array[4] = {0};
    int req_count = 0;

    ESP_LOGI(TAG, "juggler task starts"); 

    err = init_mmcfs();
    if (err) {
        ESP_LOGW(TAG, "failed to initialize mmcfs");
        die_another_day();
    }

    juggler_ports_t *ports = (juggler_ports_t *)arg;

    QueueSetMemberHandle_t q;
    
    QueueHandle_t jug_in = ports->in;
    QueueHandle_t jug_out = ports->out;
    QueueHandle_t pcm_in = xQueueCreate(8, sizeof(pacman_inmsg_t));
    QueueHandle_t pcm_out = xQueueCreate(8, sizeof(pacman_outmsg_t)); 
    QueueHandle_t pic_in = xQueueCreate(2, sizeof(picman_inmsg_t));
    QueueHandle_t pic_out = xQueueCreate(8, sizeof(picman_outmsg_t));

    QueueSetHandle_t qset = xQueueCreateSet(8);
    xQueueAddToSet(jug_in, qset);
    xQueueAddToSet(pcm_out, qset);
    xQueueAddToSet(pic_out, qset);

    pacman_context_t pacman_ctx = {
        .in = pcm_in,
        .out = pcm_out,
    };
    xTaskCreate(pacman, "pacman", 4096, &pacman_ctx, 11, NULL); 

    picman_context_t picman_ct = {
        .in = pic_in,
        .out = pic_out,
    };
    xTaskCreate(picman, "picman", 4096, &picman_ct, 12, NULL);

forever_loop:
    q = xQueueSelectFromSet(qset, portMAX_DELAY);
    if (q == jug_in) {
        ESP_LOGI(TAG, "jug_in input message");
        xQueueReceive(q, &req, 0); // TODO error handling?

        print_frame_request(req);

        track_t *trac = req->track_mix[0].track;
        if (trac) {
            mmcfs_finfo_t info;
            int res = mmcfs_stat(&trac->digest, &info);

            assert(res != EINVAL);

            if (res == -ENOENT) {
                ESP_LOGI(TAG, "mmcfs_stat ENOENT");

                req_array[req_count] = req;
                req_count++;
                if (req_count == 1) {
                    picman_inmsg_t cmd = {0};
                    cmd.url = req->url;
                    cmd.digest = &req->track_mix[0].track->digest;
                    cmd.track_size = req->track_mix[0].track->size;
                    xQueueSend(pic_in, &cmd, portMAX_DELAY);
                }
            } else {
            }
        }
    } else if (q == pic_out) {
        if (pdTRUE == uxQueueSpacesAvailable(pcm_in)) {
            picman_outmsg_t outmsg;
            xQueueReceive(pic_out, &outmsg, 0);
             
        }
    } else if (q == pcm_out) {
    }
    goto forever_loop;

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
