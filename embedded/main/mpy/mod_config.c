/**
 * @file mod_config.c
 * @brief MicroPython config 模块，提供当前生效 config.json 只读访问。
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "gw_config_manager.h"
#include "gw_defaults.h"
#include "gw_fs.h"

static void dict_store_bool(mp_obj_t dict, qstr key, bool value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_bool(value));
}

static void dict_store_u32(mp_obj_t dict, qstr key, uint32_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_int_from_uint(value));
}

static void dict_store_str(mp_obj_t dict, qstr key, const char *value)
{
    const char *text = value != NULL ? value : "";
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(key), mp_obj_new_str(text, strlen(text)));
}

static esp_err_t build_active_config_path(char *out, size_t out_size, gw_config_active_t *out_active)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_config_active_t active = {0};
    esp_err_t err = gw_config_manager_get_active(&active);
    if (err != ESP_OK) {
        return err;
    }

    int written = snprintf(out,
                           out_size,
                           GW_FS_CFGFS_BASE_PATH "/%s/config.json",
                           active.active_slot);
    if (written <= 0 || written >= (int)out_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (out_active != NULL) {
        *out_active = active;
    }
    return ESP_OK;
}

static bool file_exists(const char *path)
{
    struct stat st = {0};
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static mp_obj_t gw_mpy_config_info(void)
{
    char path[GW_FS_MAX_PATH_LEN];
    gw_config_active_t active = {0};

    esp_err_t err = build_active_config_path(path, sizeof(path), &active);
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("active config unavailable"));
    }

    mp_obj_t dict = mp_obj_new_dict(0);
    dict_store_str(dict, MP_QSTR_active_slot, active.active_slot);
    dict_store_u32(dict, MP_QSTR_sequence, active.seq_id);
    dict_store_str(dict, MP_QSTR_manifest_sha256, active.manifest_sha256);
    dict_store_bool(dict, MP_QSTR_loaded_from_nvs, active.loaded_from_nvs);
    dict_store_bool(dict, MP_QSTR_has_config_json, file_exists(path));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(gw_mpy_config_info_obj, gw_mpy_config_info);

static mp_obj_t gw_mpy_config_read(size_t n_args, const mp_obj_t *args)
{
    char path[GW_FS_MAX_PATH_LEN];
    struct stat st = {0};
    size_t max_bytes = GW_CONFIG_MANIFEST_MAX_BYTES;

    if (n_args >= 1) {
        mp_int_t requested = mp_obj_get_int(args[0]);
        if (requested < 0 || requested > (mp_int_t)GW_CONFIG_SLOT_MAX_BYTES) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid max_bytes"));
        }
        max_bytes = (size_t)requested;
    }

    esp_err_t err = build_active_config_path(path, sizeof(path), NULL);
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("active config unavailable"));
    }

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        mp_raise_OSError(errno != 0 ? errno : ENOENT);
    }
    if ((size_t)st.st_size > max_bytes) {
        mp_raise_OSError(EFBIG);
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        mp_raise_OSError(errno != 0 ? errno : EIO);
    }

    char *buf = (char *)heap_caps_malloc((size_t)st.st_size + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(fp);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("config read no memory"));
    }

    size_t got = st.st_size > 0 ? fread(buf, 1, (size_t)st.st_size, fp) : 0U;
    fclose(fp);
    if (got != (size_t)st.st_size) {
        heap_caps_free(buf);
        mp_raise_OSError(EIO);
    }
    buf[got] = '\0';

    mp_obj_t result = mp_obj_new_str(buf, got);
    heap_caps_free(buf);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_config_read_obj, 0, 1, gw_mpy_config_read);

static const mp_rom_map_elem_t gw_mpy_config_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_config)},
    {MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&gw_mpy_config_info_obj)},
    {MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&gw_mpy_config_read_obj)},
};
static MP_DEFINE_CONST_DICT(gw_mpy_config_globals, gw_mpy_config_globals_table);

const mp_obj_module_t gw_mpy_module_config = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&gw_mpy_config_globals,
};

MP_REGISTER_MODULE(MP_QSTR_config, gw_mpy_module_config);
