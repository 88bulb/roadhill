#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define MD5_SIZE            (128 / 8)

struct md5_digest;

typedef enum queue_id {
  MAIN_QUEUE,
  OTA_QUEUE,
  TCP_QUEUE,
  WGET_QUEUE,
  TRANS_QUEUE,
  PLAY_QUEUE, 
} queue_id_t;

/*
 * message type for all messages, not only message for main loop
 */
typedef enum msg_type {
  /* clang-format off */
  STA_GOT_IP,   // low level for main
  STA_LOST_IP,  // low level for main 

  NETIF,        /* main -> ??? */
  TCP_SEND,     /* others -> tcp */
  TCP_CONNECT,
  TCP_DISCONNECT,

  WGET,               /* main -> wget */
  WGET_CANCEL,        /* main -> wget */
  WGET_DATA,          /* wget -> main */
  WGET_DONE,          /* wget -> main */

  TRANSCODE,          /* main -> transcode, transcode_t */  
  TRANSCODE_CANCEL,   /* main -> transcode, NULL */
  TRANSCODE_DATA,     /* trans -> main, transcode_data_t */ 
  TRANSCODE_DONE,     /* trans -> main, transcode_done_t */

  OTA,          /* main -> ota */
  OTA_DONE,     /* ota -> main */
  CLOUD_CMD_PLAY,
  /* clang-format on */
} msg_type_t;

/*
 * general message struct (in queue), data points to
 * message type specific struct, the receiver is responsible to free
 */
typedef struct {
  msg_type_t type;
  void *data;
} msg_t;

/* general purpose message payload */
typedef struct {
  size_t len;
  void* data;
} chunk_t;

/* general purpose operation done */
typedef struct {
  esp_err_t result; 
} result_t;

/*
 *
 */
typedef struct {
  char *base_url;
  size_t size;
  struct md5_digest *md5;
} wget_t;

typedef struct __attribute__((packed)) {
  int16_t prevsample;
  uint8_t previndex;
  uint8_t nibbles[441]; 
} adpcm_data_t;

_Static_assert(sizeof(adpcm_data_t)==444, "wrong transcode data size");

typedef struct {
  char *url;
} ota_t;

void create_msg_queues();
bool send_msg(queue_id_t queue, msg_t *msg);
bool recv_msg(queue_id_t queue, msg_t *msg, bool blocking);
