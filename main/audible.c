#include <string.h>

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
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "raw_stream.h"

#include "esp_log.h"
#include "esp_peripherals.h"
#include "periph_touch.h"
#include "periph_adc_button.h"
#include "periph_button.h"
#include "board.h"

#include "roadhill.h"

const char *TAG = "audible";

#define AUDIO_DATA_BUF_SIZE (16384)
#define AUDIO_DATA_BUF_NUM (2)

extern QueueHandle_t tcp_send_queue;
extern QueueHandle_t juggler_queue;
extern QueueHandle_t audio_queue;
extern QueueHandle_t ble_queue;

play_context_t* play_context = NULL;

/**
static esp_err_t unity_open(audio_element_handle_t self) {
    return ESP_OK;
}

static int unity_process(audio_element_handle_t self, char *buf, int len) {
    static uint32_t total = 0;

    if (len == 1024) {
        int16_t* p = (int16_t*)buf;
        uint32_t sum = 0;
        for (int i = 0; i < 512; i++) {
            sum += p[i] > 0 ? p[i] : (-p[i]);
        }

        total += len;
    }

    int rsize = audio_element_input(self, buf, len);
    if (rsize <= 0) {
        return rsize;
    } else {
        return audio_element_output(self, buf, rsize);
    }
}

static esp_err_t unity_close(audio_element_handle_t self) {
    return ESP_OK;
}
*/

static esp_periph_handle_t periph_cloud = NULL;

esp_err_t periph_cloud_send_event(int cmd, void *data, int data_len) {
    if (!periph_cloud) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_periph_send_event(periph_cloud, cmd, data, data_len);
}

static esp_err_t periph_cloud_init(esp_periph_handle_t self) {
    periph_cloud = self;
    return ESP_OK;
}

static esp_err_t periph_cloud_deinit(esp_periph_handle_t self) {
    periph_cloud = NULL;
    return ESP_OK;
}



static char *data = NULL;
static int data_length = 0;
static int data_played = 0;

static blink_t *blinks = NULL;
static int blinks_array_size = 0;

static int blink_next = -1;
static int64_t blink_start = -1;

static esp_timer_handle_t blink_timer;
static esp_timer_handle_t test_timer;

extern QueueHandle_t ble_queue;

static void blink_done() {
    if (blinks_array_size) {
        blinks_array_size = 0;
        blinks = NULL;
        blink_start = -1;
        blink_next = -1;
        esp_timer_stop(blink_timer);

        message_t msg = {.type = MSG_BLINK_DONE};

        xQueueSend(juggler_queue, &msg, portMAX_DELAY);
    }
}

static void timer_cb(void *arg) {
    if (blink_next < blinks_array_size) {
        if (esp_timer_get_time() - blink_start >
            blinks[blink_next].time * 1000) {
            xQueueSend(ble_queue, &blinks[blink_next].code[0], 0);
            blinks[blink_next].code[34] = 0;
            ESP_LOGI(TAG, "blink: %s", (char *)blinks[blink_next].code);
            blink_next++;
        }
    } else {
        // xQueueSend
    }
}

static void test_timer_cb(void *arg) {
    static int test_counter = 0;
    test_counter++;
    periph_cloud_send_event(PERIPH_CLOUD_CMD_TEST, &test_counter,
                            sizeof(test_counter));
}

/**
 * 1. this function is the only handler
 * 2. no pre-empt (always send to back).
 *
 * so there's no data not played when abort. and as long as data available, play
 * it.
 */
static int mp3_music_read_cb(audio_element_handle_t el, char *buf, int len,
                             TickType_t wait_time, void *ctx) {

top:
    if (data) {
        if (len < data_length - data_played) {
            memcpy(buf, &data[data_played], len);
            data_played += len;
        } else {
            len = data_length - data_played;
            memcpy(buf, &data[data_played], len);

            message_t msg = {
                .type = MSG_AUDIO_DONE,
                .value = {.mem_block = {.length = data_length, .data = data}}};

            xQueueSend(juggler_queue, &msg, portMAX_DELAY);

            data = NULL;
            data_length = 0;
            data_played = 0;
            // no need to further "optimize", there is buffer.
        }
        return len;
    }

    while (1) {
        message_t msg;
        xQueueReceive(audio_queue, &msg, portMAX_DELAY);

        if (msg.type == MSG_BLINK_DATA) {
            blinks_array_size = msg.value.blink_data.blinks_array_size;
            blinks = msg.value.blink_data.blinks;
            blink_start = esp_timer_get_time();
            blink_next = 0;
            esp_timer_start_periodic(blink_timer, 100000);

            ESP_LOGI(TAG, "blinks array size: %d", blinks_array_size);

        } else if (msg.type == MSG_AUDIO_DATA) {
            data = msg.value.mem_block.data;
            data_length = msg.value.mem_block.length;
            goto top;
        } else if (msg.type == MSG_BLINK_ABORT) {
            blink_done();
        }
    }
}

/**
 * audible
 * 1. recv cloud command from event interface
 * 2. recv audio data from audible queue
 * 
 * audible has only one 
 */
void audible(void *arg) {
    esp_timer_init();
    esp_timer_create_args_t args = {
        .callback = &timer_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "blink",
    };
    esp_timer_create(&args, &blink_timer);

    esp_timer_create_args_t test_args = {
        .callback = &test_timer_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "test",
    };
    esp_timer_create(&test_args, &test_timer);

    audio_pipeline_handle_t pipeline;
    audio_element_handle_t i2s_stream_writer, mp3_decoder;

    ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH,
                         AUDIO_HAL_CTRL_START);

    int player_volume;
    audio_hal_get_volume(board_handle->audio_hal, &player_volume);

    ESP_LOGI(TAG, "[ 2 ] Create audio pipeline, add all elements to pipeline, "
                  "and subscribe pipeline event");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

#if 0
#define MP3_DECODER_TASK_STACK_SIZE (5 * 1024)
#define MP3_DECODER_TASK_CORE (0)
#define MP3_DECODER_TASK_PRIO (5)
#define MP3_DECODER_RINGBUFFER_SIZE (2 * 1024)

#define DEFAULT_MP3_DECODER_CONFIG()                                           \
    {                                                                          \
        .out_rb_size = MP3_DECODER_RINGBUFFER_SIZE,                            \
        .task_stack = MP3_DECODER_TASK_STACK_SIZE,                             \
        .task_core = MP3_DECODER_TASK_CORE,                                    \
        .task_prio = MP3_DECODER_TASK_PRIO, .stack_in_ext = true,              \
    }
#endif

    ESP_LOGI(TAG, "[2.1] Create mp3 decoder to decode mp3 file and set custom "
                  "read callback");
    mp3_decoder_cfg_t mp3_cfg = {
        .out_rb_size = 128 * 1024,
        .task_stack = 6 * 1024,
        .task_core = 1,
        .task_prio = 20,
        .stack_in_ext = true,
    };
    mp3_decoder = mp3_decoder_init(&mp3_cfg);
    audio_element_set_read_cb(mp3_decoder, mp3_music_read_cb, NULL);

    ESP_LOGI(TAG, "[2.2] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[2.3] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, mp3_decoder, "mp3");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG,
             "[2.4] Link it together "
             "[mp3_music_read_cb]-->mp3_decoder-->i2s_stream-->[codec_chip]");
    const char *link_tag[2] = {"mp3", "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 2);

    ESP_LOGI(TAG, "[ 3 ] Initialize peripherals");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    ESP_LOGI(TAG, "[3.1] Initialize keys on board");

    audio_board_key_init(set);

    /**
        periph_button_cfg_t btn_cfg = {
            .gpio_mask = (1ULL << get_input_rec_id()) |
                         (1ULL << get_input_mode_id()), // REC BTN & MODE BTN
        };
        esp_periph_handle_t button_handle = periph_button_init(&btn_cfg);
        esp_periph_start(set, button_handle);
    */

    ESP_LOGI(TAG, "[ 4 ] Set up event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    // this is a stack variable, not the global one
    // the global one is set/unset in init/deinit function
    esp_periph_handle_t periph_cloud =
        esp_periph_create(PERIPH_ID_CLOUD, "periph_cloud");
    assert(periph_cloud);
    esp_periph_set_data(periph_cloud, NULL);
    esp_periph_set_function(periph_cloud, periph_cloud_init, NULL,
                            periph_cloud_deinit);

    esp_periph_start(set, periph_cloud);

    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGW(TAG, "[ 5 ] Tap touch buttons to control music player:");
    ESP_LOGW(TAG, "      [Play] to start, pause and resume, [Set] to stop.");
    ESP_LOGW(TAG, "      [Vol-] or [Vol+] to adjust volume.");

    ESP_LOGI(TAG, "[ 5.1 ] Start audio_pipeline");
    // set_next_file_marker();
    audio_pipeline_run(pipeline);

    esp_timer_start_periodic(test_timer, 1000000 * 5);

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            continue;
        }

        if (msg.source_type == PERIPH_ID_CLOUD) {
            switch (msg.cmd) {
            case PERIPH_CLOUD_CMD_PLAY: {
                /**
                 * there is a little chance that this process
                 * race with esp_timer callback , ie. a mutex for
                 * play_context may be necessary
                 */

                /**
                 * blinking is stopped SYNCHRONOUSLY by stopping timer
                 * play_context is updated instantly, and read_cb will
                 * check play_index for each mem_block.
                 *
                 * index, tracks array and url is sent to next handler
                 * immediately even if there is no tracks, which acting 
                 * as a stop
                 */
                play_context_t *p;
                if (play_context) {
                    // is this idempotent? answer: it returns error if
                    // timer is already stopped, but harmless
                    esp_timer_stop(blink_timer);
                    if (play_context->blinks) {
                        free(play_context->blinks);
                    }
                }

                p = (play_context_t*)msg.data;
                message_t msg = {.type = MSG_CMD_PLAY};
                msg.value.play_data.index = p->index;
                msg.value.play_data.tracks_url = p->tracks_url;
                msg.value.play_data.tracks_array_size = p->tracks_array_size;
                msg.value.play_data.tracks = p->tracks;
                
                // handler error, otherwise memory leak
                if (pdTRUE != xQueueSend(juggler_queue, &msg, portMAX_DELAY)) {
                    // free memory if ownership not transferred to next
                    if (p->tracks_array_size > 0) {
                        free(p->tracks_url);    
                        free(p->tracks);
                    }
                }
                p->tracks_url = NULL;
                p->tracks = NULL;
                play_context = p;

                ESP_LOGI(TAG, "play command (%u) processed", p->index);
            } break;
            case PERIPH_CLOUD_CMD_TEST:
                ESP_LOGI(TAG, "periph_cloud test timer counts %d",
                         *((int *)msg.data));
                break;
            default:
                break;
            }
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void *)mp3_decoder &&
            msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(mp3_decoder, &music_info);
            ESP_LOGI(TAG,
                     "[ * ] Receive music info from mp3 decoder, "
                     "sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits,
                     music_info.channels);
            audio_element_setinfo(i2s_stream_writer, &music_info);
            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates,
                               music_info.bits, music_info.channels);
            continue;
        }

        if ((msg.source_type == PERIPH_ID_TOUCH ||
             msg.source_type == PERIPH_ID_BUTTON ||
             msg.source_type == PERIPH_ID_ADC_BTN) &&
            (msg.cmd == PERIPH_TOUCH_TAP || msg.cmd == PERIPH_BUTTON_PRESSED ||
             msg.cmd == PERIPH_ADC_BUTTON_PRESSED)) {
            if ((int)msg.data == get_input_play_id()) {
                ESP_LOGI(TAG, "[ * ] [Play] touch tap event");
                audio_element_state_t el_state =
                    audio_element_get_state(i2s_stream_writer);
                switch (el_state) {
                case AEL_STATE_INIT:
                    ESP_LOGI(TAG, "[ * ] Starting audio pipeline");
                    audio_pipeline_run(pipeline);
                    break;
                case AEL_STATE_RUNNING:
                    ESP_LOGI(TAG, "[ * ] Pausing audio pipeline");
                    audio_pipeline_pause(pipeline);
                    break;
                case AEL_STATE_PAUSED:
                    ESP_LOGI(TAG, "[ * ] Resuming audio pipeline");
                    audio_pipeline_resume(pipeline);
                    break;
                case AEL_STATE_FINISHED:
                    ESP_LOGI(TAG, "[ * ] Rewinding audio pipeline");
                    audio_pipeline_reset_ringbuffer(pipeline);
                    audio_pipeline_reset_elements(pipeline);
                    audio_pipeline_change_state(pipeline, AEL_STATE_INIT);
                    // set_next_file_marker();
                    audio_pipeline_run(pipeline);
                    break;
                default:
                    ESP_LOGI(TAG, "[ * ] Not supported state %d", el_state);
                }
            } else if ((int)msg.data == get_input_set_id()) {
                ESP_LOGI(TAG, "[ * ] [Set] touch tap event, do nothing");
            } else if ((int)msg.data == get_input_mode_id()) {
                ESP_LOGI(TAG, "[ * ] [mode] tap event");
                audio_pipeline_stop(pipeline);
                audio_pipeline_wait_for_stop(pipeline);
                audio_pipeline_terminate(pipeline);
                audio_pipeline_reset_ringbuffer(pipeline);
                audio_pipeline_reset_elements(pipeline);
                // set_next_file_marker();
                audio_pipeline_run(pipeline);
            } else if ((int)msg.data == get_input_volup_id()) {
                ESP_LOGI(TAG, "[ * ] [Vol+] touch tap event");
                player_volume += 10;
                if (player_volume > 100) {
                    player_volume = 100;
                }
                audio_hal_set_volume(board_handle->audio_hal, player_volume);
                ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);
            } else if ((int)msg.data == get_input_voldown_id()) {
                ESP_LOGI(TAG, "[ * ] [Vol-] touch tap event");
                player_volume -= 10;
                if (player_volume < 0) {
                    player_volume = 0;
                }
                audio_hal_set_volume(board_handle->audio_hal, player_volume);
                ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);
            }
        }
    }
}
