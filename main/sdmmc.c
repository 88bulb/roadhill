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

#define CHECK_EXECUTE_RESULT(err, str)                                         \
    do {                                                                       \
        if ((err) != ESP_OK) {                                                 \
            ESP_LOGE(TAG, str " (0x%x).", err);                                \
            goto cleanup;                                                      \
        }                                                                      \
    } while (0)

static const char *TAG = "sdmmc";

#define SIZE_1MB ((int64_t)(1024 * 1024))
#define SIZE_1GB ((int64_t)(1024 * SIZE_1MB))


#define BLOCK_NUM_BOUND (16 * 8 * 1024)
#define BLOCK_NUM_FRACT (14 * 8 * 1024)

static uint64_t round_up_to_next_power_of_2(uint64_t n) {
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
    return (uint64_t)64 * 1024 * 1024 +
           round_up_to_next_power_of_2((uint64_t)fre_sect * 512) * 7 / 8;
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
                                const char *drv, sdmmc_card_t *card,
                                BYTE pdrv) {
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

    FIL* fp = (FIL*)malloc(sizeof(FIL));
    res = f_open(fp, "temp", FA_WRITE | FA_CREATE_ALWAYS);
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
    f_close(fp);
    free(fp);
    if (res) {
        f_unlink("temp");
        f_unmount("");
        free(fs);
        ESP_LOGI(TAG, "prepare mmc card, failed to expand temp file (%d)", res);
        return ESP_FAIL;
    }

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
    uint32_t fre_clust, fre_sect, tot_sect;

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

        }    break;
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

esp_err_t esp_vfs_exfat_sdmmc_mount(
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
