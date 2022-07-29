#ifndef APPLICATION_PROTOCOL_H
#define APPLICATION_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <sys/queue.h>

#include "tools.h"

/*
 * This is a basic example of a root group, without subgroups
 * 
 * ```json
 * {
 *    "api_version":1,
 *    "method":"PLAY",
 *    "tracks_url":"http:\/\/myqcloud.com\/sgd_audio",
 *    "tracks":[
 *       {
 *          "name":"68da94e8526e2d669f77d07d65fd4845",
 *          "size":1655788,
 *          "time":0,
 *          "begin":0,
 *          "end":0,
 *          "repeat":1,
 *          "chan":0
 *       }
 *    ],
 *    "blinks":[
 *       {
 *          "time":68800,
 *          "mask":"ffff",
 *          "code":"100200000000000000000000000000"
 *       }
 *    ],
 *    "request_id":"xk7k0yseg7d8kpmx1nhvf4ril8t58831",
 *    "result":{
 *       
 *    }
 * }
 * ```
 */

typedef struct blink {
  int time;
  uint8_t mask[2];
  uint8_t code[15]; 
} blink_t;

typedef struct track {
  md5_digest_t md5;
  size_t size;
  int time;
  int crop_begin;
  int crop_end;
} track_t;

typedef enum align_type {
  ALIGN_BEGIN,
  ALIGN_END,
} align_type_t;

typedef struct mentry mentry_t;

struct __attribute__((packed)) group_entry {
  TAILQ_ENTRY(track_entry) entries;

  uint8_t repeat;         

  mentry_t* origin;         // mentry
  int32_t pos;            // only non-negative value supported now
};

TAILQ_HEAD(mtailhead, mentry);

typedef struct mtailhead mtailhead_t;

#endif
