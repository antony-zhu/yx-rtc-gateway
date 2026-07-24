#pragma once

#include "esp_err.h"
#include "py/obj.h"

#include "gw_py_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 绑定 Python waker 回调。
 *
 * callback 必须是可调用对象。该函数只能在 MicroPython VM 上下文调用。
 */
esp_err_t gw_py_bridge_bind(gw_py_bridge_source_t source, mp_obj_t callback, uint32_t *out_generation);

/**
 * @brief 解绑 Python waker 回调。
 *
 * 该函数只能在 MicroPython VM 上下文调用。
 */
esp_err_t gw_py_bridge_unbind(gw_py_bridge_source_t source);

#ifdef __cplusplus
}
#endif
