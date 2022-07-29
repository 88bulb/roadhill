#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/queue.h>

#include "esp_err.h"
#include "esp_log.h"

#include "cJSON.h"
#include "tools.h"
#include "parser.h"

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

static const char *TAG = "parser";

/**
 * parse a blink object
 *
 * @param obj - pointer to JSON object
 * @param 
 */
bool parse_blink_object(cJSON *obj, blink_t *blink) {

  if (!obj)
    return false;

  ESP_LOGI(TAG, "welcome tester");

  cJSON *time = cJSON_GetObjectItem(obj, "time");
  if (!time || !cJSON_IsNumber(time) || time->valueint < 0) {
    ESP_LOGI(TAG, "error parsing blink.time");
    return false;
  }

  cJSON *mask = cJSON_GetObjectItem(obj, "mask");
  if (!mask || !cJSON_IsString(mask) ||
      !hex_string_to_bytes(mask->valuestring, NULL, 2)) {
    ESP_LOGI(TAG, "error parsing blink.mask");
    return false;
  }

  cJSON *code = cJSON_GetObjectItem(obj, "code");
  if (!mask || !cJSON_IsString(code) ||
      !hex_string_to_bytes(code->valuestring, NULL, 15)) {
    ESP_LOGI(TAG, "error parsing blink.code");
    return false;
  }

  if (blink) {
    blink->time = time->valueint;
    hex_string_to_bytes(mask->valuestring, blink->mask, 2);
    hex_string_to_bytes(code->valuestring, blink->code, 15);
  }

  ESP_LOGI(TAG, "parsing blink success");
  return true;
}

/**
 * parse a track json object
 *
 * @param[in]     obj   The cJSON obj
 * @param[out]    track Pointer to track object, if NULL, dryrun.
 * @return              True if success.
 */
bool parse_track_object(cJSON *obj, track_t *track) {
  if (!obj)
    return false;

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

  if (track) {
    hex_string_to_bytes(name->valuestring, track->md5.bytes, 16);
    track->size = size->valueint;
    track->time = time->valueint;
  }
  return true;
}

/**
 * @brief
 *
 * 
 */
void destroy_group_object(group_t *group)
{
  if (group->children) {
    for (int i = 0; i < group->children_array_size; i++) {
      destroy_group_object(&group->children[i]);
    } 
    free(group->children);
    group->children = NULL;
  } 

  if (group->tracks) {
    free(group->tracks);
    group->tracks = NULL;
  }

  if (group->blinks) {
    free(group->blinks);
    group->blinks = NULL;
  }
}

/**
 * @brief Parse a group object
 *
 * A group object has
 * - [0..n] children (also group object)
 * - [0..n] tracks
 * - [0..n] blinks
 * - (optional) repeat, defaults to 1 if not provided. 0 means endless.
 * - (dependent) length, required if repeat is provided.
 */
bool parse_group_object(cJSON *obj, group_t *group) {
  if (!obj)
    return false;

  cJSON *children = cJSON_GetObjectItem(obj, "children");
  if (children) {
    if (!cJSON_IsArray(children)) {
      return false;
    }

    int children_array_size = cJSON_GetArraySize(children);
    int children_array_mem_size = sizeof(group_t) * children_array_size;

    if (group) {    
      group->children_array_size = children_array_size;
      group->children = (group_t *)malloc(children_array_mem_size);
      if (group->children == NULL) {
        return false;
      }

      memset(group->children, 0, children_array_mem_size);
    }

    for (int i = 0; i < children_array_size; i++) {
      cJSON *item = cJSON_GetArrayItem(children, i);
      if (!parse_group_object(item, group ? &group->children[i] : NULL)) {
        return false;
      }
    }
  }

  cJSON *tracks = cJSON_GetObjectItem(obj, "tracks");
  if (tracks) {
    if (!cJSON_IsArray(tracks)) {
      return false;
    }

    int tracks_array_size = cJSON_GetArraySize(tracks);
    int tracks_array_mem_size = sizeof(track_t) * tracks_array_size;

    if (group) {
      group->tracks_array_size = tracks_array_size;
      group->tracks = (track_t *)malloc(tracks_array_mem_size);
      if (group->tracks == NULL) {
        return false;
      }

      memset(group->tracks, 0, tracks_array_mem_size);
    }

    for (int i = 0; i < tracks_array_size; i++) {
      cJSON *item = cJSON_GetArrayItem(tracks, i);
      if (!parse_track_object(item, group ? &group->tracks[i] : NULL)) {
        return false;
      }
    }
  }

  cJSON* blinks = cJSON_GetObjectItem(obj, "blinks");
  if (blinks) {
    if (!cJSON_IsArray(blinks)) {
      return false;
    }  

    int blinks_array_size = cJSON_GetArraySize(blinks);
    int blinks_array_mem_size = sizeof(blink_t) * blinks_array_size;
  
    if (group) {
      group->blinks_array_size = blinks_array_size;
      group->blinks = (blink_t*)malloc(blinks_array_mem_size);
      if (group->blinks == NULL) {
        return false;
      }

      memset(group->blinks, 0, blinks_array_mem_size);
    }

    for (int i = 0; i < blinks_array_size; i++) {
      cJSON *item = cJSON_GetArrayItem(blinks, i);
      if (!parse_blink_object(item, group ? &group->blinks[i] : NULL)) {
        return false;
      }
    }
  }

  cJSON *time = cJSON_GetObjectItem(obj, "time");
  if (!time || !cJSON_IsNumber(time) || time->valueint < -1)
    return false;
  
  if (group) {
    group->time = time->valueint;
  }

  return true;
}


