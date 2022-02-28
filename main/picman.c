#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_http_client.h"

#include "roadhill.h"

static const char *TAG = "picman";

/**
 * fetcher downloads track data and send them back to juggler.
 * fetcher reads mem_block_t object out of context.
 */
void picman(void *arg) {
    esp_err_t err;
    picman_context_t *ctx = arg;
    picman_inmsg_handle_t cmd = NULL;
    picman_outmsg_t rep;
    char *data = NULL;
    int content_length, total_read_len, read_len;

    ESP_LOGI(TAG, "picman started");

forever: // instead of while (true) loop
    rep.data = NULL;
    rep.size_or_error = ESP_FAIL;
    content_length = 0;
    total_read_len = 0;
    read_len = -1;

    if (pdTRUE != xQueueReceive(ctx->in, &cmd, portMAX_DELAY)) {
        goto forever;
    }

    esp_http_client_config_t config = {.url = cmd->url};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "failed to open http client (%d, %s)", err,
                 esp_err_to_name(err));
        rep.size_or_error = err;
        goto end;
    }

    // ULLONG_MAX in limits.h
    content_length = esp_http_client_fetch_headers(client);
    if (content_length == -1) {
        ESP_LOGI(TAG, "server responds no content-length.");
        content_length = cmd->track_size;
    }

    if (content_length != cmd->track_size) {
        ESP_LOGI(TAG, "content-length mismatch, expected: %d, actual: %d",
                 cmd->track_size, content_length);
        goto end;
    }

http_read_loop:
    rep.data = NULL;
    rep.size_or_error = ESP_FAIL;

    // data is either sent to juggler or recycled in each loop
    data = (char *)malloc(PIC_BLOCK_SIZE);
    if (data == NULL) {
        rep.size_or_error = ESP_ERR_NO_MEM;
        goto end;
    }

    read_len = esp_http_client_read(client, data, PIC_BLOCK_SIZE);
    if (read_len > 0) {
        total_read_len += read_len;
        if (total_read_len > content_length) { // ERROR
            ESP_LOGI(TAG, "received more data than expected");
        } else {
            rep.data = data;
            rep.size_or_error = read_len;
            xQueueSend(ctx->out, &rep, portMAX_DELAY);
            goto http_read_loop;
        }
    } else if (read_len == 0) {
        if (total_read_len == content_length) { // FINISH
            ESP_LOGI(TAG, "finished with correct recv size");
            rep.size_or_error = ESP_OK;
        } else { // ERROR
            if (total_read_len < content_length) {
                ESP_LOGI(TAG, "finished undersize");
            } else {
                ESP_LOGI(TAG, "finished oversize");
            }
        }
    } else { // ERROR
        ESP_LOGI(TAG, "read error, ret: %d, errno: %d", read_len, errno);
    }

end:
    if (data) {
        free(data);
    }
    free(cmd);
    xQueueSend(ctx->out, &rep, portMAX_DELAY);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    goto forever;
}
