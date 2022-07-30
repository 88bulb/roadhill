#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>

#include <sys/queue.h>

#include "esp_log.h"
#include "tmalloc.h"

static const char *TAG = "TMALLOC";

struct tailq_entry {
  void* p;
  size_t size;
  TAILQ_ENTRY(tailq_entry) entries;
};

TAILQ_HEAD(, tailq_entry) tailhead = TAILQ_HEAD_INITIALIZER(tailhead);
bool log_malloc = false;

void *tmalloc(size_t size) {
  struct tailq_entry *entry = malloc(sizeof(struct tailq_entry));
  entry->p = malloc(size);
  assert(entry->p);
  entry->size = size;
  TAILQ_INSERT_TAIL(&tailhead, entry, entries);
  if (log_malloc) {
    ESP_LOGI(TAG, "tmalloc %d, %p", size, entry->p);
  }
  return entry->p;
}

void tfree(void *p) {
  struct tailq_entry *entry = TAILQ_FIRST(&tailhead);
  while (entry != NULL) {
    if (entry->p == p) {
      TAILQ_REMOVE(&tailhead, entry, entries);
      free(entry);
      free(p);
      if (log_malloc) {
        ESP_LOGI(TAG, "tfree %p", p);
      }
      return;
    }
  }
  ESP_LOGE(TAG, "freeing %p which is not allocated with tmalloc", p);
  assert(false);
}

bool tmalloc_no_leak() { return TAILQ_FIRST(&tailhead) == NULL; }
