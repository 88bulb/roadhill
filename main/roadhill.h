#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define container_of(ptr, type, member)                                        \
    ({                                                                         \
        const typeof(((type *)0)->member) *__mptr = (ptr);                     \
        (type *)((char *)__mptr - ))offsetof(type, member));                   \
    })

typedef struct {
    char url[1024];
} ota_command_data_t;

typedef struct {
    uint8_t bytes[16];
} md5_digest_t;

typedef struct {
    md5_digest_t digest;
    int size;
} track_t;

typedef struct {
    int time;
    uint8_t code[40];
} blink_t;

typedef enum {
    MSG_CMD_OTA = 0,
    MSG_CMD_CONFIG,
    MSG_CMD_PLAY,
    MSG_CMD_STOP,

    /** from juggler to fetcher,  */
    MSG_FETCH_MORE,
    MSG_FETCH_ABORT,
    MSG_FETCH_MORE_DATA,
    MSG_FETCH_FINISH,
    MSG_FETCH_ERROR,
    MSG_FETCH_ABORTED,

    MSG_BLINK_DATA,
    MSG_BLINK_ABORT,
    MSG_BLINK_DONE,

    MSG_AUDIO_DATA,
    MSG_AUDIO_DONE,
} message_type_t;

typedef struct {
    esp_err_t err;
    char *data;
} fetch_error_t;

typedef struct {
    int length;
    char *data;
} mem_block_t;

typedef struct {
    int blinks_array_size;
    blink_t *blinks;
} blink_data_t;

#define URL_BUFFER_SIZE (512)
#define TRACK_NAME_LENGTH (32)
#define FILENAME_BUFFER_SIZE (40)
/**
typedef struct {
    uint32_t reply_bits;
    uint32_t reply_serial;

    char tracks_url[URL_BUFFER_SIZE];
    int tracks_array_size;
    track_t* tracks;
    int blinks_array_size;
    blink_t* blinks;
} play_command_data_t;
*/

typedef struct play_context play_context_t;
typedef struct fetch_context fetch_context_t;

struct fetch_context {
    // TODO use pointer
    char url[URL_BUFFER_SIZE];
    md5_digest_t digest;
    int track_size;
    bool play_started;

    QueueHandle_t input;

    play_context_t *play_ctx;
};

/**
 * A play may or may not have tracks (may be lighting only);
 * A play may or may not have blinks (may be music only);
 *
 * 'live jobs' including
 * 1. music play (optional); Whether music play finished? check file_read /
 * size)
 * 2. blink (optional); To check whether blink finished? should have a flag;
 * 3. file reading or reading/writing;
 * 4. fetching file or files (fetch finished if current file written = file
 * size;
 *
 * there may be "detached" fetcher. a fetcher may be detached and attached
 * to a new job if it happens to be the tracks[0] of the new play.
 */
struct play_context {
    /** reserved */
    uint32_t reply_bits;
    /** reserved */
    uint32_t reply_serial;

    char tracks_url[URL_BUFFER_SIZE];

    int tracks_array_size;
    track_t *tracks;

    int blinks_array_size;
    blink_t *blinks;

    fetch_context_t *fetch_ctx;
};

typedef struct {
    message_type_t type;

    /**
     * most components are singleton，and type clearly encoded the
     * source and target of a message. The only exception is
     * fetcher, which may have multiple instance in an optimized implementation.
     */
    void *from;

    union {
        fetch_error_t fetch_error;
        mem_block_t mem_block;
        blink_data_t blink_data;
        play_context_t *play_context;
    } value;
} message_t;

/********************************************************************************
 *
 *
 *
 *
 ********************************************************************************/
typedef struct {
    md5_digest_t file_digest; // 16 bytes
    int file_size;            // 4 bytes
    int chunk_index;          // 4 bytes
    int chunk_size;           // 4 bytes
} chunk_metadata_t;

typedef struct {
    chunk_metadata_t metadata;
    const char *path;
    FILE *fp;
    md5_digest_t chunk_digest;
    blink_t *blinks;
    int blinks_array_size;
    int chunk_index;
} chunk_data_t;
