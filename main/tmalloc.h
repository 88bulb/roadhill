#ifndef TEST_TMALLOC_H
#define TEST_TMALLOC_H

#include <stddef.h>

extern void *tmalloc(size_t size);
extern void *tfree(void* p);

#endif

