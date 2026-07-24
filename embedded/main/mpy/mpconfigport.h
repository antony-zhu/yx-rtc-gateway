#pragma once

/**
 * @file mpconfigport.h
 * @brief 网关固件 MicroPython 最小 port 配置。
 *
 * 当前阶段只打开执行业务脚本所需的核心能力，并提供 runtime.should_stop()
 * 给脚本感知停止请求。完整网关 API 后续按模块逐步接入。
 */

#include <alloca.h>
#include <stdint.h>

#define MICROPY_CONFIG_ROM_LEVEL                 (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)
#define MICROPY_ENABLE_COMPILER                  (1)
#define MICROPY_ENABLE_GC                        (1)
#define MICROPY_PY_GC                            (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT           (1)
#define MICROPY_PY_SYS                           (1)
#define MICROPY_PY_SYS_PATH                      (1)
#define MICROPY_PY_SYS_ARGV                      (1)
#define MICROPY_PY_SYS_EXIT                      (1)
#define MICROPY_PY_SYS_MODULES                   (1)
#define MICROPY_PY_IO                            (1)
#define MICROPY_PY_IO_IOBASE                     (1)
#define MICROPY_PY_BUILTINS_BYTES_HEX            (1)
#define MICROPY_PY_BUILTINS_COMPILE              (1)
#define MICROPY_PY_BUILTINS_EVAL_EXEC            (1)
#define MICROPY_PY_BUILTINS_EXECFILE             (0)
#define MICROPY_PY_JSON                          (1)
#define MICROPY_PY_TIME                          (1)
#define MICROPY_PY_TIME_TIME_TIME_NS             (0)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME  (0)
#define MICROPY_PY_BUILTINS_HELP                 (0)
#define MICROPY_PY_BUILTINS_HELP_MODULES         (0)
#define MICROPY_PY_BUILTINS_INPUT                (0)
#define MICROPY_PY_THREAD                        (0)
#define MICROPY_STACK_CHECK_MARGIN               (1024)
#define MICROPY_LONGINT_IMPL                     (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_READER_POSIX                     (1)
#define MICROPY_FLOAT_IMPL                       (MICROPY_FLOAT_IMPL_FLOAT)

#define MICROPY_NLR_SETJMP                       (1)
#define MICROPY_GCREGS_SETJMP                    (1)
#define MICROPY_ERROR_REPORTING                  (MICROPY_ERROR_REPORTING_NORMAL)
#define MICROPY_ENABLE_SCHEDULER                 (1)
#define MICROPY_SCHEDULER_DEPTH                  (8)
#define MICROPY_SCHEDULER_STATIC_NODES           (1)
#define MICROPY_KBD_EXCEPTION                    (1)
#define MICROPY_PY_SYS_PLATFORM                  "yx-gateway"
#define MICROPY_HW_BOARD_NAME                    "yx-gateway"
#define MICROPY_HW_MCU_NAME                      "ESP32-S3"

#define MICROPY_EVENT_POLL_HOOK                                             \
    do {                                                                    \
        mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);      \
    } while (0);

typedef long mp_off_t;

#define MP_STATE_PORT MP_STATE_VM
#define MICROPY_MPHALPORT_H "mphalport.h"
