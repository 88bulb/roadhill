#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// #include "freertos/queue.h"

#include "esp_log.h"
#include "esp_http_client.h"

#include "message.h"
#include "tools.h"
#include "wget.h"

/*
 * see esp_http_client_example.c https_async()
 */

static const char *TAG = "wget";

#define DATA_BLOCK_SIZE 4096

static void wget(void *arg);

void create_wget_task(void) {
  xTaskCreate(wget, "wget", 8192, NULL, 8, NULL);
}

/**
 * fetcher downloads track data and send them back to juggler.
 * fetcher reads mem_block_t object out of context.
 */
static void wget(void *arg) {
  msg_t msg;

  esp_http_client_handle_t client = NULL;
  char *base_url;
  md5_digest_t *md5;
  char *url = NULL;
  char *data = NULL;
  size_t size;
  esp_err_t err;
  int content_length, total_read_len, read_len;

  ESP_LOGI(TAG, "wget started, idle");

enter_idle_state: // instead of while (true) loop

  if (!recv_msg(WGET_QUEUE, &msg, true) || msg.type != WGET) {
    goto enter_idle_state;
  } else {
    goto enter_working_state;
  }

enter_working_state:

  ESP_LOGI(TAG, "msg.type = %d, msg.data = %p", msg.type, msg.data);

  total_read_len = 0;

  base_url = ((wget_t *)msg.data)->base_url;
  size = ((wget_t *)msg.data)->size;
  md5 = ((wget_t *)msg.data)->md5;
  free(msg.data);

  ESP_LOGI(TAG, "base_url %p, size %d, md5 %p", base_url, size, md5);

  url = make_url(base_url, md5);
  if (!url) {
    err = ESP_ERR_NO_MEM;
    goto exit_working_state;
  }

  ESP_LOGI(TAG, "file url: %s", url);

  esp_http_client_config_t config = {.url = url};
  client = esp_http_client_init(&config);
  err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGI(TAG, "failed to open http client (%d, %s)", err,
             esp_err_to_name(err));
    goto exit_working_state;
  }

  // ULLONG_MAX in limits.h
  content_length = esp_http_client_fetch_headers(client);
  ESP_LOGI(TAG, "content length: %d", content_length);

/*
  if (recv_msg(WGET_QUEUE, &msg, false) && msg.type == WGET_CANCEL) {
    err = ESP_ERR_NOT_FINISHED;
    goto exit_working_state;
  }
*/

  if (content_length == -1) {
    ESP_LOGI(TAG, "server responds no content-length.");
    content_length = size;
  }

  if (content_length != size) {
    ESP_LOGI(TAG, "content-length mismatch, expected: %d, actual: %d", size,
             content_length);
    err = ESP_ERR_INVALID_SIZE;
    goto exit_working_state;
  }

  for (;;) {
    // data is either sent to main or recycled in each loop
    data = (char *)malloc(DATA_BLOCK_SIZE);
    if (data == NULL) {
      err = ESP_ERR_NO_MEM;
      goto exit_working_state;
    }

    read_len = esp_http_client_read(client, data, DATA_BLOCK_SIZE);
    if (recv_msg(WGET_QUEUE, &msg, false) && msg.type == WGET_CANCEL) {
      err = ESP_ERR_NOT_FINISHED;
      goto exit_working_state;
    }

    // ESP_LOGI(TAG, "read_len %d", read_len);

    if (read_len > 0) {
      total_read_len += read_len;
      if (total_read_len > content_length) { // ERROR
        ESP_LOGI(TAG, "received more data than expected");
        err = ESP_ERR_INVALID_SIZE;
      } else {
        chunk_t *chunk = (chunk_t *)malloc(sizeof(chunk_t));
        if (!chunk) {
          err = ESP_ERR_NO_MEM;
          goto exit_working_state;
        }

        chunk->len = read_len;
        chunk->data = data;
        msg.type = WGET_DATA;
        msg.data = chunk;
        if (!send_msg(MAIN_QUEUE, &msg)) {
          ESP_LOGI(TAG, "error, failed to send WGET_DATA");
        }
        continue;
      }
    } else if (read_len == 0) {
      ESP_LOGI(TAG, "errno %d", errno);

      if (total_read_len == content_length) { // FINISH
        ESP_LOGI(TAG, "finished with correct recv size");
        err = ESP_OK;
      } else { // ERROR
        if (total_read_len < content_length) {
          ESP_LOGI(TAG, "finished undersize");
        } else {
          ESP_LOGI(TAG, "finished oversize");
        }
        err = ESP_ERR_INVALID_SIZE;
      }
    } else { // ERROR
      ESP_LOGI(TAG, "read error, ret: %d, errno: %d", read_len, errno);
      err = ESP_FAIL;
    }
    break;
  }

exit_working_state:
  if (client) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    client = NULL;
  }

  // free(base_url);
  // free(md5);

  if (url) {
    free(url);
    url = NULL;
  }

  if (data) {
    free(data);
    data = NULL;
  }

  result_t *res = (result_t *)malloc_until(sizeof(result_t));
  res->result = err;
  msg.type = WGET_DONE;
  msg.data = res;
  send_msg(MAIN_QUEUE, &msg);

  goto enter_idle_state;
}


