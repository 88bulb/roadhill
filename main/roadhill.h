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

#define BLOCK_NUM_BOUND (16 * 8 * 1024)
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

    /* audio length, in units of 10 milliseconds (centiseconds).
     * for 48000Hz sample rate, 16bit and 2 channels, each unit
     * has 480 * 4 = 1920 bytes.
     * length is not provided by cloud, it is provided by juggler.
     */
    int length;

    /*
     * position in milliseconds
     */
    int position;

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

@1024       superblock, 512 bytes. Once created, never re-written.
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

typedef struct mmcfs {
    uint64_t log_start;
    uint64_t log_sect; // twice bucket_sect

    /* starting sector of bucket */
    uint64_t bucket_start;
    
    /* how much sectors for a bucket */
    uint64_t bucket_sect;

    /* total buckets number, in current implementation, 4096 */
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

typedef enum __attribute__((packed)) {
    MMCFS_FILE_UNUSED = 0,
    MMCFS_FILE_MP3 = 1,
    MMCFS_FILE_PCM = 2,
    MMCFS_FILE_16BIT = 0xffff
} mmcfs_file_type_t;

_Static_assert(sizeof(mmcfs_file_type_t) == 2,
               "mmcfs_file_type_t incorrect size");

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
    md5_digest_t self;

    // for mp3 file, this links to pcm;
    // for pcm file, this links to original mp3 file.
    // there is no need to have an "option field" denoting
    // whether this link is used or not, since a "danling"
    // reference is allowed.
    md5_digest_t link;

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
    mmcfs_file_type_t type; // 0 for unused, 1 for mp3, 2 for pcm

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

typedef struct mmcfs_file_context mmcfs_file_context_t;
typedef mmcfs_file_context_t* mmcfs_file_handle_t;
mmcfs_file_handle_t mmcfs_create_file(md5_digest_t *digest, uint64_t size);
int mmcfs_write_mp3(mmcfs_file_handle_t file, char* buf, size_t len);
int mmcfs_write_pcm(mmcfs_file_handle_t file, char* buf, size_t len);
int mmcfs_commit_file(mmcfs_file_handle_t file);

extern play_context_t play_context;
void cloud_cmd_play();
void cloud_cmd_stop();

void print_frame_request();
void sprint_md5_digest(const md5_digest_t* digest, char* buf, int trunc);

char* make_url(const char* path, const md5_digest_t* digest);

#endif // _ROAD_HILL_HOUSE_


