#ifndef APPLICATION_ADPCM_STREAM_H
#define APPLICATION_ADPCM_STREAM_H

#include <stddef.h>
#include "tools.h"

typedef struct adpcm_stream_context {
  char* base_url;
  char* url;
  size_t size;
  md5_digest_t md5;
   
} adpcm_stream_context_t;

void create_adpcm_stream(adpcm_stream_context_t* ctx);

#endif
