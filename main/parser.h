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

#define BLINK_MASK_SIZE       2
#define BLINK_CODE_SIZE       15

typedef struct blink {
  int time;
  uint8_t mask[BLINK_MASK_SIZE];
  uint8_t code[BLINK_CODE_SIZE]; 
} blink_t;

#define MD5_SIZE              16

typedef struct track {
  uint8_t md5[MD5_SIZE];
  size_t size;
  int time;
} track_t;

typedef struct group group_t;

// TODO support repeat
struct group {
  group_t* parent;
  int time;

  group_t* children;
  int children_array_size;
 
  track_t* tracks;
  int tracks_array_size;

  blink_t* blinks;
  int blinks_array_size;  
};

bool parse_blink_object(cJSON *obj, blink_t *blink);
bool parse_track_object(cJSON *obj, track_t *track);
bool parse_group_object(cJSON *obj, group_t *group);
void destroy_group_object(group_t *group);

#endif
