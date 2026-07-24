#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 对日志或异常文本中的常见敏感字段做轻量脱敏。
 *
 * 当前规则覆盖 password、passwd、pwd、token、secret、key、authorization、
 * signature、sign 以及 HTTP/HTTPS URL query。该接口只保证输出不会超出
 * out_size，并尽量保留原始上下文，供日志、脚本异常和诊断摘要使用。
 *
 * @param input 原始文本，可为空。
 * @param out 输出缓冲区，不能为空。
 * @param out_size 输出缓冲区大小，必须大于 0。
 * @return ESP_OK 表示处理完成。
 */
esp_err_t gw_redact_sensitive_text(const char *input, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
