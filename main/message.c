#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "message.h"

static QueueHandle_t main_queue;
static QueueHandle_t ota_queue;
static QueueHandle_t tcp_queue;
static QueueHandle_t wget_queue;
static QueueHandle_t trans_queue;
static QueueHandle_t play_queue;

void create_msg_queues() {
  main_queue = xQueueCreate(16, sizeof(msg_t));
  ota_queue = xQueueCreate(2, sizeof(msg_t));
  tcp_queue = xQueueCreate(4, sizeof(msg_t));
  wget_queue = xQueueCreate(2, sizeof(msg_t));
  trans_queue = xQueueCreate(2, sizeof(msg_t));
  play_queue = xQueueCreate(4, sizeof(msg_t));
}

static QueueHandle_t get_queue_handle(queue_id_t queue) {
  switch (queue) {
  case MAIN_QUEUE:
    return main_queue;
  case OTA_QUEUE:
    return ota_queue;
  case TCP_QUEUE:
    return tcp_queue;
  case WGET_QUEUE:
    return wget_queue;
  case TRANS_QUEUE:
    return trans_queue;
  case PLAY_QUEUE:
    return play_queue;
  }
  return NULL;
}

bool send_msg(queue_id_t queue, msg_t *msg) {
  QueueHandle_t handle = get_queue_handle(queue);
  return pdTRUE == xQueueSend(handle, msg, (TickType_t)0);
}

bool recv_msg(queue_id_t queue, msg_t *msg, bool blocking) {
  QueueHandle_t handle = get_queue_handle(queue);
  return pdTRUE == xQueueReceive(handle, msg, blocking ? portMAX_DELAY : 0);
}
