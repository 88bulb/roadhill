#include "errno.h"
#include "roadhill.h"

/****************************

Disk Format ver 1

first, calculate metadata table size:

4096 buckets in total, each bucket has 1024 bytes, thus 4MB for metadata table.

each record has 64 byte, thus each bucket has 1024 / 64 = 16 records. So if all
buckets are full, there are 4096 * 16 = 64KB records. For a 128GB mmc, if all
file sizes are equal, then each file has roughly 2MB. In reality, most mp3 file
will be several metabytes and pcm files will be tens of metabytes. So the bucket
number and record in buckets are enough, as estimated.

Layout:

|--------|--------|=-=-=-=-=-=-=||
|        |xxxxxxxx|             ||
         4M       8M            64M

@1024       superblock, 512 bytes. Once created, never re-written.
@512KB      header, 1024 bytes, with last 16 bytes as md5
@1MB-2KB    writelog, 2048 bytes, dual bucket
@4MB        4 megabytes, include 4096 buckets, each bucket has
            16 records, each record has 64 bytes.
@64MB       data block starts

metadata table @4MB
header @0B and @512KB

write log is dual buckets, 2 * 1024 in size. @1M

header describes:

1. magic
2. version (single uint32_t)
3. secondary header location (no, last 2 sectors)
4. buckets:
    1. size
    2. number of buckets
    3. where starts
5. writelog offset (twice the size of a bucket)
6. data
    1. starts
    2. block size

*****************************/

#define MMCFS_MAX_BITARRAY_SIZE (16 * 1024 * 8)

typedef struct mmcfs {
    uint64_t log_start;
    uint64_t log_sect; // twice bucket_sect

    /* starting sector of bucket */
    uint64_t bucket_start;
    
    /* how much sectors for a bucket */
    uint64_t bucket_sect;

    /* total buckets number, in current implementation, 4096 */
    uint64_t bucket_count;
    uint64_t block_start;
    uint64_t block_sect;
    uint64_t block_count;
} mmcfs_t;

// superblock is located at sector 2 (aka, 1024 bytes)
typedef struct __attribute__((packed)) {
    uint8_t magic[16];
    uint64_t version;
    mmcfs_t mmcfs;
    uint8_t zero_padding[512 - 16 * 2 - sizeof(uint64_t) - sizeof(mmcfs_t)];
    uint8_t md5[16];
} mmcfs_superblock_t;

_Static_assert(sizeof(mmcfs_superblock_t) == 512,
               "mmc_superblock_t size incorrect");

/**
 * 4G   -> 32KiB    * 64k = 2GB
 * 8G   -> 64KiB    * 64K = 4GB
 * 16G  -> 128KiB   * 64K = 8GB
 * 32G  -> 256KiB   * 64K = 16GB
 * 64G  -> 512KiB
 * 128G -> 1MiB
 * 256G -> 2MiB
 * 512G -> 4MiB
 * 1T   -> 8MiB
 * 2T   -> 16MiB
 * 4T   -> 32MiB
 * 8T   -> 64MiB    * 64K = 4TB
 */

typedef enum __attribute__((packed)) {
    MMCFS_FILE_UNUSED = 0,
    MMCFS_FILE_MP3 = 1,
    MMCFS_FILE_PCM = 2,
    MMCFS_FILE_MAX = 0xff
} mmcfs_file_type_t;

_Static_assert(sizeof(mmcfs_file_type_t) == 1,
               "mmcfs_file_type_t incorrect size");

/*
 * mp3 and pcm subtype are defined in the same space
 */
typedef enum __attribute__((packed)) {
    MMCFS_MP3_SUBTYPE_NONE = 0, 
    MMCFS_PCM_48K_16B_STEREO_OOB_NONE = 0,
    MMCFS_FILE_SUBTYPE_MAX = 0xff
} mmcfs_file_subtype_t;

_Static_assert(sizeof(mmcfs_file_subtype_t) == 1,
               "mmcfs_file_subtype_t incorrect size");


/**
 * There may be three TYPEs of file.
 * 1. original mp3 file.
 *      with or without link
 *      data type: stream
 *      data_size
 *      block_start
 * 2. original mp3 file, pure link, no data (not supported now)
 *      must have a link
 8      no data
 * 3. pcm file, with oob data
 *      must have a link
 *      data_size
 *      data type: packet
 *      packet_size: 16K
 *      packet_data: 15K
 *      packet_position: first or last
 */
typedef struct __attribute__((packed)) {
    md5_digest_t self;

    // for mp3 file, this links to pcm;
    // for pcm file, this links to original mp3 file.
    // there is no need to have an "option field" denoting
    // whether this link is used or not, since a "danling"
    // reference is allowed.
    md5_digest_t link;

    // access is a global monotonously increasing number
    // it is roughly equivalent to last access time, the larger, the latter
    uint32_t access;

    // starting block, relative to fs->block_start, in block unit (not sector)
    // start is inclusive and end is exclusive
    uint32_t block_start;
    uint32_t block_end;

    // in byte
    // arbitrary non-zero value for mp3
    // must be multiple of (pcm + oob) * 512 for pcm
    uint32_t size;

    //
    mmcfs_file_type_t type;     // 0 for unused, 1 for mp3, 2 for pcm
    mmcfs_file_subtype_t subtype;  // 

    // for mp3, both are zero
    // for pcm, pcm is modelled as a packet stream, not a
    uint8_t pcm_sect;
    uint8_t oob_sect;

    uint32_t zero[3];
} mmcfs_file_t;

_Static_assert(sizeof(mmcfs_file_t) == 64, "mmc_file_t size incorrect");

typedef struct {
    mmcfs_file_t files[16];
} mmcfs_bucket_t;

_Static_assert(sizeof(mmcfs_bucket_t) == 1024, "mmc_bucket_t size incorrect");


typedef struct mmcfs_file_context mmcfs_file_context_t;
typedef mmcfs_file_context_t* mmcfs_file_handle_t;
mmcfs_file_handle_t mmcfs_create_file(md5_digest_t *digest, uint32_t mp3_size);
int mmcfs_write_mp3(mmcfs_file_handle_t file, char* buf, size_t len);
int mmcfs_write_pcm(mmcfs_file_handle_t file, char* buf, size_t len);
int mmcfs_commit_file(mmcfs_file_handle_t file);

typedef struct {
    int mp3_state;  // 0, none (maybe link only), 1, partial, 2, full
    int pcm_state;  // 0, none, 1, partial, 2, full
    int pcm_format;  
    int fft_format;    
} mmcfs_finfo_t;

int mmcfs_stat(const md5_digest_t* digest, mmcfs_finfo_t* finfo);

void mmcfs_pcm_mix(const md5_digest_t *digest1, int pos1,
                   const md5_digest_t *digest2, int pos2, char buf[8192]);

