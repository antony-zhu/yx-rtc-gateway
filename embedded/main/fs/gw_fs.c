#include "gw_fs.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gw_defaults.h"

static const char *TAG = "gw_fs";

typedef struct {
    gw_fs_partition_t partition;
    const char *label;
    const char *base_path;
} gw_fs_mount_def_t;

static const gw_fs_mount_def_t s_mount_defs[] = {
    {GW_FS_PARTITION_CFGFS, GW_FS_CFGFS_LABEL, GW_FS_CFGFS_BASE_PATH},
    {GW_FS_PARTITION_DATAFS, GW_FS_DATAFS_LABEL, GW_FS_DATAFS_BASE_PATH},
    {GW_FS_PARTITION_LOGFS, GW_FS_LOGFS_LABEL, GW_FS_LOGFS_BASE_PATH},
};

static gw_fs_status_t s_status;

static gw_fs_partition_status_t *partition_status_mut(gw_fs_partition_t partition)
{
    switch (partition) {
    case GW_FS_PARTITION_CFGFS:
        return &s_status.cfgfs;
    case GW_FS_PARTITION_DATAFS:
        return &s_status.datafs;
    case GW_FS_PARTITION_LOGFS:
        return &s_status.logfs;
    default:
        return NULL;
    }
}

static const char *partition_label(gw_fs_partition_t partition)
{
    for (size_t i = 0; i < sizeof(s_mount_defs) / sizeof(s_mount_defs[0]); ++i) {
        if (s_mount_defs[i].partition == partition) {
            return s_mount_defs[i].label;
        }
    }
    return NULL;
}

static esp_err_t query_partition_info(const char *label, gw_fs_partition_status_t *status)
{
    size_t total = 0;
    size_t used = 0;
    esp_err_t err = esp_littlefs_info(label, &total, &used);
    if (err != ESP_OK) {
        status->last_error = err;
        return err;
    }

    status->total_bytes = total;
    status->used_bytes = used;
    return ESP_OK;
}

static bool should_force_format(const gw_fs_mount_def_t *def, const gw_fs_init_options_t *options)
{
    if (options == NULL || options->mode != GW_FS_INIT_MODE_FACTORY) {
        return false;
    }

    switch (def->partition) {
    case GW_FS_PARTITION_CFGFS:
        return options->force_format_cfgfs;
    case GW_FS_PARTITION_DATAFS:
        return options->force_format_datafs;
    case GW_FS_PARTITION_LOGFS:
        return options->force_format_logfs;
    default:
        return false;
    }
}

static esp_err_t register_littlefs(const gw_fs_mount_def_t *def)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = def->base_path,
        .partition_label = def->label,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    }

    return err;
}

static esp_err_t mount_with_retry(const gw_fs_mount_def_t *def,
                                  gw_fs_partition_status_t *status)
{
    /* 首次失败后再重试 3 次；重试间隔固定，避免启动阶段频繁扫分区。 */
    const uint8_t max_attempts = (uint8_t)(GW_FS_MOUNT_RETRY_COUNT + 1U);
    esp_err_t err = ESP_FAIL;

    for (uint8_t attempt = 1; attempt <= max_attempts; ++attempt) {
        err = register_littlefs(def);
        status->mount_attempts = attempt;
        status->last_error = err;
        if (err == ESP_OK) {
            return ESP_OK;
        }

        if (attempt < max_attempts) {
            ESP_LOGW(TAG,
                     "%s mount attempt %u/%u failed: %s",
                     def->label,
                     attempt,
                     max_attempts,
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(GW_FS_MOUNT_RETRY_DELAY_MS));
        }
    }

    return err;
}

static void mount_one_partition(const gw_fs_mount_def_t *def, const gw_fs_init_options_t *options)
{
    gw_fs_partition_status_t *status = partition_status_mut(def->partition);
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->last_error = ESP_FAIL;
    status->format_error = ESP_OK;

    if (should_force_format(def, options)) {
        ESP_LOGW(TAG, "%s force format requested by factory init", def->label);
        status->formatted = true;
        status->format_error = esp_littlefs_format(def->label);
        if (status->format_error != ESP_OK) {
            status->last_error = status->format_error;
            ESP_LOGW(TAG,
                     "%s force format failed: %s",
                     def->label,
                     esp_err_to_name(status->format_error));
        }
    }

    esp_err_t err = mount_with_retry(def, status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s mount failed after %u attempts, format and retry once",
                 def->label,
                 (unsigned int)status->mount_attempts);
        status->formatted = true;
        status->format_error = esp_littlefs_format(def->label);
        if (status->format_error == ESP_OK) {
            err = register_littlefs(def);
            status->mount_attempts++;
            status->last_error = err;
        } else {
            err = status->format_error;
            status->last_error = err;
        }
    }

    status->last_error = err;
    status->mounted = (err == ESP_OK);

    if (err == ESP_OK) {
        (void)query_partition_info(def->label, status);
        ESP_LOGI(TAG,
                 "%s mounted at %s total=%lu used=%lu",
                 def->label,
                 def->base_path,
                 (unsigned long)status->total_bytes,
                 (unsigned long)status->used_bytes);
    } else {
        ESP_LOGW(TAG,
                 "%s mount failed: %s format_err=%s",
                 def->label,
                 esp_err_to_name(err),
                 esp_err_to_name(status->format_error));
    }
}

esp_err_t gw_fs_init(void)
{
    return gw_fs_init_with_options(NULL);
}

esp_err_t gw_fs_init_with_options(const gw_fs_init_options_t *options)
{
    memset(&s_status, 0, sizeof(s_status));

    for (size_t i = 0; i < sizeof(s_mount_defs) / sizeof(s_mount_defs[0]); ++i) {
        mount_one_partition(&s_mount_defs[i], options);
    }

    return ESP_OK;
}

esp_err_t gw_fs_get_status(gw_fs_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_status;
    return ESP_OK;
}

esp_err_t gw_fs_refresh_partition(gw_fs_partition_t partition,
                                  gw_fs_partition_status_t *out)
{
    gw_fs_partition_status_t *status = partition_status_mut(partition);
    const char *label = partition_label(partition);
    if (status == NULL || label == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!status->mounted) {
        if (out != NULL) {
            *out = *status;
        }
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = query_partition_info(label, status);
    if (out != NULL) {
        *out = *status;
    }
    return err;
}

static bool path_has_forbidden_char(const char *path)
{
    for (const unsigned char *p = (const unsigned char *)path; *p != '\0'; ++p) {
        if (*p < 0x20 || *p == '\\' || *p == ':') {
            return true;
        }
    }

    return false;
}

static esp_err_t append_segment(char *out, size_t out_size, const char *segment)
{
    size_t used = strlen(out);
    size_t seg_len = strlen(segment);

    if (used + 1U + seg_len + 1U > out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    out[used] = '/';
    memcpy(out + used + 1U, segment, seg_len + 1U);
    return ESP_OK;
}

esp_err_t gw_fs_resolve_data_path(const char *user_path,
                                  char *out_vfs_path,
                                  size_t out_vfs_size,
                                  char *out_display_path,
                                  size_t out_display_size)
{
    if (user_path == NULL || user_path[0] != '/' || path_has_forbidden_char(user_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((out_vfs_path == NULL || out_vfs_size == 0) &&
        (out_display_path == NULL || out_display_size == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(user_path) >= GW_FS_MAX_PATH_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    char temp[GW_FS_MAX_PATH_LEN];
    strlcpy(temp, user_path, sizeof(temp));

    const char *segments[32] = {0};
    size_t depth = 0;
    char *saveptr = NULL;
    char *token = strtok_r(temp, "/", &saveptr);

    while (token != NULL) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            token = strtok_r(NULL, "/", &saveptr);
            continue;
        }

        if (strcmp(token, "..") == 0) {
            if (depth == 0) {
                return ESP_ERR_INVALID_ARG;
            }
            --depth;
            token = strtok_r(NULL, "/", &saveptr);
            continue;
        }

        if (depth >= sizeof(segments) / sizeof(segments[0])) {
            return ESP_ERR_INVALID_SIZE;
        }

        segments[depth++] = token;
        token = strtok_r(NULL, "/", &saveptr);
    }

    if (out_vfs_path != NULL && out_vfs_size > 0) {
        if (strlcpy(out_vfs_path, GW_FS_DATAFS_BASE_PATH, out_vfs_size) >= out_vfs_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        for (size_t i = 0; i < depth; ++i) {
            ESP_RETURN_ON_ERROR(append_segment(out_vfs_path, out_vfs_size, segments[i]),
                                TAG,
                                "append vfs segment failed");
        }
    }

    if (out_display_path != NULL && out_display_size > 0) {
        if (strlcpy(out_display_path, "datafs:", out_display_size) >= out_display_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (depth == 0) {
            if (strlcat(out_display_path, "/", out_display_size) >= out_display_size) {
                return ESP_ERR_INVALID_SIZE;
            }
        }
        for (size_t i = 0; i < depth; ++i) {
            ESP_RETURN_ON_ERROR(append_segment(out_display_path, out_display_size, segments[i]),
                                TAG,
                                "append display segment failed");
        }
    }

    return ESP_OK;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

esp_err_t gw_fs_scan_config_slots(gw_cfg_slot_scan_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    if (!s_status.cfgfs.mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    out->cfg_a_manifest_exists = file_exists(GW_FS_CFGFS_BASE_PATH "/cfg_a/manifest.jsonl");
    out->cfg_a_config_exists = file_exists(GW_FS_CFGFS_BASE_PATH "/cfg_a/config.json");
    out->cfg_a_main_exists = file_exists(GW_FS_CFGFS_BASE_PATH "/cfg_a/main.py");
    out->cfg_a_basic_valid = out->cfg_a_manifest_exists &&
                             out->cfg_a_config_exists &&
                             out->cfg_a_main_exists;

    out->cfg_b_manifest_exists = file_exists(GW_FS_CFGFS_BASE_PATH "/cfg_b/manifest.jsonl");
    out->cfg_b_config_exists = file_exists(GW_FS_CFGFS_BASE_PATH "/cfg_b/config.json");
    out->cfg_b_main_exists = file_exists(GW_FS_CFGFS_BASE_PATH "/cfg_b/main.py");
    out->cfg_b_basic_valid = out->cfg_b_manifest_exists &&
                             out->cfg_b_config_exists &&
                             out->cfg_b_main_exists;

    return ESP_OK;
}

const char *gw_fs_partition_name(gw_fs_partition_t partition)
{
    switch (partition) {
    case GW_FS_PARTITION_CFGFS:
        return GW_FS_CFGFS_LABEL;
    case GW_FS_PARTITION_DATAFS:
        return GW_FS_DATAFS_LABEL;
    case GW_FS_PARTITION_LOGFS:
        return GW_FS_LOGFS_LABEL;
    default:
        return "unknown";
    }
}
