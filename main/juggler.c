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

bool siffs_alloc(uint32_t blk_addr, int bits) {
    if (blk_addr + bits > ALLOC_MAP_SIZE)
        return false;

    for (uint32_t addr = blk_addr; addr < blk_addr + bits; addr++) {
        if (alloc_map[addr / 8] & (1 << (addr % 8)))
            return false;
    }

    for (uint32_t addr = blk_addr; addr < blk_addr + bits; addr++) {
        alloc_map[addr / 8] |= (1 << (addr % 8));
    }
    return true;
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
 * fetcher downloads track data and send them back to juggler.
 * fetcher reads mem_block_t object out of context.
 */
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

    xTaskCreate(pacman, "pacman", 4096, &pacman_ctx, 15, NULL); 

    vTaskDelay(portMAX_DELAY);

    // uint16_t fre_clust;
    // f_getfree("0:", &fre_clust, NULL);

    FILINFO fno;
    FRESULT res = f_stat("siffs", &fno);
    if (res != FR_OK) {
        ESP_LOGW(TAG, "siffs file not found, f_stat error (%d)", res);
        die_another_day();
    }

    assert(fno.fsize > DATA_OFFSET);
    block_size =
        (fno.fsize - DATA_OFFSET) / (BLOCK_NUM_BOUND * BLOCK_NUM_FRACT);

    assert((block_size & (block_size - 1)) == 0);

    FIL fil;
    res = f_open(&fil, "siffs", FA_READ | FA_WRITE);
    if (res) {
        ESP_LOGW(TAG, "failed to open siffs file, f_open error (%d)", res);
        die_another_day();
    }

    alloc_map = (uint8_t *)malloc(ALLOC_MAP_SIZE / sizeof(uint8_t));

    unsigned int read;
    int entries = 0;
    int used_blocks = 0;
    int bucket_count = 0;
    int read_buf_size = 16 * 1024;
    uint8_t *read_buf = (uint8_t *)malloc(read_buf_size);

    memset(alloc_map, 0, sizeof(ALLOC_MAP_SIZE / sizeof(uint8_t)));
    res = f_lseek(&fil, META_OFFSET);
    for (int i = 0; i < META_OFFSET / read_buf_size; i++) {
        res = f_read(&fil, read_buf, read_buf_size, &read);
        if (read != read_buf_size) {
            ESP_LOGI(TAG, "f_read merely reads %u bytes", read);
            // TODO fatal error
            die_another_day();
        }

        int buckets_per_buf = read_buf_size / BUCKET_SIZE;
        for (int j = 0; j < buckets_per_buf; j++) {
            uint16_t idx = i * buckets_per_buf + j;
            uint8_t head[2];
            head[0] = idx >> 4;          // high 8 bit
            head[1] = (idx & 0x0f) << 4; // low 4 bit shift to high

            siffs_metadata_bucket_t *buck =
                (siffs_metadata_bucket_t *)(&read_buf[j * BUCKET_SIZE]);
            for (int k = 0; k < 32; k++) {
                siffs_metadata_t *meta = &buck->meta[k];
                if (meta->md5sum[0] == head[0] &&
                    (meta->md5sum[1] & 0xf0) == head[1] && meta->size != 0) {
                    entries++;
                    int sib = size_in_blocks(meta->size);
                    assert(
                        siffs_alloc(meta->blk_addr, sib)); // TODO conflict !!!
                    used_blocks += sib;
                }
            }
        }

        bucket_count += buckets_per_buf;
    }

    free(read_buf);

    // TODO freespace
    ESP_LOGI(TAG, "siffs file size %lluMiB", fno.fsize / 1024 / 1024);
    ESP_LOGI(TAG,
             "%d metadata entries found in %d buckets. The size of bucket is "
             "%d bytes. The size of each metadata entry is %d bytes.",
             entries, bucket_count, BUCKET_SIZE, sizeof(siffs_metadata_t));

    ESP_LOGI(TAG,
             "%d blocks in total, %d blocks used, %d blocks free. The size "
             "of each block is %d KiB",
             (BLOCK_NUM_BOUND * BLOCK_NUM_FRACT), used_blocks,
             ALLOC_MAP_SIZE - used_blocks, block_size / 1024);

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

    int file_written = 0;
    int file_read = 0;

    uint32_t playing_index = 0;
    char *playing_tracks_url = NULL;
    track_t playing_track;

    while (xQueueReceive(juggler_queue, &msg, portMAX_DELAY)) {
        switch (msg.type) {
        case MSG_CMD_PLAY: {
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

            if (pdPASS != xTaskCreate(fetcher, "fetcher", 8192, ctx, 6, NULL)) {
                // TODO report error
                ESP_LOGI(TAG, "failed to start fetcher");
            } else {
                ESP_LOGI(TAG, "new fetcher");
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

        } break;

        case MSG_FETCH_MORE_DATA: {
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
        } break;

        case MSG_FETCH_FINISH: {
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
        } break;

        case MSG_AUDIO_DONE: {
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
        } break;

        default:
            ESP_LOGI(TAG, "message received");
            break;
        }
    }
}
