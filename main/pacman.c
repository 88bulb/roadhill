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
#include "ringbuf.h"
#include "mp3_decoder.h"
#include "filter_resample.h"

#include "esp_log.h"
#include "roadhill.h"

#define READ_BUF_SIZE   (16 * 1024)
#define WRITE_BUF_SIZE  (16 * 1024)

static const char *TAG = "pacman";

// code from ringbuf.c
struct ringbuf {
    char *p_o;                  /**< Original pointer */
    char *volatile p_r;         /**< Read pointer */
    char *volatile p_w;         /**< Write pointer */
    volatile uint32_t fill_cnt; /**< Number of filled slots */
    uint32_t size;              /**< Buffer size */
    SemaphoreHandle_t can_read;
    SemaphoreHandle_t can_write;
    SemaphoreHandle_t lock;
    bool abort_read;
    bool abort_write;
    bool is_done_write;       /**< To signal that we are done writing */
    bool unblock_reader_flag; /**< To unblock instantly from rb_read */
};

static char* read_buf = NULL;
static int read_buf_len = 0;
static int read_buf_pos = 0;
static int total_read = 0;

static int mp3_read_cb(audio_element_handle_t el, char *buf, int len,
                       TickType_t wait_time, void *ctx) {

    FILE *fp = ((pacman_context_t *)ctx)->in;
    if (read_buf_len == read_buf_pos) {
        if (feof(fp)) {
            fclose(fp);
            return 0;
        }
        read_buf_len = fread(read_buf, 1, READ_BUF_SIZE, fp);
        total_read += read_buf_len;
        read_buf_pos = 0; 
    }

    if (len < read_buf_len - read_buf_pos) {
        memcpy(buf, &read_buf[read_buf_pos], len);
        read_buf_pos += len;
        return len;
    } else {
        int l = read_buf_len - read_buf_pos;
        memcpy(buf, &read_buf[read_buf_pos], l);
        read_buf_pos = read_buf_len;
        return l;
    }
}

static char *write_buf = NULL;
static int write_buf_pos = 0;
static int total_written = 0;
static int64_t total_written_time = 0;

static int rsp_write_cb(audio_element_handle_t el, char *buf, int len,
                        TickType_t wait_time, void *ctx) {

    return len;
/*
    FILE *fp = ((pacman_context_t *)ctx)->out;
    if (write_buf_pos + len > WRITE_BUF_SIZE) {
        int64_t before = esp_timer_get_time();
        fwrite(write_buf, 1, write_buf_pos, fp);
        total_written += write_buf_pos;
        int64_t after = esp_timer_get_time();
        total_written_time += after - before;
        write_buf_pos = 0;
    }
    memcpy(&write_buf[write_buf_pos], buf, len);
    write_buf_pos += len;
    return len;
*/
}

/** pacman is a pun for pcm */
void pacman(void *ctx) {
    ringbuf_handle_t mp3_in, mp3_out, rsp_in, rsp_out;

    read_buf = (char*)heap_caps_malloc(READ_BUF_SIZE, MALLOC_CAP_DMA);
//    read_buf = (char*)malloc(READ_BUF_SIZE);
    assert(read_buf);

//    write_buf = (char*)heap_caps_malloc(WRITE_BUF_SIZE, MALLOC_CAP_DMA);
//    assert(write_buf);

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
    rsp_in = audio_element_get_input_ringbuf(rsp_filter);
    rsp_out = audio_element_get_output_ringbuf(rsp_filter);
    
    audio_pipeline_register(pipeline, mp3_decoder, "pacman_mp3");
    audio_pipeline_register(pipeline, rsp_filter, "pacman_rsp");

    const char *link_tag[2] = {"pacman_mp3", "pacman_rsp"};
    audio_pipeline_link(pipeline, &link_tag[0], 2);

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    audio_pipeline_set_listener(pipeline, evt);

    audio_pipeline_run(pipeline);

    mp3_in = audio_element_get_input_ringbuf(mp3_decoder);
    if (mp3_in == NULL) {
        ESP_LOGI(TAG, "mp3_decoder input ringbuf is NULL");
    } else {
        ESP_LOGI(TAG, "mp3_decoder input ringbuf @%08x, rb->buf (po) @%08x",
            (uint32_t)mp3_in, (uint32_t)(mp3_in->p_o));
    }

    mp3_out = audio_element_get_output_ringbuf(mp3_decoder);
    if (mp3_out == NULL) {
        ESP_LOGI(TAG, "mp3_decoder output ringbuf is NULL");
    } else {
        ESP_LOGI(TAG, "mp3_decoder output ringbuf @%08x, rb->buf (po) @%08x",
            (uint32_t)mp3_out, (uint32_t)(mp3_out->p_o));
    }

    rsp_in = audio_element_get_input_ringbuf(rsp_filter);
    if (rsp_in == NULL) {
        ESP_LOGI(TAG, "rsp_filter input ringbuf is NULL");
    } else {
        ESP_LOGI(TAG, "rsp_filter input ringbuf @%08x, rb->buf (po) @%08x",
            (uint32_t)rsp_in, (uint32_t)(rsp_in->p_o));
    }

    rsp_out = audio_element_get_output_ringbuf(rsp_filter);
    if (rsp_out == NULL) {
        ESP_LOGI(TAG, "rsp_filter output ringbuf is NULL");
    } else {
        ESP_LOGI(TAG, "rsp_filter output ringbuf @%08x, rb->buf (po) @%08x",
            (uint32_t)rsp_out, (uint32_t)(rsp_out->p_o));
    }

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            continue;
        }

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
            // audio_element_setinfo(rsp_filter, &music_info);
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
            (int)msg.data == AEL_STATUS_STATE_FINISHED) {
            if ((void *)msg.source == mp3_decoder) {
                ESP_LOGI(TAG, "mp3_decoder finished");
            } else if ((void *)msg.source == rsp_filter) {
                ESP_LOGI(TAG, "rsp_filter finished");
                FILE *fp = ((pacman_context_t *)ctx)->out;
/*
                if (write_buf_pos) {
                    int64_t before = esp_timer_get_time();
                    fwrite(write_buf, 1, write_buf_pos, fp);
                    total_written += write_buf_pos;
                    int64_t after = esp_timer_get_time();
                    total_written_time += after - before;
                }
*/
                fclose(fp);
                ESP_LOGI(TAG,
                         "totally written %d bytes, total written time: %lld",
                         total_written, total_written_time);
                break;
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

    ESP_LOGI(TAG, "task terminates");

    vTaskDelete(NULL);
}
