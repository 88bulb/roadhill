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

static esp_err_t mount_prepare_mem(const char *base_path, BYTE *out_pdrv,
                                   char **out_dup_path,
                                   sdmmc_card_t **out_card) {
    esp_err_t err = ESP_OK;
    char *dup_path = NULL;
    sdmmc_card_t *card = NULL;

    // connect SDMMC driver to FATFS
    BYTE pdrv = FF_DRV_NOT_USED;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == FF_DRV_NOT_USED) {
        ESP_LOGD(TAG, "the maximum count of volumes is already mounted");
        return ESP_ERR_NO_MEM;
    }

    // not using ff_memalloc here, as allocation in internal RAM is preferred
    card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
    if (card == NULL) {
        ESP_LOGD(TAG, "could not locate new sdmmc_card_t");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    dup_path = strdup(base_path);
    if (!dup_path) {
        ESP_LOGD(TAG, "could not copy base_path");
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

static esp_err_t partition_card(const esp_vfs_fat_mount_config_t *mount_config,
                                const char *drv, sdmmc_card_t *card,
                                BYTE pdrv) {
    FRESULT res = FR_OK;
    esp_err_t err;
    const size_t workbuf_size = 4096;
    void *workbuf = NULL;
    ESP_LOGW(TAG, "partitioning card");

    workbuf = ff_memalloc(workbuf_size);
    if (workbuf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    DWORD plist[] = {100, 0, 0, 0};
    res = f_fdisk(pdrv, plist, workbuf);
    if (res != FR_OK) {
        err = ESP_FAIL;
        ESP_LOGD(TAG, "f_fdisk failed (%d)", res);
        goto fail;
    }
    size_t alloc_unit_size = esp_vfs_fat_get_allocation_unit_size(
        card->csd.sector_size, mount_config->allocation_unit_size);
    ESP_LOGW(TAG, "formatting card, allocation unit size=%d", alloc_unit_size);
    res = f_mkfs(drv, FM_EXFAT, alloc_unit_size, workbuf, workbuf_size);
    if (res != FR_OK) {
        err = ESP_FAIL;
        ESP_LOGD(TAG, "f_mkfs failed (%d)", res);
        goto fail;
    }

    free(workbuf);
    return ESP_OK;
fail:
    free(workbuf);
    return err;
}

static esp_err_t
mount_to_vfs_fat(const esp_vfs_fat_mount_config_t *mount_config,
                 sdmmc_card_t *card, uint8_t pdrv, const char *base_path) {
    FATFS *fs = NULL;
    esp_err_t err;
    ff_diskio_register_sdmmc(pdrv, card);
    ESP_LOGD(TAG, "using pdrv=%i", pdrv);
    char drv[3] = {(char)('0' + pdrv), ':', 0};

    // connect FATFS to VFS
    err = esp_vfs_fat_register(base_path, drv, mount_config->max_files, &fs);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "esp_vfs_fat_register invalid (but it's ok");
        // it's okay, already registered with VFS
    } else if (err != ESP_OK) {
        ESP_LOGD(TAG, "esp_vfs_fat_register failed 0x(%x)", err);
        goto fail;
    }

    // Try to mount partition
    FRESULT res = f_mount(fs, drv, 1);
    if (res != FR_OK) {
        err = ESP_FAIL;
        ESP_LOGW(TAG, "failed to mount card (%d)", res);
        if (!((res == FR_NO_FILESYSTEM || res == FR_INT_ERR) &&
              mount_config->format_if_mount_failed)) {
            goto fail;
        }

        err = partition_card(mount_config, drv, card, pdrv);
        if (err != ESP_OK) {
            goto fail;
        }

        ESP_LOGW(TAG, "mounting again");
        res = f_mount(fs, drv, 0);
        if (res != FR_OK) {
            err = ESP_FAIL;
            ESP_LOGD(TAG, "f_mount failed after formatting (%d)", res);
            goto fail;
        }
    } else {
        if (fs->fs_type != FM_EXFAT) {
            ESP_LOGI(TAG, "not exFAT, reformatting card...");
            res = f_unmount(drv);
            if (res != FR_OK) {
                err = ESP_FAIL;
                ESP_LOGD(TAG, "f_unmount failed (%d)", res);
                goto fail;
            }

            err = partition_card(mount_config, drv, card, pdrv);
            if (err != ESP_OK) {
                goto fail;
            }

            ESP_LOGW(TAG, "mounting again");
            res = f_mount(fs, drv, 0);
            if (res != FR_OK) {
                err = ESP_FAIL;
                ESP_LOGD(TAG, "f_mount failed after formatting (%d)", res);
                goto fail;
            }
        } else {
            /**
            TODO
                        FRESULT fr;
                        FILINFO fno;
                        fr = f_stat("initialized", &fno);
                        switch (fr) {
                        case FR_OK:
                            ESP_LOGI(TAG, "file system properly initialized.");
                            break;
                        case FR_NO_FILE: {
                            ESP_LOGI(TAG, "file system not properly
            initialized."); res = f_unmount(drv); if (res != FR_OK) { err =
            ESP_FAIL; ESP_LOGD(TAG, "f_unmount failed (%d)", res); goto fail;
                            }

                            err = partition_card(mount_config, drv, card, pdrv);
                            if (err != ESP_OK) {
                                goto fail;
                            }

                            ESP_LOGW(TAG, "mounting again");
                            res = f_mount(fs, drv, 0);
                            if (res != FR_OK) {
                                err = ESP_FAIL;
                                ESP_LOGD(TAG, "f_mount failed after formatting
            (%d)", res); goto fail;
                            }
                        } break;
                        default:
                            break;
                        }
            */
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
