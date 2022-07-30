#ifndef UNITTEST_MALLOC_H
#define UNITTEST_MALLOC_H

#include <stdbool.h>
#include <stddef.h>

void* tmalloc(size_t size);
void tfree(void *p);

bool tmalloc_no_leak(void);

extern bool log_malloc;

#endif

