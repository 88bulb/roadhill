#include <string.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_md5.h"

#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "roadhill.h"
#include "mmcfs.h"

static const char *TAG = "mmcfs";

/*
 * to erase superblock on a linux computer. use
 * dd if=/dev/zero of=/dev/mmcblk0 bs=512 count=4
 */
#define SUPERBLOCK_SECTOR (2)

/*
 * note on bucket and write log
 *
 * 1. bucket is fixed size array of file, null-terminated.
 * 2. the first 12 bits of a null file is not zero. it's bucket index.
 *    for example: if the bucket index is 1000, the hex value is 0x03e8, and
 *    shift 12 bits to left we got 0x3e80. So the first byte of a null file
 *    in this bucket is 0x3e, the secod byte is 0x80, and all remaining
 *    bytes are 0x00.
 * 3. write log has two buckets. the former is the bucket being written. the
 *    latter is bitwise NOT-ed.
 */

/*
 * echo "morning my cat" | md5sum
 */
const char mmcfs_superblock_magic[16] = {0xf2, 0xf4, 0x82, 0x9f, 0x63, 0x69,
                                         0x5a, 0xd7, 0xc4, 0x9f, 0xcf, 0xe5,
                                         0x28, 0x4c, 0xc5, 0x8c};

/*
 * pointer to mmc card (driver level)
 */
static sdmmc_card_t *card = NULL;

/*
 * pointer to superblock, but it is not necessary for holding superblock
 * in memory
 */
static mmcfs_superblock_t *superblock = NULL;

/*
 * pointer to file system constants and configurations
 */
static mmcfs_t *fs = NULL;

/*
 *
 */
static uint64_t last_access = 0;


static uint8_t bit_array[16 * 1024] __attribute__((aligned(4))) = {0};

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

/*
 * buffer for fast read/write bucket
 */
static mmcfs_bucket_t bbuf;

static uint32_t mmcfs_block_count() {
    return fs->block_count;
};

/*
 */
/*
void print_file_info(const mmcfs_file_t *file) {
    char self[9] = {0};
    char link[9] = {0};

    sprint_md5_digest(&file->self, self, 4);
    sprint_md5_digest(&file->link, link, 4);

    if (mmcfs_file_is_mp3(file)) {
    }

    if (mmcfs_file_is_pcm(file)) {
    }
} */

/*
 *
 */
static bool mmcfs_file_is_null(const mmcfs_file_t *file) {
    return file->type == MMCFS_FILE_UNUSED;
}

/*
 * returns true if given file is mp3
 */
static bool mmcfs_file_is_mp3(const mmcfs_file_t *file) {
    return file->type == MMCFS_FILE_MP3;
}

/*
 * returns true if given file is pcm
 */
static bool mmcfs_file_is_pcm(const mmcfs_file_t *file) {
    return file->type == MMCFS_FILE_PCM;
}

/*
 * sets file to null
 */
static void mmcfs_file_nullify(mmcfs_file_t *file, const md5_digest_t *digest) {
    memset(file, 0, sizeof(mmcfs_file_t));
    file->self.bytes[0] = digest->bytes[0];
    file->self.bytes[1] = digest->bytes[1] & 0xf0;
}


static bool mmcfs_file_digest_match(const mmcfs_file_t *file,
                                    const md5_digest_t *digest) {
    return memcmp(file->self.bytes, digest->bytes, sizeof(md5_digest_t)) == 0;
}

static uint16_t mmcfs_bucket_index(const md5_digest_t *digest) {
    const uint8_t *bytes = digest->bytes;
    return (((uint16_t)bytes[0]) << 4) + (bytes[1] >> 4);
}

static size_t mmcfs_bucket_start_sector(const md5_digest_t *digest) {
    size_t index = ((size_t)digest->bytes[0]) << 4;
    index += digest->bytes[1] >> 4;
    return fs->bucket_start + index * fs->bucket_sect;
}

static size_t mmcfs_bucket_sector_count() { 
    return fs->bucket_sect; 
}

/*
 * return max files (constant)
 */
static int mmcfs_bucket_max_files() {
    return fs->bucket_sect * 512 / sizeof(mmcfs_file_t);
}

/*
 * return index, or -ENOENT
 */
static int mmcfs_bucket_find_file(const mmcfs_bucket_t *bucket,
                                  const md5_digest_t *digest) {
    for (int i = 0; i < mmcfs_bucket_max_files(); i++) {
        if (mmcfs_file_is_null(&bucket->files[i])) {
            return -ENOENT;
        }

        if (mmcfs_file_digest_match(&bucket->files[i], digest)) {
            return i;
        }
    }
    return -ENOENT;
}

/*
 * read a bucket into iobuf
 */
static int mmcfs_bucket_read(const md5_digest_t *digest,
                             mmcfs_bucket_t *bucket) {
    size_t start_sector = mmcfs_bucket_start_sector(digest);
    size_t sector_count = mmcfs_bucket_sector_count();
    char *buf = bucket ? (char *)bucket : (char *)iobuf;
    esp_err_t err = sdmmc_read_sectors(card, buf, start_sector, sector_count);
    if (err != ESP_OK) {
        return -EIO;
    }
    return 0;
}

/*
 *
 */
static int mmcfs_bucket_update(mmcfs_bucket_t *bucket) {

    if (bucket != NULL && bucket != (mmcfs_bucket_t *)iobuf) {
        memcpy(iobuf, bucket, sizeof(mmcfs_bucket_t));
    }

    mmcfs_bucket_t *log = (mmcfs_bucket_t *)iobuf;
    uint32_t *c0 = (uint32_t *)&log[0];
    uint32_t *c1 = (uint32_t *)&log[1];
    for (int i = 0; i < sizeof(mmcfs_bucket_t) / sizeof(uint32_t); i++) {
        c1[i] = ~c0[i];
    }

    // write log
    esp_err_t err =
        sdmmc_write_sectors(card, iobuf, fs->log_start, fs->log_sect);
    if (err != ESP_OK) {
        return -EIO;
    }

    err = sdmmc_write_sectors(card, iobuf,
                              mmcfs_bucket_start_sector(&log->files[0].self),
                              mmcfs_bucket_sector_count());
    if (err != ESP_OK) {
        return -EIO;
    }

    return 0;
}

/*
 * remove a given file, possibly also remove the linked pcm
 * TODO write log
 */
static int mmcfs_bucket_remove_file(const md5_digest_t *digest,
                                    bool remove_linked_pcm) {
    int ret = mmcfs_bucket_read(digest, NULL);
    if (ret < 0)
        return ret;

    mmcfs_bucket_t *buc = (mmcfs_bucket_t *)iobuf;

    // find returns -ENOENT or index
    int index = mmcfs_bucket_find_file(buc, digest);
    if (index == -ENOENT) {
        return 0;
    } else if (index < 0) {
        return index;
    }

    if (mmcfs_file_is_mp3(&buc->files[index]) && remove_linked_pcm) {
        md5_digest_t link = buc->files[index].link;
        // api seems problematic, should the caller provide file type?
        int r = mmcfs_bucket_remove_file(&link, false);
        if (r < 0)
            return r;

        return mmcfs_bucket_remove_file(digest, false);
    }

    int max_files = mmcfs_bucket_max_files();
    for (int i = index; i < max_files; i++) {
        if (i < max_files - 1) {
            buc->files[i] = buc->files[i + 1];
        } else {
            mmcfs_file_nullify(&buc->files[i], digest);
        }
    }

    return mmcfs_bucket_update(NULL);
}

/* These three functions are UNSAFE. Callers must check range for themselves.
 */
bool test_bit(int i) { return !!(bit_array[i / 8] & ((uint8_t)1 << (i % 8))); }

void set_bit(int i) { 
    // ESP_LOGI(TAG, "set bit %d", i);
    bit_array[i / 8] |= (1 << (i % 8)); 
}

void clear_bit(int i) { bit_array[i / 8] &= ~(1 << (i % 8)); }

/* Set all bits from start to end (exclusive).
 *
 * The function returns
 * - ESP_ERR_INVALID_ARG, if start > end or end > block_count
 * - ESP_ERR_INVALID_STATE, if
 */
esp_err_t set_bits(uint32_t start, uint32_t end, uint32_t *conflict) {
    if (start > end || end > mmcfs_block_count()) {
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
    if (start > end || end > mmcfs_block_count()) {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint32_t i = start; i < end; i++) {
        if (!test_bit(i)) {
            *conflict = i;

            ESP_LOGI(TAG, "clear_bits error, bit %u not set", i);

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
 * print sdmmc card info
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

/*
 * validate superblock
 */
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
    err = sdmmc_write_sectors(card, bucket, log_start, bucket_sect);
    if (err != ESP_OK) {
        free(bucket);
        free(superblock);
        superblock = NULL;
        return err;
    }

    // erasing not log, it is not bitwise NOT-ed, which means the log is invalid
    err =
        sdmmc_write_sectors(card, bucket, log_start + bucket_sect, bucket_sect);
    if (err != ESP_OK) {
        free(bucket);
        free(superblock);
        superblock = NULL;
        return err;
    }

    // erase buckets (metadata table)
    for (uint16_t i = 0; i < bucket_count; i++) {
        bucket[0] = i >> 4;
        bucket[1] = i << 4;
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

    mmcfs_bucket_t *outbuf = (mmcfs_bucket_t *)heap_caps_malloc(
        sizeof(iobuf), MALLOC_CAP_DMA | MALLOC_CAP_32BIT);
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
            // for (int k = 0; k < sizeof(mmcfs_bucket_t) /
            // sizeof(mmcfs_file_t); k++) {
            for (int k = 0; k < mmcfs_bucket_max_files(); k++) {
                mmcfs_file_t *f = &bucket->files[k];
                if (mmcfs_file_is_null(f))
                    break;

                //if (f->type == 0) break;

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

uint32_t mmcfs_block_size() { return fs->block_sect * 512; }

int convert_bytes_to_blocks(uint64_t size) {
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
            ESP_LOGI(TAG, "allocating %u blocks, start from %u", blocks, start);

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

    // TODO apply log if necessary

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

/*
 * check if a bucket is full, if so, remove oldest file
 * if the oldest file is mp3, also remove corresponding pcm
 * if the oldest file is pcm, just remove pcm (without updating corresponding
 * mp3)
 */
int mmcfs_pour_full_bucket(const md5_digest_t *digest) {
    int ret = mmcfs_bucket_read(digest, NULL);
    if (ret < 0) {
        return ret;
    }

    int max_files = mmcfs_bucket_max_files();
    mmcfs_bucket_t *buc = (mmcfs_bucket_t *)iobuf;
    if (mmcfs_file_is_null(&buc->files[max_files - 1]))
        return 0;

    uint32_t access = -1;
    int i = -1;
    for (int j = 0; j < max_files; j++) {
        if (buc->files[j].access < access) {
            i = j;
            access = buc->files[j].access;
        }
    }

    md5_digest_t target = buc->files[i].self;
    return mmcfs_bucket_remove_file(&target, true);
}

/*
 * create a file inside a bucket
 */
int mmcfs_create_file_ll(const md5_digest_t *mp3_digest,
                         const md5_digest_t *pcm_digest, uint32_t block_start,
                         uint32_t block_end, uint32_t size,
                         mmcfs_file_type_t type, mmcfs_file_subtype_t subtype) {

    int ret = mmcfs_pour_full_bucket(mp3_digest);
    if (ret < 0) {
        return ret;
    }

    // read bucket into iobuf
    ret = mmcfs_bucket_read(mp3_digest, NULL);
    if (ret < 0) {
        return ret;
    }

    // shift one place forward
    int max_files = mmcfs_bucket_max_files();
    mmcfs_bucket_t *buc = (mmcfs_bucket_t *)iobuf;
    for (int i = max_files - 1; i > 0; i--) {
        buc[i] = buc[i - 1];
    }

    // now buc->files[0] is empty
    memset(&buc->files[0], 0, sizeof(mmcfs_file_t));
    buc->files[0].self = *mp3_digest;
    buc->files[0].link = *pcm_digest;
    buc->files[0].access = last_access;
    buc->files[0].block_start = block_start;
    buc->files[0].block_end = block_end;
    buc->files[0].size = size;
    buc->files[0].type = type;
    buc->files[0].subtype = subtype;

    ret = mmcfs_bucket_update(NULL);
    if (ret < 0) {
        return ret;
    }

    last_access++;
    return 0;
}

struct mmcfs_file_context {
    bool finalized;

    md5_digest_t digest;
    md5_digest_t calculated_mp3_digest;
    md5_digest_t calculated_pcm_digest;

    uint32_t mp3_size;
    uint32_t mp3_start;
    uint32_t mp3_blocks;
    uint32_t pcm_estimated_size;
    uint32_t pcm_actual_size;
    uint32_t pcm_start;
    uint32_t pcm_estimated_blocks;
    uint32_t pcm_actual_blocks;

    uint32_t mp3_written;
    uint32_t pcm_written;

    md5_context_t mp3_md5_ctx;
    md5_context_t pcm_md5_ctx;
};

/*
 * create_file must starts from wctx = NULL
 * s0 -> s1, allocate a file creation context, allocate bits (blocks)
 * all writing must happen in s1 state, if error, file handle is invalid
 * s1 -> s0 by commit, and wctx recycled, also unused blocks
 */
static mmcfs_file_handle_t _file = NULL;

/*
 * Create a file handle, allocating blocks for writing.
 *
 * minimal 96kbps (12KiB/s)
 */
int mmcfs_create_file(md5_digest_t *digest, uint32_t mp3_size,
                      mmcfs_file_handle_t *out) {

    assert(_file == NULL);

    int ret = mmcfs_bucket_read(digest, &bbuf);  
    if (ret < 0) {
        return ret;
    }

    int index = mmcfs_bucket_find_file(&bbuf, digest);
    if (index >= 0) {
        return -EEXIST;
    } else if (index != -ENOENT) {
        return index;
    }

    int mp3_blocks = convert_bytes_to_blocks(mp3_size);
    int pcm_estimated_size = mp3_size / (12 * 1024) * 48000 * 4;
    int pcm_estimated_blocks = convert_bytes_to_blocks(pcm_estimated_size);

    uint32_t mp3_start = allocate_blocks(mp3_blocks);
    if (mp3_start == -1) {
        return -1; // this is critical error !!! TODO
    }

    uint32_t pcm_start = allocate_blocks(pcm_estimated_blocks);
    if (pcm_start == -1) {
        assert(ESP_OK == clear_bits(mp3_start, mp3_start + mp3_blocks, NULL));
        return -1; // this is also critical error !!! TODO
    }

    mmcfs_file_context_t *file =
        (mmcfs_file_context_t *)malloc(sizeof(mmcfs_file_context_t));

    if (file) {
        file->finalized = false;
        memcpy(&file->digest, digest, sizeof(md5_digest_t));
        file->mp3_size = mp3_size;
        file->mp3_start = mp3_start;
        file->mp3_blocks = mp3_blocks;
        file->pcm_estimated_size = pcm_estimated_size;
        file->pcm_actual_size = 0;
        file->pcm_start = pcm_start;
        file->pcm_estimated_blocks = pcm_estimated_blocks;
        file->pcm_actual_blocks = 0;

        file->mp3_written = 0;
        file->pcm_written = 0;

        esp_rom_md5_init(&file->mp3_md5_ctx);
        esp_rom_md5_init(&file->pcm_md5_ctx);
    }

    _file = file;
    *out = file;
    return 0;
}

void mmcfs_abort_file(mmcfs_file_handle_t file) {
    assert(file == _file);
    assert(ESP_OK == clear_bits(file->mp3_start,
                                file->mp3_start + file->mp3_blocks, NULL));
    assert(ESP_OK == clear_bits(file->pcm_start,
                                file->pcm_start + file->pcm_estimated_blocks,
                                NULL));
    free(file);
    _file = NULL;
}

int mmcfs_write_mp3(mmcfs_file_handle_t file, char *buf, size_t len) {
    assert(file == _file);
    assert(file->finalized == false);

    assert(0 < len && len <= PIC_BLOCK_SIZE);
    memcpy(iobuf, buf, len);

    size_t start_sector = fs->block_start;            // point to first block
    start_sector += file->mp3_start * fs->block_sect; // move to mp3 start
    start_sector += file->mp3_written / 512;
    size_t sector_count = (len + 511) / 512;

    esp_err_t err = sdmmc_write_sectors(card, iobuf, start_sector, sector_count);
    if (err != ESP_OK) {
        mmcfs_abort_file(file);
        return -EIO;
    }

    esp_rom_md5_update(&file->mp3_md5_ctx, iobuf, len);

    file->mp3_written += len;
    // ESP_LOGI(TAG, "mmcfs_write_mp3: %u, %u", len, file->mp3_written);
    return 0;
}

/*
 *
 */
int mmcfs_write_pcm(mmcfs_file_handle_t file, char *buf, size_t len) {
    esp_err_t err;

    assert(file == _file);
    assert(file->finalized == false);

    assert(len == FRAME_BUF_SIZE);
    memcpy(iobuf, buf, len);

    size_t start_sector = fs->block_start;
    start_sector += file->pcm_start * fs->block_sect; // move to pcm start
    start_sector += file->pcm_written / 512;
    size_t sector_count = FRAME_BUF_SIZE / 512;

    err = sdmmc_write_sectors(card, iobuf, start_sector, sector_count);
    if (err != ESP_OK) {
        mmcfs_abort_file(file); 
        return -EIO;
    }

    // only data is included in md5 calculation.
    esp_rom_md5_update(&file->pcm_md5_ctx, iobuf, FRAME_DAT_SIZE);

    file->pcm_written += len;
    // ESP_LOGI(TAG, "mmcfs_write_pcm: %u, %u", len, file->pcm_written);
    return 0;
}

/*
 *
 */
int mmcfs_commit_file(mmcfs_file_handle_t file) {
    assert(file == _file);
    assert(file->finalized == false);

    file->finalized = true;
    file->pcm_actual_size = file->pcm_written;
    file->pcm_actual_blocks = file->pcm_written / mmcfs_block_size();

    esp_rom_md5_final(file->calculated_mp3_digest.bytes, &file->mp3_md5_ctx);
    esp_rom_md5_final(file->calculated_pcm_digest.bytes, &file->pcm_md5_ctx);

    char *p1 = (char *)iobuf;
    sprint_md5_digest(&file->digest, p1, 4);
    char *p2 = p1 + strlen(p1) + 1;
    sprint_md5_digest(&file->calculated_mp3_digest, p2, 4);
    char *p3 = p2 + strlen(p2) + 1;
    sprint_md5_digest(&file->calculated_pcm_digest, p3, 4);
    bool mp3_digest_match = memcmp(&file->digest, &file->calculated_mp3_digest,
                                   sizeof(md5_digest_t)) == 0;
    bool mp3_size_match = (file->mp3_size == file->mp3_written);

    ESP_LOGI(TAG,
             "committing mp3+pcm file. \n"
             "\tmp3: %s (expected), %s (actual), %s; "
             "size: %u (expected), %u (actual), %s; "
             "starting block: %u, blocks: %u\n"
             "\tpcm: %s, size: %u (actual), %u (estimated); starting "
             "block: %u, blocks: %u (actual), %u (estimated)",
             p1, p2, (mp3_digest_match ? "match" : "mismatch"), file->mp3_size,
             file->mp3_written, (mp3_size_match ? "match" : "mismatch"),
             file->mp3_start, file->mp3_blocks, p3, file->pcm_actual_size,
             file->pcm_estimated_size, file->pcm_start, file->pcm_actual_blocks,
             file->pcm_estimated_blocks);

    if (!mp3_digest_match || !mp3_size_match) {
        mmcfs_abort_file(file);
        return -EINVAL;
    }

    int ret = mmcfs_create_file_ll(
        &file->digest, &file->calculated_pcm_digest, file->mp3_start,
        file->mp3_start + file->mp3_blocks, file->mp3_size, MMCFS_FILE_MP3,
        MMCFS_MP3_SUBTYPE_NONE);

    if (ret < 0) {
        mmcfs_abort_file(file);
        return ret;
    }

    ret = mmcfs_create_file_ll(
        &file->calculated_pcm_digest, &file->digest, file->pcm_start,
        file->pcm_start + file->pcm_actual_blocks, file->pcm_actual_size,
        MMCFS_FILE_PCM, MMCFS_PCM_48K_16B_STEREO_OOB_NONE);
    if (ret < 0) {
        mmcfs_abort_file(file);
        return ret;
    }

    uint32_t pcm_unused_start = file->pcm_start + file->pcm_actual_blocks;
    uint32_t pcm_unused_blocks =
        file->pcm_estimated_blocks - file->pcm_actual_blocks;

    if (pcm_unused_blocks) {
        esp_err_t err = clear_bits(pcm_unused_start,
                                   pcm_unused_start + pcm_unused_blocks, NULL);
        if (err != ESP_OK) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            assert(err == ESP_OK);
        } 
    }

    free(file);
    _file = NULL;
    return 0;
}

/*
 * read pcm data into first half or second harf of iobuf (which is 16K)
 */
void mmcfs_pcm_read(const md5_digest_t * digest, int pos, int half) {

    int ret = mmcfs_bucket_read(digest, &bbuf);
    if (ret < 0) {
        goto zero;
    }

    int index = mmcfs_bucket_find_file(&bbuf, digest);
    if (index == -1) {
        goto zero;
    }
    
    md5_digest_t pcm_digest = bbuf.files[index].link;
    ret = mmcfs_bucket_read(&pcm_digest, &bbuf);
    index = mmcfs_bucket_find_file(&bbuf, &pcm_digest);
    if (index == -1) {
        goto zero;
    }

    mmcfs_file_t *file = &bbuf.files[index];
    uint32_t frames = file->size / 8192;
    if (pos < frames) {
        uint32_t file_block_start =
            fs->block_start + file->block_start * fs->block_sect;

        uint32_t sector_start = file_block_start + pos * 8192 / 512;
        uint32_t sector_count = 8192 / 512;
        uint8_t *buf = half == 0 ? iobuf : &iobuf[8192];
        esp_err_t err =
            sdmmc_read_sectors(card, buf, sector_start, sector_count);
        if (err == ESP_OK) {
            return;
        }
    }

zero:
    memset(half == 0 ? iobuf : &iobuf[8192], 0, 8192);
}

void mmcfs_pcm_mix(const md5_digest_t *digest1, int pos1,
                   const md5_digest_t *digest2, int pos2, char buf[8192]) {
    if (digest1 == NULL && digest2 == NULL) {
        memset(buf, 0, 8192);
        return;
    }

    if (digest1 != NULL && digest2 != NULL) {
        mmcfs_pcm_read(digest1, pos1, 0);
        mmcfs_pcm_read(digest2, pos2, 0);
        for (int i = 0; i < FRAME_DAT_SIZE; i++) {
            
        }  
        return;
    }

    if (digest1 != NULL) {
        mmcfs_pcm_read(digest1, pos1, 0);
        memcpy(buf, iobuf, 8192);
        return;
    } 

    if (digest2 != NULL) {
        mmcfs_pcm_read(digest2, pos2, 0);
        memcpy(buf, iobuf, 8192); 
        return;
    }
}
