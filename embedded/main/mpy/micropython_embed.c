/**
 * @file micropython_embed.c
 * @brief MicroPython VM 初始化、脚本执行和异常文本提取。
 */

#include "micropython_embed.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "py/builtin.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/obj.h"
#include "py/objlist.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "py/stream.h"
#include "shared/runtime/gchelper.h"

#include "gw_defaults.h"
#include "gw_fs.h"

#define GW_MPY_SCRIPT_MAX_BYTES (1024U * 1024U)
#define GW_MPY_PATH_MAX         256U

static char s_import_root[GW_MPY_PATH_MAX];

static bool path_is_safe_import_target(const char *path);
static bool path_is_partition_root_or_child(const char *path, const char *root);

typedef struct {
    mp_obj_base_t base;
    FILE *fp;
    bool enforce_data_quota;
    bool append_mode;
} gw_mpy_file_obj_t;

typedef struct {
    char *buffer;
    size_t size;
    size_t len;
} gw_micropython_exception_capture_t;

static void exception_capture_strn(void *data, const char *str, size_t len)
{
    gw_micropython_exception_capture_t *capture = (gw_micropython_exception_capture_t *)data;
    if (capture == NULL || capture->buffer == NULL || capture->size == 0U ||
        str == NULL || len == 0U || capture->len >= capture->size - 1U) {
        return;
    }

    size_t writable = capture->size - capture->len - 1U;
    if (writable > len) {
        writable = len;
    }
    memcpy(capture->buffer + capture->len, str, writable);
    capture->len += writable;
    capture->buffer[capture->len] = '\0';
}

static void capture_exception(mp_obj_t exc_obj, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U || exc_obj == MP_OBJ_NULL) {
        return;
    }

    out[0] = '\0';
    gw_micropython_exception_capture_t capture = {
        .buffer = out,
        .size = out_size,
        .len = 0U,
    };
    mp_print_t print = {
        .data = &capture,
        .print_strn = exception_capture_strn,
    };
    mp_obj_print_exception(&print, exc_obj);
}

static bool is_keyboard_interrupt(mp_obj_t exc_obj)
{
    return exc_obj != MP_OBJ_NULL &&
           mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(((mp_obj_base_t *)exc_obj)->type),
                                   MP_OBJ_FROM_PTR(&mp_type_KeyboardInterrupt));
}

static mp_obj_t new_script_globals(const char *source_name)
{
    const char *file_name = source_name != NULL ? source_name : "<stdin>";
    mp_obj_t globals = mp_obj_new_dict(0);

    mp_obj_dict_store(globals,
                      MP_OBJ_NEW_QSTR(qstr_from_str("__name__")),
                      mp_obj_new_str("__main__", 8));
    mp_obj_dict_store(globals,
                      MP_OBJ_NEW_QSTR(qstr_from_str("__file__")),
                      mp_obj_new_str(file_name, strlen(file_name)));
    return globals;
}

static void maybe_call_script_main(mp_obj_t globals)
{
    mp_obj_dict_t *globals_dict = MP_OBJ_TO_PTR(globals);
    mp_map_t *map = &globals_dict->map;
    mp_obj_t key = MP_OBJ_NEW_QSTR(qstr_from_str("main"));
    mp_map_elem_t *elem = mp_map_lookup(map, key, MP_MAP_LOOKUP);

    if (elem == NULL || elem->value == MP_OBJ_NULL || elem->value == mp_const_none) {
        return;
    }

    if (!mp_obj_is_callable(elem->value)) {
        mp_raise_TypeError(MP_ERROR_TEXT("main is not callable"));
    }

    /*
     * 网关框架负责调用 main()；用户脚本不需要再写
     * if __name__ == "__main__": main()，否则会执行两次。
     */
    (void)mp_call_function_0(elem->value);
}

static esp_err_t exec_str(const char *source_name,
                          const char *src,
                          bool *had_exception,
                          bool *was_keyboard_interrupt,
                          char *exception_text,
                          size_t exception_text_size)
{
    nlr_buf_t nlr;

    if (had_exception != NULL) {
        *had_exception = false;
    }
    if (was_keyboard_interrupt != NULL) {
        *was_keyboard_interrupt = false;
    }
    if (src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(qstr_from_str(source_name != NULL ? source_name : "<stdin>"),
                                                    src,
                                                    strlen(src),
                                                    0);
        mp_obj_t globals = new_script_globals(source_name);
        mp_obj_dict_t *globals_dict = MP_OBJ_TO_PTR(globals);

        (void)mp_parse_compile_execute(lex, MP_PARSE_FILE_INPUT, globals_dict, globals_dict);
        maybe_call_script_main(globals);
        nlr_pop();
        return ESP_OK;
    }

    if (had_exception != NULL) {
        *had_exception = true;
    }
    if (was_keyboard_interrupt != NULL) {
        *was_keyboard_interrupt = is_keyboard_interrupt((mp_obj_t)nlr.ret_val);
    }
    capture_exception((mp_obj_t)nlr.ret_val, exception_text, exception_text_size);
    mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    return ESP_OK;
}

void gw_micropython_init(void *gc_heap, size_t gc_heap_size, void *stack_top)
{
    mp_stack_ctrl_init();
    mp_stack_set_top(stack_top);
    gc_init(gc_heap, (uint8_t *)gc_heap + gc_heap_size);
    mp_init();
}

void gw_micropython_deinit(void)
{
    mp_deinit();
    s_import_root[0] = '\0';
}

esp_err_t gw_micropython_exec_file(const char *real_path,
                                   bool *had_exception,
                                   bool *was_keyboard_interrupt,
                                   char *exception_text,
                                   size_t exception_text_size)
{
    FILE *fp = NULL;
    char *buffer = NULL;
    long file_size = 0;
    size_t read_size = 0U;
    esp_err_t err = ESP_OK;

    if (real_path == NULL || real_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    fp = fopen(real_path, "rb");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        err = ESP_FAIL;
        goto cleanup;
    }
    file_size = ftell(fp);
    if (file_size < 0 || file_size > (long)GW_MPY_SCRIPT_MAX_BYTES || fseek(fp, 0, SEEK_SET) != 0) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    buffer = (char *)heap_caps_malloc((size_t)file_size + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    read_size = fread(buffer, 1, (size_t)file_size, fp);
    if (read_size != (size_t)file_size) {
        err = ESP_FAIL;
        goto cleanup;
    }
    buffer[file_size] = '\0';
    err = exec_str(real_path, buffer, had_exception, was_keyboard_interrupt, exception_text, exception_text_size);

cleanup:
    if (fp != NULL) {
        fclose(fp);
    }
    if (buffer != NULL) {
        memset(buffer, 0, (size_t)(file_size > 0 ? file_size : 0));
        heap_caps_free(buffer);
    }
    return err;
}

void gw_micropython_prepare_sys_path(const char *active_slot_path)
{
    mp_obj_list_t *path_list = MP_OBJ_TO_PTR(mp_sys_path);

    if (active_slot_path != NULL && active_slot_path[0] != '\0') {
        strlcpy(s_import_root, active_slot_path, sizeof(s_import_root));
        mp_obj_list_append(MP_OBJ_FROM_PTR(path_list),
                           mp_obj_new_str(active_slot_path, strlen(active_slot_path)));
    }
}

void gw_micropython_bind_stack_top(void *stack_top)
{
    if (stack_top != NULL) {
        mp_stack_set_top(stack_top);
    }
}

void gw_micropython_request_keyboard_interrupt(void)
{
    mp_sched_keyboard_interrupt();
}

void gc_collect(void)
{
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

void nlr_jump_fail(void *val)
{
    (void)val;
    abort();
}

void MP_NORETURN __fatal_error(const char *msg)
{
    (void)msg;
    abort();
}

mp_import_stat_t mp_import_stat(const char *path)
{
    struct stat st = {0};

    if (!path_is_safe_import_target(path)) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    if (stat(path, &st) != 0) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    if (S_ISDIR(st.st_mode)) {
        return MP_IMPORT_STAT_DIR;
    }
    if (S_ISREG(st.st_mode)) {
        return MP_IMPORT_STAT_FILE;
    }
    return MP_IMPORT_STAT_NO_EXIST;
}

static bool parse_open_mode(const char *mode,
                            char *c_mode,
                            size_t c_mode_size,
                            bool *binary,
                            bool *append_mode)
{
    char op = '\0';
    bool plus = false;
    bool has_binary = false;
    bool has_text = false;
    size_t pos = 0U;

    if (mode == NULL || mode[0] == '\0' || c_mode == NULL || c_mode_size < 4U ||
        binary == NULL || append_mode == NULL) {
        return false;
    }

    for (const char *p = mode; *p != '\0'; ++p) {
        switch (*p) {
        case 'r':
        case 'w':
        case 'a':
            if (op != '\0') {
                return false;
            }
            op = *p;
            break;
        case '+':
            if (plus) {
                return false;
            }
            plus = true;
            break;
        case 'b':
            if (has_binary) {
                return false;
            }
            has_binary = true;
            break;
        case 't':
            if (has_text) {
                return false;
            }
            has_text = true;
            break;
        default:
            return false;
        }
    }
    if (op == '\0' || (has_binary && has_text)) {
        return false;
    }

    c_mode[pos++] = op;
    if (has_binary) {
        c_mode[pos++] = 'b';
    }
    if (plus) {
        c_mode[pos++] = '+';
    }
    c_mode[pos] = '\0';
    *binary = has_binary;
    *append_mode = op == 'a';
    return true;
}

static bool segment_has_forbidden_char(const char *seg, size_t seg_len)
{
    for (size_t i = 0; i < seg_len; ++i) {
        unsigned char ch = (unsigned char)seg[i];
        if (ch < 0x20 || ch == '\\' || ch == ':') {
            return true;
        }
    }
    return false;
}

static bool append_path_segment(char *out, size_t out_size, size_t *len, const char *seg, size_t seg_len)
{
    if (*len + 1U + seg_len >= out_size) {
        return false;
    }
    out[(*len)++] = '/';
    memcpy(out + *len, seg, seg_len);
    *len += seg_len;
    out[*len] = '\0';
    return true;
}

static bool resolve_data_open_path(const char *path, char *out, size_t out_size)
{
    size_t len = strlen("/datafs");

    if (path == NULL || path[0] != '/' || out == NULL || out_size <= len) {
        return false;
    }
    memcpy(out, "/datafs", len + 1U);

    const char *p = path + 1;
    while (*p != '\0') {
        const char *start = p;
        while (*p != '\0' && *p != '/') {
            ++p;
        }
        size_t seg_len = (size_t)(p - start);
        if (seg_len == 0U || (seg_len == 1U && start[0] == '.')) {
            /* 忽略空段和当前目录。 */
        } else if (seg_len == 2U && start[0] == '.' && start[1] == '.') {
            if (len == strlen("/datafs")) {
                return false;
            }
            while (len > strlen("/datafs") && out[len - 1U] != '/') {
                --len;
            }
            if (len > strlen("/datafs")) {
                --len;
            }
            out[len] = '\0';
        } else if (segment_has_forbidden_char(start, seg_len)) {
            return false;
        } else if (!append_path_segment(out, out_size, &len, start, seg_len)) {
            return false;
        }
        if (*p == '/') {
            ++p;
        }
    }
    return true;
}

static bool path_contains_parent_segment(const char *path)
{
    const char *p = path;

    while (p != NULL && *p != '\0') {
        while (*p == '/') {
            ++p;
        }
        const char *start = p;
        while (*p != '\0' && *p != '/') {
            ++p;
        }
        if ((size_t)(p - start) == 2U && start[0] == '.' && start[1] == '.') {
            return true;
        }
    }
    return false;
}

static bool path_is_partition_root_or_child(const char *path, const char *root)
{
    if (path == NULL || root == NULL) {
        return false;
    }
    size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) != 0) {
        return false;
    }
    return path[root_len] == '\0' || path[root_len] == '/';
}

static bool path_is_safe_import_target(const char *path)
{
    size_t root_len = strlen(s_import_root);

    /*
     * import 只允许从当前 active 配置区加载用户模块。脚本可修改 sys.path，
     * 因此这里必须再次约束，避免跨区读取 cfgfs、datafs 或 logfs 内容。
     */
    if (path == NULL || root_len == 0U ||
        strlen(path) >= GW_MPY_PATH_MAX ||
        strncmp(path, s_import_root, root_len) != 0 ||
        (path[root_len] != '\0' && path[root_len] != '/') ||
        path_contains_parent_segment(path) ||
        segment_has_forbidden_char(path, strlen(path))) {
        return false;
    }
    return true;
}

static bool resolve_open_path(const char *path, const char *c_mode, char *out, size_t out_size)
{
    (void)c_mode;

    if (path == NULL || path[0] != '/' || out == NULL || out_size == 0U) {
        return false;
    }

    if (path_is_partition_root_or_child(path, GW_FS_CFGFS_BASE_PATH) ||
        path_is_partition_root_or_child(path, GW_FS_LOGFS_BASE_PATH)) {
        return false;
    }

    if (strncmp(path, "/datafs/", 8) == 0) {
        char user_path[GW_MPY_PATH_MAX];
        int written = snprintf(user_path, sizeof(user_path), "/%s", path + 8);
        if (written <= 0 || written >= (int)sizeof(user_path)) {
            return false;
        }
        return resolve_data_open_path(user_path, out, out_size);
    }

    return resolve_data_open_path(path, out, out_size);
}

static void file_check_open(gw_mpy_file_obj_t *self)
{
    if (self == NULL || self->fp == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("I/O operation on closed file"));
    }
}

static void file_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind)
{
    (void)kind;
    gw_mpy_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<io.%s %p>", mp_obj_get_type_str(self_in), self->fp);
}

static mp_uint_t file_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode)
{
    gw_mpy_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    size_t got = fread(buf, 1, size, self->fp);
    if (got < size && ferror(self->fp) != 0) {
        *errcode = errno != 0 ? errno : EIO;
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)got;
}

static mp_uint_t file_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode)
{
    gw_mpy_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->enforce_data_quota && size > 0U) {
        gw_fs_partition_status_t datafs = {0};
        struct stat st = {0};
        long pos = ftell(self->fp);
        if (gw_fs_refresh_partition(GW_FS_PARTITION_DATAFS, &datafs) != ESP_OK ||
            fstat(fileno(self->fp), &st) != 0 ||
            st.st_size < 0 ||
            pos < 0) {
            *errcode = EIO;
            return MP_STREAM_ERROR;
        }

        size_t old_size = (size_t)st.st_size;
        size_t write_start = self->append_mode || (size_t)pos > old_size ? old_size : (size_t)pos;
        size_t new_size = write_start + (size_t)size;
        if (new_size < write_start) {
            *errcode = EFBIG;
            return MP_STREAM_ERROR;
        }
        if (new_size < old_size) {
            new_size = old_size;
        }

        size_t adjusted_used = datafs.used_bytes >= old_size ? datafs.used_bytes - old_size : 0U;
        if (adjusted_used + new_size > GW_DATAFS_DEFAULT_QUOTA_BYTES) {
            *errcode = ENOSPC;
            return MP_STREAM_ERROR;
        }
    }

    size_t wrote = fwrite(buf, 1, size, self->fp);
    if (wrote < size && ferror(self->fp) != 0) {
        *errcode = errno != 0 ? errno : EIO;
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)wrote;
}

static mp_uint_t file_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode)
{
    gw_mpy_file_obj_t *self = MP_OBJ_TO_PTR(self_in);

    switch (request) {
    case MP_STREAM_FLUSH:
        if (fflush(self->fp) != 0) {
            *errcode = errno != 0 ? errno : EIO;
            return MP_STREAM_ERROR;
        }
        (void)fsync(fileno(self->fp));
        return 0;

    case MP_STREAM_SEEK: {
        struct mp_stream_seek_t *seek = (struct mp_stream_seek_t *)arg;
        if (seek == NULL || fseek(self->fp, (long)seek->offset, seek->whence) != 0) {
            *errcode = errno != 0 ? errno : EIO;
            return MP_STREAM_ERROR;
        }
        long offset = ftell(self->fp);
        if (offset < 0) {
            *errcode = errno != 0 ? errno : EIO;
            return MP_STREAM_ERROR;
        }
        seek->offset = (mp_off_t)offset;
        return 0;
    }

    case MP_STREAM_CLOSE:
        if (self->fp != NULL) {
            (void)fclose(self->fp);
            self->fp = NULL;
        }
        return 0;

    case MP_STREAM_GET_FILENO:
        if (self->fp == NULL) {
            *errcode = EBADF;
            return MP_STREAM_ERROR;
        }
        return (mp_uint_t)fileno(self->fp);

    default:
        *errcode = EINVAL;
        return MP_STREAM_ERROR;
    }
}

static mp_obj_t file_fileno(mp_obj_t self_in)
{
    gw_mpy_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    file_check_open(self);
    return MP_OBJ_NEW_SMALL_INT(fileno(self->fp));
}
static MP_DEFINE_CONST_FUN_OBJ_1(file_fileno_obj, file_fileno);

static const mp_rom_map_elem_t file_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_fileno), MP_ROM_PTR(&file_fileno_obj)},
    {MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj)},
    {MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj)},
    {MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj)},
    {MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj)},
    {MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp_stream_seek_obj)},
    {MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp_stream_tell_obj)},
    {MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj)},
    {MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj)},
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj)},
    {MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj)},
    {MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj)},
};
static MP_DEFINE_CONST_DICT(file_locals, file_locals_table);

static const mp_stream_p_t fileio_stream = {
    .read = file_read,
    .write = file_write,
    .ioctl = file_ioctl,
};

MP_DEFINE_CONST_OBJ_TYPE(
    gw_mpy_type_fileio,
    MP_QSTR_FileIO,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    print, file_print,
    protocol, &fileio_stream,
    locals_dict, &file_locals);

static const mp_stream_p_t textio_stream = {
    .read = file_read,
    .write = file_write,
    .ioctl = file_ioctl,
    .is_text = true,
};

MP_DEFINE_CONST_OBJ_TYPE(
    gw_mpy_type_textio,
    MP_QSTR_TextIOWrapper,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    print, file_print,
    protocol, &textio_stream,
    locals_dict, &file_locals);

mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs)
{
    enum {
        ARG_file,
        ARG_mode,
    };
    static const mp_arg_t allowed_args[] = {
        {MP_QSTR_file, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL}},
        {MP_QSTR_mode, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL}},
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)] = {0};
    const char *path = NULL;
    const char *mode = "r";
    char c_mode[4] = {0};
    char real_path[GW_MPY_PATH_MAX] = {0};
    bool binary = false;
    bool append_mode = false;

    mp_arg_parse_all(n_args, args, kwargs, MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);
    path = mp_obj_str_get_str(parsed[ARG_file].u_obj);
    if (parsed[ARG_mode].u_obj != MP_OBJ_NULL && parsed[ARG_mode].u_obj != mp_const_none) {
        mode = mp_obj_str_get_str(parsed[ARG_mode].u_obj);
    }
    if (!parse_open_mode(mode, c_mode, sizeof(c_mode), &binary, &append_mode)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid file mode"));
    }
    if (!resolve_open_path(path, c_mode, real_path, sizeof(real_path))) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid file path"));
    }

    FILE *fp = fopen(real_path, c_mode);
    if (fp == NULL) {
        mp_raise_OSError(errno != 0 ? errno : EIO);
    }

    gw_mpy_file_obj_t *file = mp_obj_malloc_with_finaliser(gw_mpy_file_obj_t,
                                                           binary ? &gw_mpy_type_fileio : &gw_mpy_type_textio);
    file->fp = fp;
    file->enforce_data_quota = strncmp(real_path, GW_FS_DATAFS_BASE_PATH, strlen(GW_FS_DATAFS_BASE_PATH)) == 0;
    file->append_mode = append_mode;
    return MP_OBJ_FROM_PTR(file);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);
