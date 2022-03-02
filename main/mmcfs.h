#include "errno.h"
#include "roadhill.h"

typedef struct {
    int mp3_state;  // 0, none (maybe link only), 1, partial, 2, full
    int pcm_state;  // 0, none, 1, partial, 2, full
    int pcm_format;  
    int fft_format;    
} mmcfs_finfo_t;

int mmcfs_stat(const md5_digest_t* digest, mmcfs_finfo_t* finfo);

