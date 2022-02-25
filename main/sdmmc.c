#include <string.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"

#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_host.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "vfs_fat_internal.h"
#include "esp_vfs_fat.h"

#include "roadhill.h"

#define CHECK_EXECUTE_RESULT(err, str)                                         \
    do {                                                                       \
        if ((err) != ESP_OK) {                                                 \
            ESP_LOGE(TAG, str " (0x%x).", err);                                \
            goto cleanup;                                                      \
        }                                                                      \
    } while (0)

static const char *TAG = "sdmmc";

uint64_t round_up_to_next_power_of_2(uint64_t n) {
    n = n - 1;
    while (n & (n - 1)) {
        n = n & (n - 1);
    }
    return n << 1;
}

static uint64_t siffs_block_size(DWORD fre_sect) {
    return round_up_to_next_power_of_2((uint64_t)fre_sect * 512) /
           BLOCK_NUM_BOUND;
}

static uint64_t siffs_file_size(DWORD fre_sect) {
    return DATA_OFFSET + round_up_to_next_power_of_2((uint64_t)fre_sect * 512) *
                             BLOCK_NUM_FRACT;
}

void sdmmc_card_info(const sdmmc_card_t *card) {
    bool print_scr = true;
    bool print_csd = true;
    const char *type;

    char *TAG = pcTaskGetName(NULL);

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

static esp_err_t mount_prepare_mem(const char *base_path, BYTE *out_pdrv,
                                   char **out_dup_path,
                                   sdmmc_card_t **out_card) {
    esp_err_t err = ESP_OK;
    char *dup_path = NULL;
    sdmmc_card_t *card = NULL;

    // connect SDMMC driver to FATFS
    BYTE pdrv = FF_DRV_NOT_USED;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == FF_DRV_NOT_USED) {
        ESP_LOGI(TAG, "the maximum count of volumes is already mounted");
        return ESP_ERR_NO_MEM;
    }

    // not using ff_memalloc here, as allocation in internal RAM is preferred
    card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
    if (card == NULL) {
        ESP_LOGI(TAG, "could not locate new sdmmc_card_t");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    dup_path = strdup(base_path);
    if (!dup_path) {
        ESP_LOGI(TAG, "could not copy base_path");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    *out_card = card;
    *out_pdrv = pdrv;
    *out_dup_path = dup_path;
    return ESP_OK;
cleanup:
    free(card);
    free(dup_path);
    return err;
}

/**
 *
 */
static esp_err_t prepare_card(const esp_vfs_fat_mount_config_t *mount_config,
                              const char *drv, sdmmc_card_t *card, BYTE pdrv) {
    FRESULT res = FR_OK;
    const size_t workbuf_size = 4096;
    esp_err_t err;
    FATFS *fs = NULL;
    void *workbuf = NULL;

    ESP_LOGI(TAG, "partitioning card");

    workbuf = ff_memalloc(workbuf_size);
    if (workbuf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    DWORD plist[] = {100, 0, 0, 0};
    res = f_fdisk(pdrv, plist, workbuf);
    if (res != FR_OK) {
        ESP_LOGI(TAG, "f_fdisk failed (%d)", res);
        free(workbuf);
        return ESP_FAIL;
    }

    size_t alloc_unit_size = esp_vfs_fat_get_allocation_unit_size(
        card->csd.sector_size, mount_config->allocation_unit_size);

    ESP_LOGI(TAG, "formatting card, allocation unit size=%d", alloc_unit_size);
    res = f_mkfs(drv, FM_EXFAT, alloc_unit_size, workbuf, workbuf_size);
    free(workbuf);
    if (res != FR_OK) {
        ESP_LOGI(TAG, "f_mkfs failed (%d)", res);
        return ESP_FAIL;
    }

    fs = malloc(sizeof(FATFS));
    if (fs == NULL) {
        ESP_LOGW(TAG, "failed to prepare mmc card, no mem");
        return ESP_ERR_NO_MEM;
    }

    res = f_mount(fs, "", 0);
    if (res != FR_OK) {
        ESP_LOGW(TAG, "failed to prepare mmc card, f_mount failed (%d)", res);
        free(fs);
        return ESP_FAIL;
    }

    DWORD fre_clust, fre_sect, tot_sect;

    res = f_getfree("0:", &fre_clust, &fs);
    if (res != FR_OK) {
        ESP_LOGW(TAG, "failed to prepare mmc card, f_getfree failed (%d)", res);
        free(fs);
        f_unmount("");
        return ESP_FAIL;
    }

    tot_sect = (fs->n_fatent - 2) * fs->csize;
    fre_sect = fre_clust * fs->csize;
    ESP_LOGI(TAG,
             "prepare mmc card, drive size: %u KiB, free space: %u KiB, "
             "siffs block size: %llu KiB",
             tot_sect / 2, fre_sect / 2, siffs_block_size(fre_sect) / 1024);

    // !!! CANNOT BE FREED HERE
    // free(fs);

    FIL *fp = (FIL *)malloc(sizeof(FIL));
    res = f_open(fp, "temp", FA_READ | FA_WRITE | FA_CREATE_ALWAYS);
    if (res) {
        free(fp);
        f_unmount("");
        free(fs);
        ESP_LOGI(TAG, "prepare mmc card, failed to create temp file (%d)", res);
        return ESP_FAIL;
    }

    uint64_t file_size = siffs_file_size(fre_sect);
    ESP_LOGI(TAG, "prepare mmc card, expand to file size: %llu", file_size);

    res = f_expand(fp, file_size, 1);
    if (res) {
        f_close(fp);
        free(fp);
        f_unlink("temp");
        f_unmount("");
        free(fs);
        ESP_LOGI(TAG, "prepare mmc card, failed to expand temp file (%d)", res);
        return ESP_FAIL;
    }

    res = f_lseek(fp, (FSIZE_t)0);
    if (res) {
        f_close(fp);
        free(fp);
        f_unlink("temp");
        f_unmount("");
        free(fs);
        ESP_LOGI(TAG, "prepare mmc card, f_lseek error (%d)", res);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "write zero to first 64MB space");

    unsigned int buf_size = 64 * 1024;
    uint8_t *buf = (uint8_t*)malloc(buf_size);
    memset(buf, 0, buf_size);
    unsigned int written;
    for (int i = 0; i < (DATA_OFFSET / buf_size); i++) {
        res = f_write(fp, buf, buf_size, &written);
        if (res || written != buf_size) {
            f_close(fp);
            free(fp);
            f_unlink("temp");
            f_unmount("");
            free(buf);
            free(fs);
            ESP_LOGI(TAG, "prepare mmc card, f_write error (%d)", res);
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "done");

    free(buf);
    f_close(fp);
    free(fp);

    res = f_rename("temp", "siffs");
    if (res) {
        f_unmount("");
        free(fs);
        ESP_LOGI(TAG, "prepare mmc card, failed to rename temp file (%d)", res);
        return ESP_FAIL;
    }

    res = f_unmount("");
    if (res) {
        ESP_LOGI(TAG, "prepare mmc card, failed to unmount (%d)", res);
        free(fs);
        return ESP_FAIL;
    }

    free(fs);
    return ESP_OK;
}

static esp_err_t
mount_to_vfs_fat(const esp_vfs_fat_mount_config_t *mount_config,
                 sdmmc_card_t *card, uint8_t pdrv, const char *base_path) {
    esp_err_t err;
    // uint32_t fre_clust, fre_sect, tot_sect;

    FRESULT fr;
    FILINFO fno;
    FATFS *fs = NULL;

    ff_diskio_register_sdmmc(pdrv, card);
    ESP_LOGI(TAG, "using pdrv=%i", pdrv);
    char drv[3] = {(char)('0' + pdrv), ':', 0};

    // connect FATFS to VFS
    err = esp_vfs_fat_register(base_path, drv, mount_config->max_files, &fs);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "esp_vfs_fat_register invalid (but it's ok");
        // it's okay, already registered with VFS
    } else if (err != ESP_OK) {
        ESP_LOGI(TAG, "esp_vfs_fat_register failed 0x(%x)", err);
        goto fail;
    }

try_mount:

    ESP_LOGW(TAG, "try to mount mmc card");

    fr = f_mount(fs, drv, 1);
    if (fr != FR_OK) {
        ESP_LOGW(TAG, "failed to mount card (%d)", fr);

        if (!((fr == FR_NO_FILESYSTEM || fr == FR_INT_ERR) &&
              mount_config->format_if_mount_failed)) {
            err = ESP_FAIL;
            goto fail;
        }

        err = prepare_card(mount_config, drv, card, pdrv);
        if (err != ESP_OK) {
            goto fail;
        } else {
            goto try_mount;
        }
    } else if (fs->fs_type != FM_EXFAT) {
        ESP_LOGI(TAG, "not exFAT, unmount and reformat mmc card");
        fr = f_unmount(drv);
        if (fr != FR_OK) {
            err = ESP_FAIL;
            ESP_LOGI(TAG, "f_unmount failed (%d)", fr);
            goto fail;
        }

        err = prepare_card(mount_config, drv, card, pdrv);
        if (err != ESP_OK) {
            goto fail;
        } else {
            goto try_mount;
        }
    } else {
        fr = f_stat("siffs", &fno);

        switch (fr) {
        case FR_OK:
            ESP_LOGI(TAG, "size of siffs file: %lld.", fno.fsize);
            break;
        case FR_NO_FILE: {
            ESP_LOGI(
                TAG,
                "siffs file not found. reformat card and create siffs file");
            fr = f_unmount(drv);
            if (fr != FR_OK) {
                err = ESP_FAIL;
                ESP_LOGI(TAG, "f_unmount failed (%d)", fr);
                goto fail;
            }

            err = prepare_card(mount_config, drv, card, pdrv);
            if (err != ESP_OK) {
                goto fail;
            } else {
                goto try_mount;
            }

        } break;
        default:
            err = ESP_FAIL;
            goto fail;
            break;
        }
    }
    return ESP_OK;

fail:
    if (fs) {
        f_mount(NULL, drv, 0);
    }
    esp_vfs_fat_unregister_path(base_path);
    ff_diskio_unregister(pdrv);
    return err;
}

static esp_err_t esp_vfs_exfat_sdmmc_mount(
    const char *base_path, const sdmmc_host_t *host_config,
    const void *slot_config, const esp_vfs_fat_mount_config_t *mount_config,
    sdmmc_card_t **out_card) {
    esp_err_t err;
    sdmmc_card_t *card = NULL;
    BYTE pdrv = FF_DRV_NOT_USED;
    char *dup_path = NULL;
    bool host_inited = false;

    err = mount_prepare_mem(base_path, &pdrv, &dup_path, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount_prepare failed");
        return err;
    }

    err = (*host_config->init)();
    CHECK_EXECUTE_RESULT(err, "host init failed");
    // deinit() needs to be called to revert the init
    host_inited = true;
    // If this failed (indicated by card_handle != -1), slot deinit needs to
    // called() leave card_handle as is to indicate that (though slot deinit not
    // implemented yet.
    err = sdmmc_host_init_slot(host_config->slot,
                               (const sdmmc_slot_config_t *)slot_config);
    CHECK_EXECUTE_RESULT(err, "slot init failed");

    // probe and initialize card
    err = sdmmc_card_init(host_config, card);
    CHECK_EXECUTE_RESULT(err, "sdmmc_card_init failed");

    err = mount_to_vfs_fat(mount_config, card, pdrv, dup_path);
    CHECK_EXECUTE_RESULT(err, "mount_to_vfs failed");

    if (out_card != NULL) {
        *out_card = card;
    }
    return ESP_OK;
cleanup:
    if (host_inited) {
        if (host_config->flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
            host_config->deinit_p(host_config->slot);
        } else {
            host_config->deinit();
        }
    }
    free(card);
    free(dup_path);
    return err;
}

esp_err_t init_mmc() {
    const char *TAG = "init_mmc";
    esp_err_t err;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 64,
        .allocation_unit_size = 64 * 1024};

    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(TAG, "intializing SD card");

    // Use settings defined above to initialize SD card and mount FAT
    // filesystem. Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience
    // functions. Please check its source code and implement error recovery when
    // developing production applications.
    ESP_LOGI(TAG, "Using SDMMC peripheral");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    // This initializes the slot without card detect (CD) and write protect (WP)
    // signals. Modify slot_config.gpio_cd and slot_config.gpio_wp if your board
    // has these signals.
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    // To use 1-line SD mode, change this to 1:
    slot_config.width = 1;

    // Enable internal pullups on enabled pins. The internal pullups
    // are insufficient however, please make sure 10k external pullups are
    // connected on the bus. This is for debug / example purpose only.
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "mounting exfat on sdmmc");
    err = esp_vfs_exfat_sdmmc_mount(mount_point, &host, &slot_config,
                                    &mount_config, &card);
    if (err == ESP_OK) {
        sdmmc_card_info(card);
    } else {
        ESP_LOGE(TAG,
                 "Failed to initialize the card (%s). "
                 "Make sure SD card lines have pull-up resistors in place.",
                 esp_err_to_name(err));
    }

    return err;
}
