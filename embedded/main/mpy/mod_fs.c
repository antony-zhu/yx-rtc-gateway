/**
 * @file mod_fs.c
 * @brief MicroPython fs 模块，提供受限 datafs 文件操作。
 */

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "py/obj.h"
#include "py/objlist.h"
#include "py/runtime.h"

#include "gw_crypto.h"
#include "gw_defaults.h"
#include "gw_fs.h"
#include "gw_http.h"

/** @brief 将路径解析错误转换为 Python 异常。 */
static void raise_path_error(esp_err_t err)
{
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid file path"));
    }
    mp_raise_OSError(EIO);
}

/** @brief 将常见 ESP 错误码转换为 fs 模块异常。 */
static void raise_esp_fs_error(esp_err_t err, const char *message)
{
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid fs argument"));
    }
    if (err == ESP_ERR_NO_MEM) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("fs no psram"));
    }
    if (err == ESP_ERR_INVALID_STATE) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("fs invalid state"));
    }
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s: %d"), message, (int)err);
}

/** @brief 使用 errno 或通用 I/O 错误抛出 OSError。 */
static void raise_errno_or_io(void)
{
    mp_raise_OSError(errno != 0 ? errno : EIO);
}

/** @brief 解析脚本路径为真实 VFS 路径。 */
static void resolve_path(mp_obj_t path_obj, char *vfs_path, size_t vfs_size)
{
    const char *path = mp_obj_str_get_str(path_obj);
    esp_err_t err = gw_fs_resolve_data_path(path, vfs_path, vfs_size, NULL, 0);
    if (err != ESP_OK) {
        raise_path_error(err);
    }
}

/** @brief 解析脚本路径为诊断显示路径，例如 datafs:/a。 */
static void resolve_display_path(mp_obj_t path_obj, char *display_path, size_t display_size)
{
    const char *path = mp_obj_str_get_str(path_obj);
    esp_err_t err = gw_fs_resolve_data_path(path, NULL, 0, display_path, display_size);
    if (err != ESP_OK) {
        raise_path_error(err);
    }
}

/** @brief 向 dict 写入运行期字符串 key。 */
static void store_named(mp_obj_t dict, const char *key, mp_obj_t value)
{
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(qstr_from_str(key)), value);
}

/** @brief 使用 PSRAM 分配临时缓冲，避免大文件操作占用内部 RAM。 */
static uint8_t *alloc_psram_or_raise(size_t size)
{
    uint8_t *buf = (uint8_t *)heap_caps_malloc(size > 0U ? size : 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("fs no psram"));
    }
    return buf;
}

/**
 * @brief 判断 datafs 逻辑配额是否允许本次写入。
 *
 * 覆盖写时会扣除目标文件旧大小；append 则按新增字节计算。
 */
static bool datafs_quota_allows_replace(const char *path, size_t write_size, bool append)
{
    gw_fs_partition_status_t datafs = {0};
    struct stat st = {0};
    size_t old_size = 0U;

    if (gw_fs_refresh_partition(GW_FS_PARTITION_DATAFS, &datafs) != ESP_OK) {
        return false;
    }

    if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        old_size = (size_t)st.st_size;
    }

    size_t adjusted_used = datafs.used_bytes;
    if (!append && adjusted_used >= old_size) {
        adjusted_used -= old_size;
    }

    return adjusted_used + write_size <= GW_DATAFS_DEFAULT_QUOTA_BYTES;
}

/** @brief 拼出同目录临时文件或备份文件路径。 */
static void make_sibling_path(const char *path, const char *suffix, char *out, size_t out_size)
{
    if (snprintf(out, out_size, "%s%s", path, suffix) >= (int)out_size) {
        mp_raise_OSError(ENAMETOOLONG);
    }
}

/** @brief 写文件并执行 flush/fsync，确保 replace/download 的落盘边界清晰。 */
static void write_file_checked(const char *path, const void *data, size_t len, bool append)
{
    if (!datafs_quota_allows_replace(path, len, append)) {
        mp_raise_OSError(ENOSPC);
    }

    FILE *fp = fopen(path, append ? "ab" : "wb");
    if (fp == NULL) {
        raise_errno_or_io();
    }

    size_t wrote = len > 0U ? fwrite(data, 1, len, fp) : 0U;
    int flush_rc = fflush(fp);
    int sync_rc = fsync(fileno(fp));
    int close_rc = fclose(fp);
    if (wrote != len || flush_rc != 0 || sync_rc != 0 || close_rc != 0) {
        raise_errno_or_io();
    }
}

/** @brief 构造文件或目录状态字典。 */
static mp_obj_t build_stat_dict(const char *display_path, const char *vfs_path)
{
    struct stat st = {0};
    if (stat(vfs_path, &st) != 0) {
        mp_raise_OSError(errno != 0 ? errno : ENOENT);
    }

    const char *type = S_ISREG(st.st_mode) ? "file" :
                       S_ISDIR(st.st_mode) ? "dir" :
                       "other";
    mp_obj_t dict = mp_obj_new_dict(0);
    store_named(dict, "path", mp_obj_new_str(display_path != NULL ? display_path : "", strlen(display_path != NULL ? display_path : "")));
    store_named(dict, "type", mp_obj_new_str(type, strlen(type)));
    store_named(dict, "size", mp_obj_new_int_from_ull((unsigned long long)st.st_size));
    store_named(dict, "mode", mp_obj_new_int_from_uint((uint32_t)st.st_mode));
    store_named(dict, "mtime", mp_obj_new_int_from_uint((mp_uint_t)st.st_mtime));
    return dict;
}

static mp_obj_t gw_mpy_fs_resolve(mp_obj_t path_obj)
{
    char path[GW_FS_MAX_PATH_LEN];
    resolve_path(path_obj, path, sizeof(path));
    return mp_obj_new_str(path, strlen(path));
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_fs_resolve_obj, gw_mpy_fs_resolve);

static mp_obj_t gw_mpy_fs_exists(mp_obj_t path_obj)
{
    char path[GW_FS_MAX_PATH_LEN];
    struct stat st = {0};

    resolve_path(path_obj, path, sizeof(path));
    return mp_obj_new_bool(stat(path, &st) == 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_fs_exists_obj, gw_mpy_fs_exists);

static mp_obj_t gw_mpy_fs_stat(mp_obj_t path_obj)
{
    char vfs_path[GW_FS_MAX_PATH_LEN];
    char display_path[GW_FS_MAX_PATH_LEN];

    resolve_path(path_obj, vfs_path, sizeof(vfs_path));
    resolve_display_path(path_obj, display_path, sizeof(display_path));
    return build_stat_dict(display_path, vfs_path);
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_fs_stat_obj, gw_mpy_fs_stat);

static mp_obj_t gw_mpy_fs_listdir(size_t n_args, const mp_obj_t *args)
{
    char path[GW_FS_MAX_PATH_LEN];

    if (n_args == 0) {
        esp_err_t err = gw_fs_resolve_data_path("/", path, sizeof(path), NULL, 0);
        if (err != ESP_OK) {
            raise_path_error(err);
        }
    } else {
        resolve_path(args[0], path, sizeof(path));
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        mp_raise_OSError(errno != 0 ? errno : ENOENT);
    }

    mp_obj_t list = mp_obj_new_list(0, NULL);
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        mp_obj_list_append(list, mp_obj_new_str(entry->d_name, strlen(entry->d_name)));
    }
    closedir(dir);
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_fs_listdir_obj, 0, 1, gw_mpy_fs_listdir);
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_fs_list_obj, 0, 1, gw_mpy_fs_listdir);

static mp_obj_t gw_mpy_fs_read(size_t n_args, const mp_obj_t *args)
{
    char path[GW_FS_MAX_PATH_LEN];
    struct stat st = {0};
    size_t max_bytes = 64U * 1024U;

    resolve_path(args[0], path, sizeof(path));
    if (n_args >= 2) {
        mp_int_t requested = mp_obj_get_int(args[1]);
        if (requested < 0 || requested > (mp_int_t)GW_DATAFS_DEFAULT_QUOTA_BYTES) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid max_bytes"));
        }
        max_bytes = (size_t)requested;
    }

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        mp_raise_OSError(errno != 0 ? errno : ENOENT);
    }
    if ((size_t)st.st_size > max_bytes) {
        mp_raise_OSError(EFBIG);
    }

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        raise_errno_or_io();
    }

    uint8_t *buf = (size_t)st.st_size > 0U ? alloc_psram_or_raise((size_t)st.st_size) : NULL;
    size_t got = st.st_size > 0 ? fread(buf, 1, (size_t)st.st_size, fp) : 0U;
    fclose(fp);
    if (got != (size_t)st.st_size) {
        if (buf != NULL) {
            heap_caps_free(buf);
        }
        mp_raise_OSError(EIO);
    }

    mp_obj_t result = got > 0U ? mp_obj_new_bytes(buf, got) : mp_obj_new_bytes((const byte *)"", 0);
    if (buf != NULL) {
        heap_caps_free(buf);
    }
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_fs_read_obj, 1, 2, gw_mpy_fs_read);

static mp_obj_t gw_mpy_fs_write(size_t n_args, const mp_obj_t *args)
{
    char path[GW_FS_MAX_PATH_LEN];
    mp_buffer_info_t data = {0};
    bool append = n_args >= 3 && mp_obj_is_true(args[2]);

    resolve_path(args[0], path, sizeof(path));
    mp_get_buffer_raise(args[1], &data, MP_BUFFER_READ);
    write_file_checked(path, data.buf, data.len, append);
    return mp_obj_new_int_from_uint((uint32_t)data.len);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_fs_write_obj, 2, 3, gw_mpy_fs_write);

static mp_obj_t gw_mpy_fs_replace(mp_obj_t path_obj, mp_obj_t data_obj)
{
    char path[GW_FS_MAX_PATH_LEN];
    char tmp_path[GW_FS_MAX_PATH_LEN + 8U];
    char bak_path[GW_FS_MAX_PATH_LEN + 8U];
    mp_buffer_info_t data = {0};
    struct stat st = {0};
    bool backup_ready = false;

    resolve_path(path_obj, path, sizeof(path));
    mp_get_buffer_raise(data_obj, &data, MP_BUFFER_READ);
    make_sibling_path(path, ".tmp", tmp_path, sizeof(tmp_path));
    make_sibling_path(path, ".bak", bak_path, sizeof(bak_path));

    (void)unlink(tmp_path);
    (void)unlink(bak_path);
    write_file_checked(tmp_path, data.buf, data.len, false);

    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            (void)unlink(tmp_path);
            mp_raise_OSError(EISDIR);
        }
        if (rename(path, bak_path) != 0) {
            (void)unlink(tmp_path);
            raise_errno_or_io();
        }
        backup_ready = true;
    }

    if (rename(tmp_path, path) != 0) {
        int saved_errno = errno;
        (void)unlink(tmp_path);
        if (backup_ready) {
            (void)rename(bak_path, path);
        }
        errno = saved_errno;
        raise_errno_or_io();
    }
    if (backup_ready) {
        (void)unlink(bak_path);
    }
    return mp_obj_new_int_from_uint((uint32_t)data.len);
}
static MP_DEFINE_CONST_FUN_OBJ_2(gw_mpy_fs_replace_obj, gw_mpy_fs_replace);

static mp_obj_t gw_mpy_fs_mkdir(mp_obj_t path_obj)
{
    char path[GW_FS_MAX_PATH_LEN];

    resolve_path(path_obj, path, sizeof(path));
    if (mkdir(path, 0775) != 0) {
        raise_errno_or_io();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_fs_mkdir_obj, gw_mpy_fs_mkdir);

static mp_obj_t gw_mpy_fs_makedirs(mp_obj_t path_obj)
{
    char path[GW_FS_MAX_PATH_LEN];
    resolve_path(path_obj, path, sizeof(path));

    size_t base_len = strlen(GW_FS_DATAFS_BASE_PATH);
    for (char *p = path + base_len + 1U; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(path, 0775) != 0 && errno != EEXIST) {
                *p = '/';
                raise_errno_or_io();
            }
            *p = '/';
        }
    }
    if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        raise_errno_or_io();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_fs_makedirs_obj, gw_mpy_fs_makedirs);

static mp_obj_t gw_mpy_fs_remove(mp_obj_t path_obj)
{
    char path[GW_FS_MAX_PATH_LEN];

    resolve_path(path_obj, path, sizeof(path));
    if (unlink(path) != 0) {
        raise_errno_or_io();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_fs_remove_obj, gw_mpy_fs_remove);

static mp_obj_t gw_mpy_fs_rename(mp_obj_t src_obj, mp_obj_t dst_obj)
{
    char src[GW_FS_MAX_PATH_LEN];
    char dst[GW_FS_MAX_PATH_LEN];

    resolve_path(src_obj, src, sizeof(src));
    resolve_path(dst_obj, dst, sizeof(dst));
    if (rename(src, dst) != 0) {
        raise_errno_or_io();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(gw_mpy_fs_rename_obj, gw_mpy_fs_rename);

static mp_obj_t gw_mpy_fs_rmdir(mp_obj_t path_obj)
{
    char path[GW_FS_MAX_PATH_LEN];

    resolve_path(path_obj, path, sizeof(path));
    if (rmdir(path) != 0) {
        raise_errno_or_io();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(gw_mpy_fs_rmdir_obj, gw_mpy_fs_rmdir);

static mp_obj_t gw_mpy_fs_info(size_t n_args, const mp_obj_t *args)
{
    if (n_args >= 1U) {
        char vfs_path[GW_FS_MAX_PATH_LEN];
        char display_path[GW_FS_MAX_PATH_LEN];
        resolve_path(args[0], vfs_path, sizeof(vfs_path));
        resolve_display_path(args[0], display_path, sizeof(display_path));
        return build_stat_dict(display_path, vfs_path);
    }

    gw_fs_status_t status = {0};
    if (gw_fs_get_status(&status) != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("fs status unavailable"));
    }

    mp_obj_t dict = mp_obj_new_dict(0);
    size_t free_bytes = status.datafs.total_bytes > status.datafs.used_bytes ?
                        status.datafs.total_bytes - status.datafs.used_bytes :
                        0U;
    store_named(dict, "total", mp_obj_new_int_from_ull(status.datafs.total_bytes));
    store_named(dict, "used", mp_obj_new_int_from_ull(status.datafs.used_bytes));
    store_named(dict, "free", mp_obj_new_int_from_ull(free_bytes));
    store_named(dict, "quota", mp_obj_new_int_from_uint(GW_DATAFS_DEFAULT_QUOTA_BYTES));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(gw_mpy_fs_info_obj, 0, 1, gw_mpy_fs_info);

static mp_obj_t gw_mpy_fs_download(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kwargs)
{
    enum {
        ARG_url,
        ARG_path,
        ARG_sha256,
        ARG_max_bytes,
        ARG_connect_timeout_sec,
        ARG_response_interval_timeout_sec,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_url, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_path, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_sha256, MP_ARG_OBJ, {.u_obj = mp_const_none}},
        {MP_QSTR_max_bytes, MP_ARG_INT, {.u_int = GW_SCRIPT_HTTP_RESPONSE_MAX_BYTES}},
        {MP_QSTR_connect_timeout_sec, MP_ARG_INT, {.u_int = GW_HTTP_DEFAULT_CONNECT_TIMEOUT_SEC}},
        {MP_QSTR_response_interval_timeout_sec,
         MP_ARG_INT,
         {.u_int = GW_HTTP_DEFAULT_RESPONSE_INTERVAL_TIMEOUT_SEC}},
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)] = {0};
    mp_arg_parse_all(n_args, pos_args, kwargs, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    mp_int_t requested_max = parsed[ARG_max_bytes].u_int;
    if (requested_max < 0 || requested_max > (mp_int_t)GW_DATAFS_DEFAULT_QUOTA_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid max_bytes"));
    }

    const char *url = mp_obj_str_get_str(parsed[ARG_url].u_obj);
    const char *sha256_expect = NULL;
    if (parsed[ARG_sha256].u_obj != mp_const_none) {
        sha256_expect = mp_obj_str_get_str(parsed[ARG_sha256].u_obj);
    }

    char path[GW_FS_MAX_PATH_LEN];
    resolve_path(parsed[ARG_path].u_obj, path, sizeof(path));

    size_t response_cap = (size_t)requested_max;
    char *body = (char *)heap_caps_calloc(1, response_cap + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        raise_esp_fs_error(ESP_ERR_NO_MEM, "download alloc");
    }

    gw_http_request_t req = {
        .method = GW_HTTP_METHOD_GET,
        .url = url,
        .response_buf = body,
        .response_buf_size = response_cap + 1U,
        .connect_timeout_sec = (uint32_t)parsed[ARG_connect_timeout_sec].u_int,
        .response_interval_timeout_sec = (uint32_t)parsed[ARG_response_interval_timeout_sec].u_int,
    };
    gw_http_response_t resp = {0};
    esp_err_t err = gw_http_perform(&req, &resp);
    if (err != ESP_OK) {
        heap_caps_free(body);
        raise_esp_fs_error(err, "download http");
    }
    if (resp.response_truncated) {
        heap_caps_free(body);
        mp_raise_OSError(EFBIG);
    }

    uint8_t digest[GW_CRYPTO_SHA256_SIZE] = {0};
    char sha256_actual[GW_CRYPTO_SHA256_HEX_SIZE] = {0};
    err = gw_crypto_sha256(body, resp.response_len, digest);
    if (err == ESP_OK) {
        err = gw_crypto_sha256_hex(digest, sha256_actual);
    }
    if (err != ESP_OK) {
        heap_caps_free(body);
        raise_esp_fs_error(err, "download sha256");
    }
    if (sha256_expect != NULL && strcasecmp(sha256_expect, sha256_actual) != 0) {
        heap_caps_free(body);
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("sha256 mismatch"));
    }

    write_file_checked(path, body, resp.response_len, false);
    heap_caps_free(body);

    mp_obj_t dict = mp_obj_new_dict(0);
    store_named(dict, "ok", mp_const_true);
    store_named(dict, "status", mp_obj_new_int(resp.http_status));
    store_named(dict, "size", mp_obj_new_int_from_ull((unsigned long long)resp.response_len));
    store_named(dict, "sha256", mp_obj_new_str(sha256_actual, strlen(sha256_actual)));
    store_named(dict, "path", parsed[ARG_path].u_obj);
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(gw_mpy_fs_download_obj, 2, gw_mpy_fs_download);

static const mp_rom_map_elem_t gw_mpy_fs_globals_table[] = {
    {MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_fs)},
    {MP_ROM_QSTR(MP_QSTR_resolve), MP_ROM_PTR(&gw_mpy_fs_resolve_obj)},
    {MP_ROM_QSTR(MP_QSTR_exists), MP_ROM_PTR(&gw_mpy_fs_exists_obj)},
    {MP_ROM_QSTR(MP_QSTR_stat), MP_ROM_PTR(&gw_mpy_fs_stat_obj)},
    {MP_ROM_QSTR(MP_QSTR_listdir), MP_ROM_PTR(&gw_mpy_fs_listdir_obj)},
    {MP_ROM_QSTR(MP_QSTR_list), MP_ROM_PTR(&gw_mpy_fs_list_obj)},
    {MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&gw_mpy_fs_read_obj)},
    {MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&gw_mpy_fs_write_obj)},
    {MP_ROM_QSTR(MP_QSTR_replace), MP_ROM_PTR(&gw_mpy_fs_replace_obj)},
    {MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&gw_mpy_fs_mkdir_obj)},
    {MP_ROM_QSTR(MP_QSTR_makedirs), MP_ROM_PTR(&gw_mpy_fs_makedirs_obj)},
    {MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&gw_mpy_fs_remove_obj)},
    {MP_ROM_QSTR(MP_QSTR_rename), MP_ROM_PTR(&gw_mpy_fs_rename_obj)},
    {MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&gw_mpy_fs_rmdir_obj)},
    {MP_ROM_QSTR(MP_QSTR_download), MP_ROM_PTR(&gw_mpy_fs_download_obj)},
    {MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&gw_mpy_fs_info_obj)},
};
static MP_DEFINE_CONST_DICT(gw_mpy_fs_globals, gw_mpy_fs_globals_table);

const mp_obj_module_t gw_mpy_module_fs = {
    .base = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&gw_mpy_fs_globals,
};

MP_REGISTER_MODULE(MP_QSTR_fs, gw_mpy_module_fs);
