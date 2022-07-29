#ifndef APPLICATION_TOOLS_H
#define APPLICATION_TOOLS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef struct md5_digest {
  uint8_t bytes[16];
} md5_digest_t;

bool hex_string_to_bytes(const char *hex_sr, uint8_t *bytes, int len);
void* malloc_until(size_t size);
char *make_url(const char *base_url, const md5_digest_t* md5);
void sprint_md5_digest(const md5_digest_t *digest, char *buf, int trunc);
md5_digest_t *make_md5_digest(const char *hex_str);

#endif
