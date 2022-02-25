#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"


// used by mem_block_t
#define MEM_BLOCK_SIZE (32768)

#define container_of(ptr, type, member)                                        \
    ({                                                                         \
        const typeof(((type *)0)->member) *__mptr = (ptr);                     \
        (type *)((char *)__mptr - ))offsetof(type, member));                   \
    })

#define PERIPH_ID_CLOUD (AUDIO_ELEMENT_TYPE_PERIPH + 0xc1)

#define PERIPH_CLOUD_CMD_OTA (0)
#define PERIPH_CLOUD_CMD_CONFIG (1)
#define PERIPH_CLOUD_CMD_PLAY (2)
#define PERIPH_CLOUD_CMD_STOP (3)
#define PERIPH_CLOUD_CMD_TEST (0x7e57)

#define MOUNT_POINT "/mmc"
#define CHUNK_FILE_SIZE (1024 * 1024)
#define TEMP_FILE_PATH "/mmc/temp"

#define BLOCK_NUM_BOUND (16 * 8 * 1024)
#define BLOCK_NUM_FRACT 7 / 8           // delibrately no parenthesis
#define BUCKET_BITS (12)
#define BUCKET_SIZE (1024)
#define META_OFFSET (((uint64_t)1 << BUCKET_BITS) * BUCKET_SIZE) // 4MB
#define DATA_OFFSET (META_OFFSET * 2)   // 8MB
#define WLOG_OFFSET (META_OFFSET / 2)   // 2MB

typedef struct __attribute__((packed)) {
    uint8_t md5sum[16];
    uint32_t size;
    uint32_t access;
    uint32_t blk_addr;
    uint16_t hole_addr;
    uint16_t hole_size;
} siffs_metadata_t;

typedef struct {
    siffs_metadata_t meta[32];
} siffs_metadata_bucket_t;

typedef struct play_context play_context_t;
typedef struct fetch_context fetch_context_t;
typedef struct pacman_context pacman_context_t;

extern QueueHandle_t play_context_queue;
extern QueueHandle_t juggler_queue;
extern QueueHandle_t tcp_send_queue;
extern QueueHandle_t audio_queue;

extern const char hex_char[16];

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
        play_data_t play_data;
//        play_context_t *play_context;
    } value;
} message_t;

#define URL_BUFFER_SIZE (512)
#define TRACK_NAME_LENGTH (32)
#define FILENAME_BUFFER_SIZE (40)

struct fetch_context {
    // TODO use pointer
    char url[URL_BUFFER_SIZE];
    md5_digest_t digest;
    int track_size;
    uint32_t play_index; 
    bool play_started;

    QueueHandle_t input;

    play_context_t *play_ctx;
};

/* use to initiate a pacman task */
struct pacman_context {
    FILE* in;
    FILE* out;
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

/****************************

Disk Format ver 1

first, calculate metadata table size:

4096 buckets in total, each bucket has 1024 bytes, thus 4MB for metadata table.

each record has 64 byte, thus each bucket has 1024 / 64 = 16 records. So if all
buckets are full, there are 4096 * 16 = 64KB records. For a 128GB mmc, if all
file sizes are equal, then each file has roughly 2MB. In reality, most mp3 file
will be several metabytes and pcm files will be tens of metabytes. So the bucket
number and record in buckets are enough, as estimated.

Layout:

|--------|--------|=-=-=-=-=-=-=||
|        |xxxxxxxx|             ||
         4M       8M            64M

metadata table @4MB
header @0B and @512KB

write log is dual buckets, 2 * 1024 in size. @1M

header describes:

1. magic
2. version (single uint32_t)
3. secondary header location
4. buckets:
    1. size
    2. number of buckets
    3. where starts 
5. writelog offset (twice the size of a bucket)
6. data
    1. starts
    2. block size

*****************************/


