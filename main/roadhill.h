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

typedef enum {
    MSG_CMD_OTA = 0,
    MSG_CMD_CONFIG,
    MSG_CMD_PLAY,
    MSG_CMD_STOP,
    MSG_CHUNK_FETCHED,
    MSG_DATA_FETCHED,
    MSG_AUDIO_DATA_REQUEST,
    MSG_CHUNK_PLAYED,
    MSG_FETCH_MORE,
    MSG_FETCH_ABORT
} message_type_t;

typedef struct {
    int length;
    char* data;
} mem_block_t;

// change this to tagged union
typedef struct {
    message_type_t type;
    void *data;
} message_t;

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

#define URL_BUFFER_SIZE (1024)
#define TRACK_NAME_LENGTH (32)
#define FILENAME_BUFFER_SIZE (40)

typedef struct {
    uint32_t reply_bits;
    uint32_t reply_serial;
    char tracks_url[URL_BUFFER_SIZE];
    int tracks_array_size;
    track_t* tracks;
    int blinks_array_size;
    blink_t* blinks;
} play_command_data_t;

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
    blink_t* blinks;
    int blinks_array_size;
    int chunk_index;
} chunk_data_t;

typedef struct {
    // TODO use pointer
    char url[URL_BUFFER_SIZE];
    md5_digest_t digest;
    int track_size;

    /** fetch gets chunk_data_t out of this queue */
    int fetch_buffer_size;
    char *fetch_buffer;

    QueueHandle_t fetch_chunk_in;
    QueueHandle_t play_chunk_in;
} fetch_context_t;


