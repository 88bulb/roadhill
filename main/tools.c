#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"

#include "tools.h"

/* 32 hex + .mp3 suffix */
#define FILENAME_LEN (32 + 4)

#define INVALID_HEXCHAR         (0xff)

static const char *TAG = "tools";
static const char hex_char[16] = "0123456789abcdef";

/*
 * Convert a hex char to a half byte, i.e., int from 0 to 15;
 * return 0xff if given chr is not a hex char
 */
char hex_char_to_half_byte(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  } else if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  } else if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  } else {
    return 0xff;
  }
}

/*
 * Convert hex string to byte array
 * 
 * @param [in]  hex_str Input string buffer
 * @param [out] buf     Output buffer, if null, dryrun.
 * @param [out] len     Output length
 * @param [in]  maxlen  Max output lengnth 
 * @return true for success
 */
bool hex_string_to_bytes(const char *hex_str, uint8_t *buf, int *len,
                         int maxlen) {
  if (!hex_str)
    return false;

  size_t slen = strnlen(hex_str, maxlen * 2);
  if (slen % 2 != 0)
    return false;

  for (int i = 0; i < slen / 2; i++) {
    uint8_t high = hex_char_to_half_byte(hex_str[i * 2]);
    if (high == INVALID_HEXCHAR)
      return false;
    uint8_t low = hex_char_to_half_byte(hex_str[i * 2 + 1]);
    if (low == INVALID_HEXCHAR)
      return false;

    if (buf) {
      buf[i] = (high << 4) + low;
    }
  }

  if (len) {
    *len = slen / 2;
  }

  return true;
}

md5_digest_t *make_md5_digest(const char *hex_str) {
  md5_digest_t *md5 = (md5_digest_t *)malloc(sizeof(md5_digest_t));
  if (md5 == NULL)
    return NULL;

  for (int i = 0; i < 32; i++) {
    char c = hex_str[i];
    uint8_t v;
    if (c >= '0' && c <= '9') {
      v = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      v = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      v = c - 'A' + 10;
    } else {
      free(md5);
      return NULL;
    }

    if (i % 2 == 0) {
      md5->bytes[i / 2] = (v << 4);
    } else {
      md5->bytes[i / 2] |= v;
    }
  }

  return md5;
}

void sprint_md5_digest(const md5_digest_t *digest, char *buf, int trunc) {
  int i;
  int bound = (trunc <= 0 || trunc > 16) ? 16 : trunc;
  for (i = 0; i < bound; i++) {
    buf[2 * i + 0] = hex_char[digest->bytes[i] / 16];
    buf[2 * i + 1] = hex_char[digest->bytes[i] % 16];
  }
  buf[2 * i] = '\0';
}

char *make_url(const char *base_url, const md5_digest_t *digest) {

  char filename[FILENAME_LEN + 1];
  sprint_md5_digest(digest, filename, 0);
  strcat(filename, ".mp3");

  int len = strlen(base_url) + strlen(filename);

  char *url = (char *)malloc(len + 2); // don't forget the slash
  if (url == NULL)
    return NULL;

  memset(url, 0, len + 2);
  strcpy(url, base_url);
  strcat(url, "/");
  strcat(url, filename);
  return url;
}

void *malloc_until(size_t size) {
  void *m = malloc(size);
  while (!m) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
    m = malloc(size);
  }
  return m;
}
