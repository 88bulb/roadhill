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

static const char *TAG = "juggler";

extern void pacman(void *arg);
extern void picman(void *arg);
extern esp_err_t init_mmcfs();

/* upstream and downstream ports */
QueueHandle_t jug_in = NULL, jug_out = NULL, pcm_in = NULL, pcm_out = NULL,
              pic_in = NULL, pic_out = NULL;

/* current job */
mmcfs_file_handle_t file = NULL;

/* deprecated */
void track_url_strlcat(char *tracks_url, md5_digest_t digest, size_t size) {
    char str[40] = {0};

    for (int i = 0; i < 16; i++) {
        str[2 * i + 0] = hex_char[digest.bytes[i] / 16];
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

static void handle_frame_request() {
    frame_request_t *req = NULL;


again:
    if (0 == uxQueueMessagesWaiting(jug_in)) {
        return;
    }

    xQueuePeek(jug_in, &req, 0);
    if (req == NULL) {
        return; // TODO end of stream
    }

    print_frame_request(req);

    if (file) {
        return;
    }

    // do we need to create a file for this request? or it could be served
    // instantly?
    track_t *tr0 = req->track_mix[0].track;
    track_t *tr1 = req->track_mix[1].track;

    // TODO fill blank
    if (tr0 == NULL && tr1 == NULL) {
        return;
    }

    if (tr0) {
        mmcfs_finfo_t info;
        int res = mmcfs_stat(&tr0->digest, &info);
        assert(res != EINVAL);

        if (res == -ENOENT) {
            file = mmcfs_create_file(&tr0->digest, tr0->size);
            // TODO in case of file is NULL

            // start picman and pacman
            picman_inmsg_t cmd = {
                .url = req->url,
                .digest = &tr0->digest,
                .size = tr0->size,
            };
            xQueueSend(pic_in, &cmd, portMAX_DELAY);
            return;
        }

        // TODO don't assume pcm exists
    }

    if (tr1) {
        mmcfs_finfo_t info;
        int res = mmcfs_stat(&tr1->digest, &info);
        assert(res != EINVAL);

        if (res == -ENOENT) {
            file = mmcfs_create_file(&tr0->digest, tr0->size);
            // TODO in case of file is NULL

            // start picman and pacman
            picman_inmsg_t cmd = {
                .url = req->url,
                .digest = &tr0->digest,
                .size = tr0->size,
            };
            xQueueSend(pic_in, &cmd, portMAX_DELAY);
            return;
        }
    }

    xQueueReceive(jug_in, &req, 0);
    mmcfs_pcm_mix(tr0 ? &tr0->digest : NULL, tr0 ? req->track_mix[0].pos : 0,
                  tr1 ? &tr1->digest : NULL, tr1 ? req->track_mix[1].pos : 0,
                  req->buf);

    req->res = JUG_REQ_FULFILLED;

    xQueueSend(jug_out, &req, portMAX_DELAY);
    goto again;
}

static void handle_pic_out() {
    if (0 == uxQueueSpacesAvailable(pcm_in) ||
        0 == uxQueueMessagesWaiting(pic_out)) {
        return;
    }

    picman_outmsg_t outmsg;
    xQueueReceive(pic_out, &outmsg, 0);

    if (outmsg.data == NULL) {

        ESP_LOGI(TAG, "pic_out data NULL");

        pacman_inmsg_t msg = {
            .data = NULL,
            .len = 0,
        };
        xQueueSend(pcm_in, &msg, 0); // assert and error TODO
    } else {
        mmcfs_write_mp3(file, outmsg.data, outmsg.size_or_error);
        pacman_inmsg_t inmsg = {
            .data = outmsg.data,
            .len = outmsg.size_or_error,
        };
        xQueueSend(pcm_in, &inmsg, 0); // assert and error TODO
    }
}

static void handle_pcm_out() {
    pacman_outmsg_t outmsg;
    xQueueReceive(pcm_out, &outmsg, 0);
    switch (outmsg.type) {
    case PCM_IN_DRAIN: {
        handle_pic_out();
    } break;
    case PCM_OUT_DATA: {
        mmcfs_write_pcm(file, outmsg.data, outmsg.len);
        free(outmsg.data);
    } break;
    case PCM_OUT_ERROR:
        break;
    case PCM_OUT_FINISH: {
        mmcfs_commit_file(file);
        file = NULL;
        handle_frame_request();
    } break;
    default:
        break;
    }
}

/**
 * juggler task
 */
void juggler(void *arg) {
    esp_err_t err;
    ESP_LOGI(TAG, "juggler task starts");

    err = init_mmcfs();
    if (err) {
        ESP_LOGW(TAG, "failed to initialize mmcfs");
        die_another_day();
    }

    jug_in = ((juggler_ports_t *)arg)->in;
    jug_out = ((juggler_ports_t *)arg)->out;
    pcm_in = xQueueCreate(4, sizeof(pacman_inmsg_t));
    pcm_out = xQueueCreate(4, sizeof(pacman_outmsg_t));
    pic_in = xQueueCreate(4, sizeof(picman_inmsg_t));
    pic_out = xQueueCreate(4, sizeof(picman_outmsg_t));

    // !!! qset's length must be at least the sum of all queue's length
    QueueSetHandle_t qset = xQueueCreateSet(12);
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

    while (1) {
        QueueSetMemberHandle_t q = xQueueSelectFromSet(qset, portMAX_DELAY);
        if (q == jug_in) {
            handle_frame_request();
        } else if (q == pic_out) {
            handle_pic_out();
        } else if (q == pcm_out) {
            handle_pcm_out();
        }
    }
}
