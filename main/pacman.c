#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_err.h"

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_mem.h"
#include "audio_common.h"
#include "mp3_decoder.h"
#include "filter_resample.h"

#include "esp_log.h"
#include "roadhill.h"

#define READ_BUF_SIZE   (16 * 1024)
#define WRITE_BUF_SIZE  (16 * 1024)

static const char *TAG = "pacman";

/*
 *
 */
typedef enum {
    PCM_STARTED,
    PCM_STOPPED
} pacman_state_t;

static pacman_state_t state = PCM_STOPPED;

static char* read_buf = NULL;
static int read_buf_len = 0;
static int read_buf_pos = -1;
static int total_read = 0;

/*
 * read data from in queue
 */
static int mp3_read_cb(audio_element_handle_t el, char *buf, int len, TickType_t wait_time, void *ctx) {

    QueueHandle_t in = ((pacman_context_t *)ctx)->in;
    pacman_inmsg_t msg;

    while (read_buf == NULL) {
        if (pdTRUE != xQueueReceive(in, &msg, portMAX_DELAY)) {
            continue;
        }

        // null-terminator
        if (msg.data == NULL) {
            return 0;
        }

        read_buf = msg.data;
        read_buf_len = msg.len;
        read_buf_pos = 0;
    }

    int left = read_buf_len - read_buf_pos;
    if (len < left) {
        memcpy(buf, &read_buf[read_buf_pos], len);
        read_buf_pos++;
        return len;
    } else {
        memcpy(buf, &read_buf[read_buf_pos], left);
        free(read_buf);
        read_buf = NULL;
        read_buf_pos = -1;
        return left;
    }
}

#define WRITE_BUF_SIZE (16 * 1024)

static char *write_buf = NULL;
static int write_buf_pos = -1;
static int total_written = 0;
static int64_t total_written_time = 0;

/*
 * write given data to mem block and send to out queue when full.
 */
static int rsp_write_cb(audio_element_handle_t el, char *buf, int len, TickType_t wait_time, void *ctx) {
    if (write_buf == NULL) {
        write_buf = (char *)malloc(WRITE_BUF_SIZE);
        memcpy(write_buf, buf, len);
        write_buf_pos = len;
    } else {
        int remain = WRITE_BUF_SIZE - write_buf_pos;
        if (len < remain) {
            memcpy(&write_buf[write_buf_pos], buf, len);
            write_buf_pos += len;
        } else {
            memcpy(&write_buf[write_buf_pos], buf, remain);
            /*
             * write_buf_pos += remain
             * so, write_buf_pos = WRITE_BUF_SIZE - write_buf_pos +
             * write_buf_pos so, write_buf_pos = WRITE_BUF_SIZE so, write_buf is
             * full
             */
            QueueHandle_t out = ((pacman_context_t *)ctx)->out;
            pacman_outmsg_t msg = {.data = write_buf, .len = WRITE_BUF_SIZE};
            xQueueSend(out, &msg, portMAX_DELAY);
            write_buf = NULL;
            write_buf_pos = -1;

            if (len > remain) {
                write_buf = (char *)malloc(WRITE_BUF_SIZE);
                memcpy(write_buf, &buf[remain], len - remain);
                write_buf_pos = len - remain;
            }
        }
    }
    return len;
}

/** pacman is a pun for pcm */
void pacman(void *ctx) {
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    audio_pipeline_handle_t pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    /**********************************************************
      #define MP3_DECODER_TASK_STACK_SIZE     (5 * 1024)
      #define MP3_DECODER_TASK_CORE           (0)
      #define MP3_DECODER_TASK_PRIO           (5)
      #define MP3_DECODER_RINGBUFFER_SIZE     (2 * 1024)

      #define DEFAULT_MP3_DECODER_CONFIG() {                  \
          .out_rb_size        = MP3_DECODER_RINGBUFFER_SIZE,  \
          .task_stack         = MP3_DECODER_TASK_STACK_SIZE,  \
          .task_core          = MP3_DECODER_TASK_CORE,        \
          .task_prio          = MP3_DECODER_TASK_PRIO,        \
          .stack_in_ext       = true,                         \
      }
    **********************************************************/
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_cfg.task_core = 1;
    mp3_cfg.task_prio = 18;
    mp3_cfg.stack_in_ext = false;
    audio_element_handle_t mp3_decoder = mp3_decoder_init(&mp3_cfg);
    audio_element_set_read_cb(mp3_decoder, mp3_read_cb, ctx);

    /******************************************************
      #define RSP_FILTER_BUFFER_BYTE              (512)
      #define RSP_FILTER_TASK_STACK               (4 * 1024)
      #define RSP_FILTER_TASK_CORE                (0)
      #define RSP_FILTER_TASK_PRIO                (5)
      #define RSP_FILTER_RINGBUFFER_SIZE          (2 * 1024)

      #define DEFAULT_RESAMPLE_FILTER_CONFIG() {          \
              .src_rate = 44100,                          \
              .src_ch = 2,                                \
              .dest_rate = 48000,                         \
              .dest_ch = 2,                               \
              .sample_bits = 16,                          \
              .mode = RESAMPLE_DECODE_MODE,               \
              .max_indata_bytes = RSP_FILTER_BUFFER_BYTE, \
              .out_len_bytes = RSP_FILTER_BUFFER_BYTE,    \
              .type = ESP_RESAMPLE_TYPE_AUTO,             \
              .complexity = 2,                            \
              .down_ch_idx = 0,                           \
              .prefer_flag = ESP_RSP_PREFER_TYPE_SPEED,   \
              .out_rb_size = RSP_FILTER_RINGBUFFER_SIZE,  \
              .task_stack = RSP_FILTER_TASK_STACK,        \
              .task_core = RSP_FILTER_TASK_CORE,          \
              .task_prio = RSP_FILTER_TASK_PRIO,          \
              .stack_in_ext = true,                       \
          }
    */
    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    // rsp_cfg.prefer_flag = ESP_RSP_PREFER_TYPE_MEMORY;
    rsp_cfg.task_core = 1;
    rsp_cfg.task_prio = 18;
    rsp_cfg.complexity = 5;
    rsp_cfg.stack_in_ext = false;
    rsp_cfg.dest_rate = 48000;
    rsp_cfg.dest_ch = 2;
    audio_element_handle_t rsp_filter = rsp_filter_init(&rsp_cfg);
    audio_element_set_write_cb(rsp_filter, rsp_write_cb, ctx);
    
    audio_pipeline_register(pipeline, mp3_decoder, "pacman_mp3");
    audio_pipeline_register(pipeline, rsp_filter, "pacman_rsp");

    const char *link_tag[2] = {"pacman_mp3", "pacman_rsp"};
    audio_pipeline_link(pipeline, &link_tag[0], 2);

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    audio_pipeline_set_listener(pipeline, evt);

    audio_pipeline_run(pipeline);

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            continue;
        }

/**
        if (msg.source_type == AUDIO_ELEMENT_TYPE_UNKNOW) {
            audio_pipeline_run(p 
        }
*/
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void *)mp3_decoder &&
            msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(mp3_decoder, &music_info);
            ESP_LOGI(
                TAG,
                "track info from mp3 decoder, sample rates=%d, bits=%d, ch=%d",
                music_info.sample_rates, music_info.bits, music_info.channels);
            rsp_filter_set_src_info(rsp_filter, music_info.sample_rates,
                                    music_info.channels);
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
            (int)msg.data == AEL_STATUS_STATE_FINISHED) {
            if ((void *)msg.source == mp3_decoder) {
                ESP_LOGI(TAG, "mp3_decoder finished");
            } else if ((void *)msg.source == rsp_filter) {
                ESP_LOGI(TAG, "rsp_filter finished");
                QueueHandle_t out = ((pacman_context_t *)ctx)->out;
                pacman_outmsg_t msg = {.data = write_buf, .len = write_buf_pos};
                xQueueSend(out, &msg, portMAX_DELAY);
                if (write_buf != NULL) {
                    write_buf = NULL;
                    write_buf_pos = -1;
                    msg.data = write_buf;
                    msg.len = write_buf_pos;
                    xQueueSend(out, &msg, portMAX_DELAY);
                }

                /*
                 * ESP_LOGI(TAG, "totally written %d bytes, total
                 * written time: %lld", total_written, total_written_time);
                 */

                audio_pipeline_run(pipeline);
            }
        }
    }

    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);
    audio_pipeline_unregister_more(pipeline, mp3_decoder, rsp_filter);

    audio_pipeline_remove_listener(pipeline);
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(mp3_decoder);
    audio_element_deinit(rsp_filter);

    // audio_event_iface_remove_listener(???)
    audio_event_iface_destroy(evt);
}

