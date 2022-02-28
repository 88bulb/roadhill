#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"


// used by mem_block_t
#define MEM_BLOCK_SIZE (32768)

#define PIC_BLOCK_SIZE (16 * 1024)

#define container_of(ptr, type, member)                                        \
    ({                                                                         \
        const typeof(((type *)0)->member) *__mptr = (ptr);                     \
        (type *)((char *)__mptr - ))offsetof(type, member));                   \
    })

#define PERIPH_ID_CLOUD (AUDIO_ELEMENT_TYPE_PERIPH + 0xc1)
#define PERIPH_ID_JUGGLER (AUDIO_ELEMENT_TYPE_PERIPH + 0xc2)

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
typedef struct picman_context picman_context_t;

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

struct picman_context {
    QueueHandle_t in;
    QueueHandle_t out;
};

typedef struct {
    char url[URL_BUFFER_SIZE];
    md5_digest_t digest;
    int track_size;
} picman_inmsg_data_t;

typedef picman_inmsg_data_t* picman_inmsg_handle_t;

typedef struct {
    char* data;
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
    PCM_STREAM_DATA,
    PCM_STREAM_ERROR,
    PCM_STREAM_FINISH
} pacman_outmsg_type_t;

typedef struct {
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

@512KB      header, 1024 bytes, with last 16 bytes as md5
@1MB-2KB    writelog, 2048 bytes, dual bucket
@4MB        4 megabytes, include 4096 buckets, each bucket has
            16 records, each record has 64 bytes.
@64MB       data block starts

metadata table @4MB
header @0B and @512KB

write log is dual buckets, 2 * 1024 in size. @1M

header describes:

1. magic
2. version (single uint32_t)
3. secondary header location (no, last 2 sectors)
4. buckets:
    1. size
    2. number of buckets
    3. where starts 
5. writelog offset (twice the size of a bucket)
6. data
    1. starts
    2. block size

*****************************/

#define MMCFS_MAX_BITARRAY_SIZE (16 * 1024 * 8)

typedef struct {
    uint64_t log_start;
    uint64_t log_sect;      // twice bucket_sect
    uint64_t bucket_start;
    uint64_t bucket_sect;
    uint64_t bucket_count;
    uint64_t block_start;
    uint64_t block_sect;
    uint64_t block_count;
} mmcfs_t;

// superblock is located at sector 2 (aka, 1024 bytes)
typedef struct __attribute__((packed)) {
    uint8_t magic[16];
    uint64_t version;
    mmcfs_t mmcfs;
    uint8_t zero_padding[512 - 16 * 2 - sizeof(uint64_t) - sizeof(mmcfs_t)];
    uint8_t md5[16];
} mmcfs_superblock_t;

_Static_assert(sizeof(mmcfs_superblock_t) == 512,
               "mmc_superblock_t size incorrect");

/**
 * 4G   -> 32KiB    * 64k = 2GB
 * 8G   -> 64KiB    * 64K = 4GB
 * 16G  -> 128KiB   * 64K = 8GB
 * 32G  -> 256KiB   * 64K = 16GB
 * 64G  -> 512KiB
 * 128G -> 1MiB
 * 256G -> 2MiB
 * 512G -> 4MiB
 * 1T   -> 8MiB
 * 2T   -> 16MiB
 * 4T   -> 32MiB
 * 8T   -> 64MiB    * 64K = 4TB
 */

/**
 * There may be three TYPEs of file.
 * 1. original mp3 file. 
 *      with or without link
 *      data type: stream
 *      data_size
 *      block_start
 * 2. original mp3 file, pure link, no data (not supported now)
 *      must have a link
 8      no data
 * 3. pcm file, with oob data
 *      must have a link
 *      data_size
 *      data type: packet
 *      packet_size: 16K
 *      packet_data: 15K
 *      packet_position: first or last
 */
typedef struct __attribute__((packed)) {
    // for
    uint8_t self[16];

    // for mp3 file, this links to pcm;
    // for pcm file, this links to original mp3 file.
    // there is no need to have an "option field" denoting
    // whether this link is used or not, since a "danling"
    // reference is allowed.
    uint8_t link[16];

    // access is a global monotonously increasing number
    // it is roughly equivalent to last access time, the larger, the latter
    uint32_t access;

    // starting block, relative to fs->block_start, in block unit (not sector)
    // start is inclusive and end is exclusive
    uint32_t block_start;
    uint32_t block_end;

    // in byte
    // arbitrary non-zero value for mp3
    // must be multiple of (pcm + oob) * 512 for pcm
    uint32_t size;

    //
    uint16_t type; // 0 for unused, 1 for mp3, 2 for pcm

    // for mp3, both are zero
    // for pcm, pcm is modelled as a packet stream, not a 
    uint8_t pcm_sect;
    uint8_t oob_sect;  

    uint32_t zero[3];
} mmcfs_file_t;

_Static_assert(sizeof(mmcfs_file_t) == 64, "mmc_file_t size incorrect");

typedef struct {
  mmcfs_file_t files[16];
} mmcfs_bucket_t;

_Static_assert(sizeof(mmcfs_bucket_t) == 1024, "mmc_bucket_t size incorrect");


