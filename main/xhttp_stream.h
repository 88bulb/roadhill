#ifndef _XHTTP_STREAM_H_
#define _XHTTP_STREAM_H_

#include "audio_error.h"
#include "audio_element.h"
#include "audio_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief      HTTP Stream hook type
 */
typedef enum {
    /* The event handler will be called before HTTP Client making the connection
       to the server */
    XHTTP_STREAM_PRE_REQUEST = 0x01,

    /* The event handler will be called when HTTP
     * Client is requesting data,     If the fucntion
     * return     the value (-1: ESP_FAIL), HTTP Client
     * will     be     stopped     If the fucntion return
     * the value >     0,     HTTP Stream will ignore the
     * post_field     If     the     fucntion return the
     * value = 0, HTTP Stream     continue send data from
     * post_field (if any)
     */
    HTTP_STREAM_ON_REQUEST,

    /*!< The event handler will be called when HTTP
     * Client is receiving data    If the fucntion
     * return    the value (-1: ESP_FAIL), HTTP Client
     * will be    stopped    If the fucntion return the
     * value > 0,    HTTP Stream will ignore the read
     * function    If the    fucntion return the value
     * =    0, HTTP Stream    continue read data from
     * HTTP    Server
     */
    HTTP_STREAM_ON_RESPONSE,

    /* The event handler will be called after HTTP Client send header and body
     * to the server, before fetching the headers
     */
    HTTP_STREAM_POST_REQUEST,
    /* The event handler will be called after HTTP Client fetch the header and
       ready to read HTTP body */
    HTTP_STREAM_FINISH_REQUEST,
    HTTP_STREAM_RESOLVE_ALL_TRACKS,
    HTTP_STREAM_FINISH_TRACK,
    HTTP_STREAM_FINISH_PLAYLIST,
} xhttp_stream_event_id_t;

/**
 * @brief      Stream event message
 */
typedef struct {
    xhttp_stream_event_id_t event_id; /*!< Event ID */
    void
        *http_client; /*!< Reference to HTTP Client using by this HTTP Stream */
    void *buffer;     /*!< Reference to Buffer using by the Audio Element */
    int buffer_len;   /*!< Length of buffer */
    void *user_data;  /*!< User data context, from `xhttp_stream_cfg_t` */
    audio_element_handle_t el; /*!< Audio element context */
} xhttp_stream_event_msg_t;

typedef int (*xhttp_stream_event_handle_t)(xhttp_stream_event_msg_t *msg);

/**
 * @brief      HTTP Stream configurations
 *             Default value will be used if any entry is zero
 */
typedef struct {
    audio_stream_type_t type; /*!< Type of stream */
    int out_rb_size;          /*!< Size of output ringbuffer */
    int task_stack;           /*!< Task stack size */
    int task_core;            /*!< Task running in core (0 or 1) */
    int task_prio;            /*!< Task priority (based on freeRTOS priority) */
    bool stack_in_ext;        /*!< Try to allocate stack in external memory */
    xhttp_stream_event_handle_t
        event_handle;             /*!< The hook function for HTTP Stream */
    void *user_data;              /*!< User data context */
    bool auto_connect_next_track; /*!< connect next track without open/close */
    bool enable_playlist_parser;  /*!< Enable playlist parser*/
    int multi_out_num;            /*!< The number of multiple output */
} xhttp_stream_cfg_t;

#define XHTTP_STREAM_TASK_STACK (6 * 1024)
#define XHTTP_STREAM_TASK_CORE (0)
#define XHTTP_STREAM_TASK_PRIO (4)
#define XHTTP_STREAM_RINGBUFFER_SIZE (20 * 1024)

#define XHTTP_STREAM_CFG_DEFAULT()                                              \
    {                                                                          \
        .type = AUDIO_STREAM_READER,                                           \
        .out_rb_size = XHTTP_STREAM_RINGBUFFER_SIZE,                            \
        .task_stack = XHTTP_STREAM_TASK_STACK,                                  \
        .task_core = XHTTP_STREAM_TASK_CORE,                                    \
        .task_prio = XHTTP_STREAM_TASK_PRIO, .stack_in_ext = true,              \
        .event_handle = NULL, .user_data = NULL,                               \
        .auto_connect_next_track = false, .enable_playlist_parser = false,     \
        .multi_out_num = 0,                                                    \
    }

/**
 * @brief      Create a handle to an Audio Element to stream data from HTTP to
 * another Element or get data from other elements sent to HTTP, depending on
 * the configuration the stream type, either AUDIO_STREAM_READER or
 * AUDIO_STREAM_WRITER.
 *
 * @param      config  The configuration
 *
 * @return     The Audio Element handle
 */
audio_element_handle_t xhttp_stream_init(xhttp_stream_cfg_t *config);

/**
 * @brief      Connect to next track in the playlist.
 *
 *             This function can be used in event_handler of xhttp_stream.
 *             User can call this function to connect to next track in playlist
 * when he/she gets `HTTP_STREAM_FINISH_TRACK` event
 *
 * @param      el  The xhttp_stream element handle
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_FAIL on errors
 */
esp_err_t xhttp_stream_next_track(audio_element_handle_t el);
esp_err_t xhttp_stream_restart(audio_element_handle_t el);

/**
 * @brief       Try to fetch the tracks again.
 *
 *              If this is live stream we will need to keep fetching URIs.
 *
 * @param       el  The xhttp_stream element handle
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_NOT_SUPPORTED if playlist is finished
 */
esp_err_t xhttp_stream_fetch_again(audio_element_handle_t el);

#ifdef __cplusplus
}
#endif

#endif
