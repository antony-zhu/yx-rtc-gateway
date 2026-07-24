#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 固件版本号固定使用 3 个字节表达：major.minor.patch。
 * 当前 P0 骨架版本从 0.1.0 起步，后续发布版本只允许递增。
 */

#define GW_FIRMWARE_VERSION_MAJOR   0U
#define GW_FIRMWARE_VERSION_MINOR   1U
#define GW_FIRMWARE_VERSION_PATCH   0U

#define GW_FIRMWARE_VERSION_STRING  "0.1.0"

/**
 * @brief 获取固件版本字符串。
 *
 * @return 固定格式 "major.minor.patch" 的静态字符串。
 */
static inline const char *gw_version_get_string(void)
{
    return GW_FIRMWARE_VERSION_STRING;
}

#ifdef __cplusplus
}
#endif
