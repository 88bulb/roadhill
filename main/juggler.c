#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

void juggler(void *arg) {
  for (;;) {
    vTaskDelay(60 * 1000 / portTICK_PERIOD_MS);
  }
}
