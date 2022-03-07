#ifndef _ROAD_HILL_HOUSE_
#define _ROAD_HILL_HOUSE_

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "audio_common.h"

// used by mem_block_t
#define MEM_BLOCK_SIZE (32768)

#define FRAME_BUF_SIZE (8 * 1024)
#define FRAME_DAT_SIZE (15 * 512)
#define PIC_BLOCK_SIZE (8 * 1024)

#define MMCFS_FILE_PER_BUCKET   (16)

#define container_of(ptr, type, member)                                        \
    ({                                                                         \
        const typeof(((type *)0)->member) *__mptr = (ptr);                     \
        (type *)((char *)__mptr - ))offsetof(type, member));                   \
    })

#define PERIPH_ID_EMITTER   (AUDIO_ELEMENT_TYPE_PERIPH + 0xc1)

#define CLOUD_CMD_OTA (0)
#define CLOUD_CMD_CONFIG (1)
#define CLOUD_CMD_PLAY (2)
#define CLOUD_CMD_STOP (3)
#define TEST_TIMER_FIRE (0x7e57)

// #define BLOCK_NUM_BOUND (16 * 8 * 1024)
#define BLOCK_NUM_FRACT 7 / 8 // delibrately no parenthesis
#define BUCKET_BITS (12)
#define BUCKET_SIZE (1024)
#define META_OFFSET (((uint64_t)1 << BUCKET_BITS) * BUCKET_SIZE) // 4MB
#define DATA_OFFSET (META_OFFSET * 2)                            // 8MB
#define WLOG_OFFSET (META_OFFSET / 2)                            // 2MB
#define URL_BUFFER_SIZE (512)
#define TRACK_NAME_LENGTH (32)
#define FILENAME_BUFFER_SIZE (40)

#define MD5_HEX_STRING_SIZE (16 * 2 + 1)
#define MD5_HEX_BUF_SIZE    (16 * 2 + 1)

typedef struct play_context play_context_t;
typedef struct fetch_context fetch_context_t;
typedef struct pacman_context pacman_context_t;
typedef struct picman_context picman_context_t;

extern QueueHandle_t play_context_queue;
extern QueueHandle_t tcp_send_queue;

extern const char hex_char[16];

/*
 * ota command (url)
 */
typedef struct {
    char url[1024];
} ota_command_data_t;

/*
 * md5 type
 */
typedef struct {
    uint8_t bytes[16];
} md5_digest_t;

/*
 * track
 */
typedef struct {
    md5_digest_t digest;
    /* mp3 file size */
    int size;

    /*
     * position in milliseconds
     */
    int position_ms;

    /*
     *
     */
    int pos;
    int len;

    /*
     * begin
     */
    int begin;
    int end;
    int chan;
} track_t;

typedef struct {
    track_t* track;
    int pos;
    int len;
} track_mix_t;

typedef struct {
    int time;
    uint8_t code[40];
} blink_t;

typedef struct {
    esp_err_t err;
    char *data;
} fetch_error_t;

typedef struct {
    int play_index;
    int length;
    char *data;
} mem_block_t;

/** for MSG_CMD_PLAY */
typedef struct {
    uint32_t index;
    char *tracks_url;
    int tracks_array_size;
    track_t *tracks;
} play_data_t;

typedef struct {
    int blinks_array_size;
    blink_t *blinks;
} blink_data_t;

typedef enum {
    MSG_CMD_OTA = 0,
    MSG_CMD_CONFIG,
    MSG_CMD_PLAY,
    MSG_CMD_STOP,

    /** from juggler to fetcher */
    MSG_FETCH_MORE,

    /**
     * from juggler to fetcher
     */
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

typedef struct message {
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
        play_data_t play_data;
    } value;
} message_t;

typedef struct juggler_ports {
    QueueHandle_t in;
    QueueHandle_t out;
} juggler_ports_t;

typedef enum juggler_response {
    JUG_REQ_FULFILLED,  // succeeded
    JUG_REQ_REJECTED,   // failed, including cancelled.
} juggler_response_t;

/*
 * each request request fixed number of slices
 *
 * each slice is 8KiB in size, containing 15 sectors of pcm data
 * and 1 sector of metadata, volume and fft results perhaps.
 * 15 * 512 pcm data contains 15 * 512 / 4 = 1920 samples, which
 * in turn translates into 1920 / 48000 = 0.04s. Which means the *frame* rate
 * is 25fps, if the metadata is used for displaying a spectrum visualizer.
 * 
 * the buffer size must be a multiple of 8192.
 */
typedef struct {
    // player set this index incrementally
    int index;
    // juggler set this value
    juggler_response_t res;
    // point to the same string in play_context
    const char* url;
    // if not used, set track (track_t*) to NULL
    track_mix_t track_mix[2];

    char buf[8192]; 
} frame_request_t;

struct picman_context {
    QueueHandle_t in;
    QueueHandle_t out;
};

/*
 * fetch a file
 */
typedef struct {
    const char *url;
    const md5_digest_t *digest;
    int size;
} picman_inmsg_t;

typedef struct {
    char *data;
    int size_or_error;
} picman_outmsg_t;

/* use to initiate a pacman task */
struct pacman_context {
    QueueHandle_t in;
    QueueHandle_t out;
};

typedef enum {
    MP3_STREAM_START,
    MP3_STREAM_WRITE,
    MP3_STREAM_END,
} pacman_inmsg_type_t;

typedef struct {
    char *data;
    uint32_t len;
} pacman_inmsg_t;

typedef enum {
    PCM_IN_DRAIN,
    PCM_OUT_DATA,
    PCM_OUT_ERROR,
    PCM_OUT_FINISH
} pacman_outmsg_type_t;

/*
 * 
 */
typedef struct {
    pacman_outmsg_type_t type;
    char *data;
    uint32_t len;
} pacman_outmsg_t;

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
    SemaphoreHandle_t lock;
    uint32_t index;

    /** reserved */
    uint32_t reply_bits;
    /** reserved */
    uint32_t reply_serial;

    char *tracks_url;
    int tracks_array_size;
    track_t *tracks;

    int blinks_array_size;
    blink_t *blinks;
};

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

extern play_context_t play_context;
void cloud_cmd_play();
void cloud_cmd_stop();

void print_frame_request();
void sprint_md5_digest(const md5_digest_t* digest, char* buf, int trunc);

char* make_url(const char* path, const md5_digest_t* digest);

#endif // _ROAD_HILL_HOUSE_


