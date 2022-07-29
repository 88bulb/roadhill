#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/queue.h>

#include "esp_err.h"
#include "cJSON.h"
#include "tools.h"
#include "protocol.h"

/*
 * general properties:
 * all time units are 1/100 second.
 *
 * track object
 * 1. name, md5 value in hex string, mandatory
 * 2. size, positive integer, mandatory
 * 3. time, non-negative integer, relative to current container, mandatory
 * 4. begin, crop begin of mp3 file, inclusive, optional, defaults to 0
 * 5. end, crop end of mp3 file, exclusive, optional, defaults to length (or -1) 
 * 6. chan, non-negtive integer
 *
 * blink object
 * 1. time
 * 2. mask uint8_t array[2] encoded in hex string
 * 3. code uint8_t array[15] encoded in hex string
 * 
 * group object
 * 
 * 0. has [0-n] children
 * 1. has [0-n] tracks
 * 2. has [0-n] blinks
 * 3. time
 * 4. repeat (1..n)
 * 5. dur(ation), mandatory if repeat not 0. otherwise neglected.
 *
 * root group extra
 * 0. api_version
 * 1. method: PLAY
 * 2. tracks_url: string 
 * 3. request_id? what is this and how to use it?
 * 4. result? what is this?
 */

/*
 * parse a blink object
 *
 * @param obj - pointer to 
 */
bool parse_blink_object(cJSON *obj, blink_t **pp) {
  if (!obj) return false;

  cJSON *time = cJSON_GetObjectItem(obj, "time");
  if (!time || !cJSON_IsNumber(time) || time->valueint < 0)
    return false;

  cJSON *mask = cJSON_GetObjectItem(obj, "mask");
  if (!mask || !cJSON_IsString(mask) ||
      !hex_string_to_bytes(mask->valuestring, NULL, 2))
    return false;

  cJSON *code = cJSON_GetObjectItem(obj, "code");
  if (!mask || !cJSON_IsString(code) ||
      !hex_string_to_bytes(code->valuestring, NULL, 15))
    return false;

  if (!pp) return true;

  blink_t* blink = (blink_t*)malloc(sizeof(blink_t));
  if (!blink) return false;
  
  blink->time = time->valueint;
  hex_string_to_bytes(mask->valuestring, blink->mask, 2);
  hex_string_to_bytes(code->valuestring, blink->code, 15);  

  *pp = blink;
  return true;
}

bool parse_track_object(cJSON *obj, track_t **pp) {
  if (!obj) return false;

  cJSON *name = cJSON_GetObjectItem(obj, "name");
  if (!name || !cJSON_IsString(name) ||
      !hex_string_to_bytes(name->valuestring, NULL, 16))
    return false;

  cJSON *size = cJSON_GetObjectItem(obj, "size");
  if (!size || !cJSON_IsNumber(size) || size->valueint <= 0)
    return false;

  cJSON *time = cJSON_GetObjectItem(obj, "time");
  if (!time || !cJSON_IsNumber(time) || time->valueint < 0)
    return false;

  cJSON *crop_begin = cJSON_GetObjectItem(obj, "begin");
  if (crop_begin && (!cJSON_IsNumber(crop_begin) || crop_begin->valueint < 0))
    return false;

  cJSON *crop_end = cJSON_GetObjectItem(obj, "end");
  if (crop_end) {
    if (!cJSON_IsNumber(crop_end))
      return false;
    if (crop_begin) {
      if (crop_end->valueint <= crop_begin->valueint)
        return false;
    } else {
      if (crop_end->valueint <= 0)
        return false;
    }
  }

  track_t* track = (track_t*)malloc(sizeof(track_t));
  if (!track) return false;

  hex_string_to_bytes(name->valuestring, track->md5.bytes, 16);
  track->size = size->valueint;
  track->time = time->valueint;
  track->crop_begin = crop_begin ? crop_begin->valueint : 0;
  track->crop_end = crop_end ? crop_end->valueint : 0; 
  *pp = track;
  return true;
}

bool parse_group_object(cJSON* obj, group_t *group) {
  if (!obj) return NULL;

  cJSON *children = cJSON_GetObjectItem(obj, "children");
  if (children) {
    if (!cJSON_IsArray(children)) {
      return false;
    }

    int array_size = cJSON_GetArraySize(children);
  }

  
  cJSON *time = cJSON_GetObjectItem(obj, "time");
  if (!time || !cJSON_IsNumber(time) || time->valueint < 0)
    return false;
}

/*
esp_err_t parse_play_cmd(cJSON* root) {

  cJSON *blinks = NULL;
  tracks = cJSON_GetObjectItem(root, "tracks");

  


  mtailhead_t head;
  TAILQ_INIT(&head);

  mentry_t* root_entry = (mentry_t*) malloc(sizeof(mentry_t));
  memset(root_entry, 0, sizeof(mentry_t));
  root_entry->type = MT_ANCHOR; 
  TAILQ_INSERT_TAIL(&head, root_entry, entries);
 
} */

