#include <string.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_md5.h"

#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_host.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "vfs_fat_internal.h"
#include "esp_vfs_fat.h"

#include "roadhill.h"
#include "mmcfs.h"

static const char *TAG = "mmcfs";

#define SUPERBLOCK_SECTOR (2)

typedef struct mmcfs_range {
    uint64_t begin;
    uint64_t end;
} mmcfs_range_t;

/** echo "morning my cat" | md5sum */
const char mmcfs_superblock_magic[16] = {0xf2, 0xf4, 0x82, 0x9f, 0x63, 0x69,
                                         0x5a, 0xd7, 0xc4, 0x9f, 0xcf, 0xe5,
                                         0x28, 0x4c, 0xc5, 0x8c};

/**
 * this may be changed to pointer in future TODO
 */
static sdmmc_card_t *card = NULL;
static mmcfs_superblock_t *superblock = NULL;
static mmcfs_t *fs = NULL;

static uint64_t last_access = 0;

static uint8_t bit_array[16 * 1024] __attribute__((aligned(4))) = {0};
static uint8_t bit_count = 0;

/* The ONLY read/write buf for mmc io, which means data must be
 * copied into this buffer before doing any write operation.
 * Explicit 'preload' may be required for write operation if the caller
 * want to reuse the data as soon as possible.
 * For read operation, the caller should copy the data into another buffer
 * if it is possible to trigger further mmc read/write operation during
 * data processing. Otherwise, the data could be proessed in situ to avoid
 * memory duplication.
 */
static uint8_t iobuf[16 * 1024] __attribute__((aligned(8))) = {0};

static bool mmcfs_file_is_null(const mmcfs_file_t *file) {
    return file->type == 0;
}
static bool mmcfs_file_is_mp3(const mmcfs_file_t *file) {
    return file->type == 1;
}
static bool mmcfs_file_is_pcm(const mmcfs_file_t *file) {
    return file->type == 2;
}
static bool mmcfs_file_digest_match(const mmcfs_file_t *file,
                                    const md5_digest_t *digest) {
    return memcmp(file->self.bytes, digest->bytes, sizeof(md5_digest_t)) == 0;
}

static uint16_t mmcfs_bucket_start_sector(const md5_digest_t *digest) {
    uint16_t index = ((uint16_t)digest->bytes[0]) << 4;
    index += digest->bytes[1] >> 4;
    return fs->bucket_start + index * fs->bucket_sect;
}

static size_t mmcfs_bucket_sector_count() { return fs->bucket_sect; }

static int mmcfs_bucket_find_file(const mmcfs_bucket_t *bucket,
                                  const md5_digest_t *digest) {
    for (int i = 0; i < sizeof(bucket->files) / sizeof(bucket->files[0]); i++) {
        if (mmcfs_file_is_null(&bucket->files[i])) {
            return -ENOENT;
        }

        if (mmcfs_file_digest_match(&bucket->files[i], digest)) {
            return i;
        }
    }
    return -ENOENT;
}

/* These three functions are UNSAFE. Callers must check range for themselves.
 */
bool test_bit(int i) {
    return !!(bit_array[i / 8] & ((uint8_t)1 << (i % 8)));
}

void set_bit(int i) { bit_array[i / 8] |= (1 << (i % 8)); }

void clear_bit(int i) { bit_array[i / 8] &= ~(1 << (i % 8)); }

/* Set all bits from start to end (exclusive).
 *
 * The function returns
 * - ESP_ERR_INVALID_ARG, if start > end or end > bit_count
 * - ESP_ERR_INVALID_STATE, if
 */
esp_err_t set_bits(uint32_t start, uint32_t end, uint32_t *conflict) {
    if (start > end || end > bit_count) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t i = start; i < end; i++) {
        if (test_bit(i)) {
            *conflict = i;
            return ESP_ERR_INVALID_STATE;
        }
    }

    for (uint32_t i = start; i < end; i++) {
        set_bit(i);
    }

    return ESP_OK;
}

/* Clear all bits from start to end (exclusive)
 * TODO optimize performance
 */
esp_err_t clear_bits(uint32_t start, uint32_t end, uint32_t *conflict) {
    if (start > end || end > bit_count) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t i = start; i < end; i++) {
        if (!test_bit(i)) {
            *conflict = i;
            return ESP_ERR_INVALID_STATE;
        }
    }

    for (uint32_t i = start; i < end; i++) {
        clear_bit(i);
    }

    return ESP_OK;
}

/*
 * for mp3 and pcm file
 * create_mp3 (md5 & size)
 * create_pcm (with mp3 md5, only estimated size)
 *
 * write_mp3
 * write_pcm
 *
 * read mp3
 * read pcm by mp3_md5, sect
 * read pcm by pcm_md5, sect
 */

/* round up to next power of 2
 */
uint64_t round_power2(uint64_t n) {
    n = n - 1;
    while (n & (n - 1)) {
        n = n & (n - 1);
    }
    return n << 1;
}

/*
 *
 */
static void sdmmc_card_info(const sdmmc_card_t *card) {
    bool print_scr = false;
    bool print_csd = false;
    const char *type;

    ESP_LOGI(TAG, "sdmmc name: %s", card->cid.name);
    if (card->is_sdio) {
        type = "SDIO";
        print_scr = true;
        print_csd = true;
    } else if (card->is_mmc) {
        type = "MMC";
        print_csd = true;
    } else {
        type = (card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC";
    }

    ESP_LOGI(TAG, "sdmmc type: %s", type);

    if (card->max_freq_khz < 1000) {
        ESP_LOGI(TAG, "sdmmc speed: %d kHz", card->max_freq_khz);
    } else {
        ESP_LOGI(TAG, "sdmmc speed: %d MHz%s", card->max_freq_khz / 1000,
                 card->is_ddr ? ", DDR" : "");
    }

    ESP_LOGI(TAG, "sdmmc size: %lluMB, sector size: %d",
             ((uint64_t)card->csd.capacity) * card->csd.sector_size /
                 (1024 * 1024),
             card->csd.sector_size);

    if (print_csd) {
        ESP_LOGI(
            TAG,
            "sdmmc csd: ver=%d, sector_size=%d, capacity=%d read_bl_len=%d",
            card->csd.csd_ver, card->csd.sector_size, card->csd.capacity,
            card->csd.read_block_len);
    }

    if (print_scr) {
        ESP_LOGI(TAG, "sdmmc scr: sd_spec=%d, bus_width=%d", card->scr.sd_spec,
                 card->scr.bus_width);
    }
}

bool mmcfs_superblock_valid(mmcfs_superblock_t *superblock) {
    md5_context_t md5_ctx;
    uint8_t digest[16];
    if (memcmp(superblock->magic, mmcfs_superblock_magic,
               sizeof(mmcfs_superblock_magic)) == 0) {
        esp_rom_md5_init(&md5_ctx);
        esp_rom_md5_update(&md5_ctx, superblock, 512 - 16);
        esp_rom_md5_final(digest, &md5_ctx);
        if (memcmp(digest, superblock->md5, sizeof(digest)) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * Initialize mmc card. If successful, (variable) card is available.
 */
static esp_err_t init_mmc() {
    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        return err;
    }

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot_config);
    if (err != ESP_OK) {
        return err;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
    if (card == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        free(card);
        card = NULL;
        return err;
    }

    sdmmc_card_info(card);
    return ESP_OK;
}

/*
 * print mmcfs info
 */
void mmcfs_info(mmcfs_t *fs) {
    const char *TAG = pcTaskGetName(NULL);
    ESP_LOGI(TAG, "log start @ %llu sector (%llu bytes offset)", fs->log_start,
             fs->log_start * 512);
    ESP_LOGI(TAG, "log size %llu sectors (%llu bytes)", fs->log_sect,
             fs->log_sect * 512);

    ESP_LOGI(TAG, "bucket start @ %llu sector (%llu bytes offset)",
             fs->bucket_start, fs->bucket_start * 512);
    ESP_LOGI(TAG, "each bucket has %llu sectors (%llu bytes)", fs->bucket_sect,
             fs->bucket_sect * 512);
    ESP_LOGI(
        TAG,
        "there are %lld buckets in total, occupying %lld sectors (%lldKiB)",
        fs->bucket_count, fs->bucket_count * fs->bucket_sect,
        fs->bucket_count * fs->bucket_sect / 2);

    ESP_LOGI(TAG, "block start @ %llu secotr (%llu bytes or %lluMiB offset)",
             fs->block_start, fs->block_start * 512, fs->block_start / 2048);
    ESP_LOGI(TAG, "each block has %llu sectors (%llu bytes or %lluKiB)",
             fs->block_sect, fs->block_sect * 512, fs->block_sect / 2);
    ESP_LOGI(TAG,
             "there are %lld blocks in total, occupying %lld sectors (%lldMiB)",
             fs->block_count, fs->block_count * fs->block_sect,
             fs->block_count * fs->block_sect / 2048);
}

/*
 * retrieve superblock. If none, new one created with log and buckets erased.
 */
esp_err_t init_fs() {
    esp_err_t err;
    md5_context_t md5_ctx;

    superblock = (mmcfs_superblock_t *)malloc(512);
    if (superblock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = sdmmc_read_sectors(card, superblock, SUPERBLOCK_SECTOR, 1);
    if (err != ESP_OK) {
        free(superblock);
        superblock = NULL;
        return err;
    }

    if (mmcfs_superblock_valid(superblock)) {
        fs = &superblock->mmcfs;
        return ESP_OK;
    }

    memset(superblock, 0, sizeof(mmcfs_superblock_t));
    memcpy(superblock->magic, mmcfs_superblock_magic,
           sizeof(mmcfs_superblock_magic));

    uint64_t bucket_sect = 1024 / 512;
    uint64_t log_start = 1024 * 1024 / 512 - 2 * bucket_sect;
    uint64_t log_sect = 2 * bucket_sect;
    uint64_t bucket_start = 4 * 1024 * 1024 / 512;
    uint64_t bucket_count = 4096;
    uint64_t block_start = 64 * 1024 * 1024 / 512;
    uint64_t avail_sect_for_blocks = card->csd.capacity - block_start;
    uint64_t block_sect =
        round_power2(avail_sect_for_blocks) / MMCFS_MAX_BITARRAY_SIZE;

    /** reserve the last sector */
    uint64_t block_count = (avail_sect_for_blocks - 1) / block_sect;

    superblock->version = 0;
    superblock->mmcfs.log_start = log_start;
    superblock->mmcfs.log_sect = log_sect;
    superblock->mmcfs.bucket_start = bucket_start;
    superblock->mmcfs.bucket_sect = bucket_sect;
    superblock->mmcfs.bucket_count = bucket_count;
    superblock->mmcfs.block_start = block_start;
    superblock->mmcfs.block_sect = block_sect;
    superblock->mmcfs.block_count = block_count;

    uint8_t *bucket =
        (uint8_t *)heap_caps_malloc(bucket_sect * 512, MALLOC_CAP_DMA);
    memset(bucket, 0, bucket_sect * 512);

    // erasing log
    for (int i = 0; i < log_sect / bucket_sect; i++) {
        err = sdmmc_write_sectors(card, bucket, log_start + i * bucket_sect,
                                  bucket_sect);
        if (err != ESP_OK) {
            free(bucket);
            free(superblock);
            superblock = NULL;
            return err;
        }
    }

    // erase buckets (metadata table)
    for (uint64_t i = 0; i < bucket_count; i++) {
        err = sdmmc_write_sectors(card, bucket, bucket_start + i * bucket_sect,
                                  bucket_sect);
        if (err != ESP_OK) {
            free(bucket);
            free(superblock);
            superblock = NULL;
            return err;
        }
    }

    free(bucket);

    esp_rom_md5_init(&md5_ctx);
    esp_rom_md5_update(&md5_ctx, superblock, sizeof(mmcfs_superblock_t) - 16);
    esp_rom_md5_final(superblock->md5, &md5_ctx);

    assert(mmcfs_superblock_valid(superblock));

    // write superblock
    err = sdmmc_write_sectors(card, superblock, SUPERBLOCK_SECTOR, 1);
    if (err) {
        free(superblock);
        superblock = NULL;
        return err;
    }

    fs = &superblock->mmcfs;
    bit_count = fs->block_count;

    mmcfs_info(fs);
    return ESP_OK;
}

/*
 * read data into iobuf, as well as outbuf if provided.
 */
esp_err_t mmcfs_read_buf_buckets(int buf_index, mmcfs_bucket_t *outbuf) {
    const int buf_sect = sizeof(iobuf) / 512;
    esp_err_t err = sdmmc_read_sectors(
        card, iobuf, fs->bucket_start + buf_index * buf_sect, buf_sect);
    if (err) {
        return err;
    }

    if (outbuf) {
        memcpy(outbuf, iobuf, sizeof(iobuf));
    }

    return ESP_OK;
}

/*
 * init file allocation bit map (bit_array)
 */
static esp_err_t init_falloc_bitmap() {
    esp_err_t err;

    int mp3_count = 0;
    int mp3_block_count = 0;
    int pcm_count = 0;
    int pcm_block_count = 0;

    mmcfs_bucket_t *outbuf = (mmcfs_bucket_t *)malloc(sizeof(iobuf));
    if (outbuf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    int buf_sect = sizeof(iobuf) / 512;
    int bucket_per_buf = buf_sect / fs->bucket_sect;
    int buf_count = fs->bucket_count / bucket_per_buf;

    for (int i = 0; i < buf_count; i++) {
        err = mmcfs_read_buf_buckets(i, outbuf);
        if (err) {
            free(outbuf);
            return err;
        }

        // iterate bucket
        for (int j = 0; j < bucket_per_buf; j++) {
            mmcfs_bucket_t *bucket = &outbuf[j];
            for (int k = 0; k < sizeof(mmcfs_bucket_t) / sizeof(mmcfs_file_t);
                 k++) {

                mmcfs_file_t *f = &bucket->files[k];
                if (f->type == 0)
                    break;

                uint32_t conflict;
                err = set_bits(f->block_start, f->block_end, &conflict);
                // TODO handle err

                if (last_access < f->access) {
                    last_access = f->access;
                }

                if (f->type == 1) {
                    mp3_count++;
                    mp3_block_count += f->block_end - f->block_start;
                }

                if (f->type == 2) {
                    pcm_count++;
                    pcm_block_count += f->block_end - f->block_start;
                }
            }
        }
    }

    free(outbuf);

    last_access++;

    ESP_LOGI(TAG, "%d mp3 files found, %d blocks used.", mp3_count,
             mp3_block_count);
    ESP_LOGI(TAG, "%d pcm files found, %d blocks used.", pcm_count,
             pcm_block_count);

    return ESP_OK;
}

int size_in_block(uint64_t size) {
    int block_size = fs->block_sect * 512;
    return (size % block_size == 0) ? size / block_size : size / block_size + 1;
}

/*
 * return starting block index, zero-based, or -1 (0xffffffff).
 */
uint32_t allocate_blocks(uint32_t blocks) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < 16 * 1024; i++) {
        if (test_bit(i)) {
            count = 0;
        } else {
            count++;
        }

        if (count == blocks) {
            uint32_t start = i - blocks + 1;
            for (uint32_t i = start; i < start + blocks; i++) {
                set_bit(i);
            }
            return start;
        }
    } 
    return -1;
}

/*
 * all-in-one function to do them all
 */
esp_err_t init_mmcfs() {
    esp_err_t err;

    err = init_mmc();
    if (err != ESP_OK)
        return err;

    err = init_fs();
    if (err != ESP_OK)
        return err;

    return init_falloc_bitmap();
}

int mmcfs_stat(const md5_digest_t *digest, mmcfs_finfo_t *finfo) {
    esp_err_t err;

    err = sdmmc_read_sectors(card, iobuf, mmcfs_bucket_start_sector(digest),
                             mmcfs_bucket_sector_count());
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "mmcfs_stat failed, %s", esp_err_to_name(err));
        return -EIO;
    }

    int index = mmcfs_bucket_find_file((mmcfs_bucket_t *)iobuf, digest);
    if (index < 0) {
        return -ENOENT;
    }

    mmcfs_file_t *mp3_file = (mmcfs_file_t *)malloc(sizeof(mmcfs_file_t));
    if (mp3_file == NULL) {
        return -ENOMEM;
    }

    memcpy(mp3_file, &((mmcfs_bucket_t *)iobuf)->files[index],
           sizeof(mmcfs_file_t));

    err = sdmmc_read_sectors(card, iobuf,
                             mmcfs_bucket_start_sector(&mp3_file->link),
                             mmcfs_bucket_sector_count());
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "mmcfs_stat failed, %s", esp_err_to_name(err));
        free(mp3_file);
        return -EIO;
    }

    index = mmcfs_bucket_find_file((mmcfs_bucket_t *)iobuf, &mp3_file->link);
    if (finfo) {
        memset(finfo, 0, sizeof(mmcfs_finfo_t));
        finfo->mp3_state = 2;
        finfo->pcm_state = index < 0 ? 0 : 2;
    }

    free(mp3_file);
    return 0;
}

struct mmcfs_file_context {
    md5_digest_t digest;
    uint64_t size;
    uint32_t mp3_start;
    uint32_t mp3_blocks;
    uint32_t pcm_start;
    uint32_t pcm_blocks;
};

static mmcfs_file_context_t *wctx = NULL;

/*
 * minimal 128kbps (16KiB/s)
 */
mmcfs_file_handle_t mmcfs_create_file(md5_digest_t *digest, uint64_t size) {
    int mp3_blocks = size_in_block(size);
    int est_pcm_size = size / (16 * 1024) * 48000 * 4;
    int est_pcm_blocks = size_in_block(est_pcm_size);
    int total_blocks = mp3_blocks + est_pcm_blocks;

    uint32_t start = allocate_blocks(total_blocks);
    if (start == -1)
        return NULL;

    wctx = (mmcfs_file_context_t *)malloc(sizeof(mmcfs_file_context_t));
    if (wctx) {
        memcpy(&wctx->digest, digest, sizeof(wctx->digest));
        wctx->size = size;
        wctx->mp3_start = start;
        wctx->mp3_blocks = mp3_blocks;
        wctx->pcm_start = start + mp3_blocks;
        wctx->pcm_blocks = est_pcm_blocks;
    }

    return wctx;
}

int mmcfs_write_mp3(mmcfs_file_handle_t file, char* buf, size_t len) {
    return 0;
}

int mmcfs_write_pcm(mmcfs_file_handle_t file, char* buf, size_t len) {
    return 0;
}

int mmcfs_write_end(mmcfs_file_handle_t file) {
    return 0;
}
