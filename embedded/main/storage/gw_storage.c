#include "gw_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "gw_defaults.h"
#include "gw_param.h"

#define GW_STORAGE_PACKET_MAGIC_0   'Y'
#define GW_STORAGE_PACKET_MAGIC_1   'X'
#define GW_STORAGE_PACKET_MAGIC_2   'S'
#define GW_STORAGE_PACKET_MAGIC_3   '1'
#define GW_STORAGE_PACKET_VERSION_V1 1U
#define GW_STORAGE_PACKET_VERSION_V2 2U
#define GW_STORAGE_PACKET_VERSION    GW_STORAGE_PACKET_VERSION_V2
#define GW_STORAGE_AES_KEY_BITS     256U
#define GW_STORAGE_SHA256_LEN       32U
#define GW_STORAGE_CTR_LEN          16U

static const char *TAG = "gw_storage";

typedef struct {
    uint8_t magic[4];
    uint8_t version;
    uint8_t type;
    uint16_t reserved;
    uint32_t plain_len;
    uint32_t crc32;
} gw_storage_packet_header_v1_t;

/**
 * @brief V2 安全记录头。
 *
 * generation 用于比较主、副副本的新旧。V1 记录没有该字段，读取时按 generation=0
 * 处理；任意一次后续写入都会自然迁移为 V2。
 */
typedef struct {
    gw_storage_packet_header_v1_t base;
    uint64_t generation;
} gw_storage_packet_header_v2_t;

typedef struct {
    uint8_t *packet;
    const uint8_t *payload;
    size_t packet_len;
    uint32_t plain_len;
    uint64_t generation;
} gw_storage_decoded_packet_t;

_Static_assert(sizeof(gw_storage_packet_header_v1_t) == 16U,
               "unexpected secure storage V1 header size");
_Static_assert(sizeof(gw_storage_packet_header_v2_t) == 24U,
               "unexpected secure storage V2 header size");

static gw_storage_identity_t s_identity;
static SemaphoreHandle_t s_secure_storage_lock;

static esp_err_t secure_storage_lock(void)
{
    if (s_secure_storage_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(s_secure_storage_lock, portMAX_DELAY) == pdTRUE
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void secure_storage_unlock(void)
{
    if (s_secure_storage_lock != NULL) {
        (void)xSemaphoreGive(s_secure_storage_lock);
    }
}

static esp_err_t read_mac_upper(char out[13])
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        return err;
    }

    snprintf(out,
             13,
             "%02X%02X%02X%02X%02X%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
    return ESP_OK;
}

static esp_err_t sha256_segments(const uint8_t *const *segments,
                                 const size_t *lengths,
                                 size_t count,
                                 uint8_t out[GW_STORAGE_SHA256_LEN])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) {
        return ESP_FAIL;
    }

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    int rc = mbedtls_md_setup(&ctx, info, 0);
    if (rc == 0) {
        rc = mbedtls_md_starts(&ctx);
    }

    for (size_t i = 0; rc == 0 && i < count; ++i) {
        rc = mbedtls_md_update(&ctx, segments[i], lengths[i]);
    }

    if (rc == 0) {
        rc = mbedtls_md_finish(&ctx, out);
    }

    mbedtls_md_free(&ctx);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t derive_master_key(uint8_t out[GW_STORAGE_SHA256_LEN])
{
    const char *prefix = "yx_gateway_nvs_aes_v1:";
    const uint8_t *segments[] = {
        (const uint8_t *)prefix,
        (const uint8_t *)s_identity.mac,
    };
    const size_t lengths[] = {
        strlen(prefix),
        strlen(s_identity.mac),
    };

    return sha256_segments(segments, lengths, 2, out);
}

static esp_err_t derive_field_key(const char *ns,
                                  const char *key,
                                  uint8_t out[GW_STORAGE_SHA256_LEN])
{
    uint8_t master_key[GW_STORAGE_SHA256_LEN];
    ESP_RETURN_ON_ERROR(derive_master_key(master_key), TAG, "derive master key failed");

    const char *sep = ":";
    const uint8_t *segments[] = {
        master_key,
        (const uint8_t *)sep,
        (const uint8_t *)ns,
        (const uint8_t *)sep,
        (const uint8_t *)key,
    };
    const size_t lengths[] = {
        sizeof(master_key),
        strlen(sep),
        strlen(ns),
        strlen(sep),
        strlen(key),
    };

    return sha256_segments(segments, lengths, 5, out);
}

static esp_err_t derive_ctr(const char *ns, const char *key, uint8_t out[GW_STORAGE_CTR_LEN])
{
    const char *prefix = "yx_gateway_nvs_ctr_v1:";
    const char *sep = ":";
    uint8_t hash[GW_STORAGE_SHA256_LEN];
    const uint8_t *segments[] = {
        (const uint8_t *)prefix,
        (const uint8_t *)s_identity.mac,
        (const uint8_t *)sep,
        (const uint8_t *)ns,
        (const uint8_t *)sep,
        (const uint8_t *)key,
    };
    const size_t lengths[] = {
        strlen(prefix),
        strlen(s_identity.mac),
        strlen(sep),
        strlen(ns),
        strlen(sep),
        strlen(key),
    };

    ESP_RETURN_ON_ERROR(sha256_segments(segments, lengths, 6, hash), TAG, "derive ctr hash failed");
    memcpy(out, hash, GW_STORAGE_CTR_LEN);
    return ESP_OK;
}

static esp_err_t crypt_packet(const char *ns, const char *key, uint8_t *data, size_t len)
{
    uint8_t field_key[GW_STORAGE_SHA256_LEN];
    uint8_t nonce_counter[GW_STORAGE_CTR_LEN];
    uint8_t stream_block[GW_STORAGE_CTR_LEN] = {0};
    size_t nc_off = 0;

    ESP_RETURN_ON_ERROR(derive_field_key(ns, key, field_key), TAG, "derive field key failed");
    ESP_RETURN_ON_ERROR(derive_ctr(ns, key, nonce_counter), TAG, "derive ctr failed");

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int rc = mbedtls_aes_setkey_enc(&aes, field_key, GW_STORAGE_AES_KEY_BITS);
    if (rc == 0) {
        rc = mbedtls_aes_crypt_ctr(&aes, len, &nc_off, nonce_counter, stream_block, data, data);
    }
    mbedtls_aes_free(&aes);

    return rc == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t make_backup_key(const char *key, char out[GW_NVS_KEY_MAX_BYTES])
{
    if (key == NULL || key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t key_len = strlen(key);
    if (key_len > GW_NVS_BACKED_KEY_MAX_CHARS) {
        ESP_LOGE(TAG,
                 "nvs key too long for backup suffix: key=%s len=%u max=%u",
                 key,
                 (unsigned int)key_len,
                 (unsigned int)GW_NVS_BACKED_KEY_MAX_CHARS);
        return ESP_ERR_INVALID_ARG;
    }

    int written = snprintf(out, GW_NVS_KEY_MAX_BYTES, "%s%s", key, GW_NVS_BACKUP_SUFFIX);
    if (written < 0 || written >= (int)GW_NVS_KEY_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static void decoded_packet_free(gw_storage_decoded_packet_t *decoded)
{
    if (decoded == NULL) {
        return;
    }
    free(decoded->packet);
    memset(decoded, 0, sizeof(*decoded));
}

/**
 * @brief 解析已经解密的 V1/V2 记录头。
 *
 * V1 没有 generation，按 0 处理。记录长度必须和头内 plain_len 严格一致，
 * 避免尾随垃圾被当成有效记录。
 */
static esp_err_t parse_packet_header(const uint8_t *packet,
                                     size_t packet_len,
                                     gw_storage_value_type_t expected_type,
                                     size_t *out_header_len,
                                     uint32_t *out_plain_len,
                                     uint32_t *out_crc32,
                                     uint64_t *out_generation)
{
    if (packet == NULL || out_header_len == NULL || out_plain_len == NULL ||
        out_crc32 == NULL || out_generation == NULL ||
        packet_len < sizeof(gw_storage_packet_header_v1_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const gw_storage_packet_header_v1_t *base =
        (const gw_storage_packet_header_v1_t *)packet;
    if (base->magic[0] != GW_STORAGE_PACKET_MAGIC_0 ||
        base->magic[1] != GW_STORAGE_PACKET_MAGIC_1 ||
        base->magic[2] != GW_STORAGE_PACKET_MAGIC_2 ||
        base->magic[3] != GW_STORAGE_PACKET_MAGIC_3) {
        return ESP_ERR_INVALID_CRC;
    }
    if (base->type != (uint8_t)expected_type) {
        return ESP_ERR_NVS_TYPE_MISMATCH;
    }

    size_t header_len;
    uint64_t generation;
    if (base->version == GW_STORAGE_PACKET_VERSION_V1) {
        header_len = sizeof(gw_storage_packet_header_v1_t);
        generation = 0U;
    } else if (base->version == GW_STORAGE_PACKET_VERSION_V2) {
        if (packet_len < sizeof(gw_storage_packet_header_v2_t)) {
            return ESP_ERR_INVALID_SIZE;
        }
        const gw_storage_packet_header_v2_t *v2 =
            (const gw_storage_packet_header_v2_t *)packet;
        if (v2->generation == 0U) {
            return ESP_ERR_INVALID_CRC;
        }
        header_len = sizeof(gw_storage_packet_header_v2_t);
        generation = v2->generation;
    } else {
        return ESP_ERR_INVALID_VERSION;
    }

    if (base->plain_len > SIZE_MAX - header_len ||
        header_len + (size_t)base->plain_len != packet_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_header_len = header_len;
    *out_plain_len = base->plain_len;
    *out_crc32 = base->crc32;
    *out_generation = generation;
    return ESP_OK;
}

static esp_err_t nvs_write_encrypted_blob(const char *ns,
                                          const char *key,
                                          gw_storage_value_type_t type,
                                          const void *data,
                                          size_t len,
                                          uint64_t generation)
{
    if (ns == NULL || key == NULL || data == NULL || len == 0 || generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len > UINT32_MAX || len > SIZE_MAX - sizeof(gw_storage_packet_header_v2_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t packet_len = sizeof(gw_storage_packet_header_v2_t) + len;
    uint8_t *packet = (uint8_t *)calloc(1, packet_len);
    if (packet == NULL) {
        return ESP_ERR_NO_MEM;
    }

    gw_storage_packet_header_v2_t *header = (gw_storage_packet_header_v2_t *)packet;
    header->base.magic[0] = GW_STORAGE_PACKET_MAGIC_0;
    header->base.magic[1] = GW_STORAGE_PACKET_MAGIC_1;
    header->base.magic[2] = GW_STORAGE_PACKET_MAGIC_2;
    header->base.magic[3] = GW_STORAGE_PACKET_MAGIC_3;
    header->base.version = GW_STORAGE_PACKET_VERSION;
    header->base.type = (uint8_t)type;
    header->base.reserved = 0;
    header->base.plain_len = (uint32_t)len;
    header->base.crc32 = esp_crc32_le(0, (const uint8_t *)data, len);
    header->generation = generation;
    memcpy(packet + sizeof(*header), data, len);

    esp_err_t err = crypt_packet(ns, key, packet, packet_len);
    if (err != ESP_OK) {
        free(packet);
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, key, packet, packet_len);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }

    free(packet);
    return err;
}

static esp_err_t nvs_load_encrypted_blob(const char *ns,
                                         const char *key,
                                         gw_storage_value_type_t expected_type,
                                         gw_storage_decoded_packet_t *out)
{
    if (ns == NULL || key == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t packet_len = 0;
    err = nvs_get_blob(handle, key, NULL, &packet_len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    if (packet_len < sizeof(gw_storage_packet_header_v1_t)) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *packet = (uint8_t *)malloc(packet_len);
    if (packet == NULL) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_blob(handle, key, packet, &packet_len);
    nvs_close(handle);
    if (err != ESP_OK) {
        free(packet);
        return err;
    }

    err = crypt_packet(ns, key, packet, packet_len);
    if (err != ESP_OK) {
        free(packet);
        return err;
    }

    size_t header_len = 0U;
    uint32_t plain_len = 0U;
    uint32_t expected_crc = 0U;
    uint64_t generation = 0U;
    err = parse_packet_header(packet,
                              packet_len,
                              expected_type,
                              &header_len,
                              &plain_len,
                              &expected_crc,
                              &generation);
    if (err != ESP_OK) {
        free(packet);
        return err;
    }

    const uint8_t *payload = packet + header_len;
    uint32_t actual_crc = esp_crc32_le(0, payload, plain_len);
    if (actual_crc != expected_crc) {
        free(packet);
        return ESP_ERR_INVALID_CRC;
    }

    out->packet = packet;
    out->payload = payload;
    out->packet_len = packet_len;
    out->plain_len = plain_len;
    out->generation = generation;
    return ESP_OK;
}

/** @brief 判断错误是否已经明确表示该副本不存在或内容不可用。 */
static bool record_unusable_error(esp_err_t err)
{
    return err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_INVALID_CRC ||
           err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_INVALID_VERSION ||
           err == ESP_ERR_NVS_TYPE_MISMATCH || err == ESP_ERR_NVS_INVALID_LENGTH;
}

/**
 * @brief 读取主、副副本 generation 并分配下一序号。
 *
 * 损坏副本不参与比较；内存不足、NVS I/O 等瞬时错误必须向上传递，不能在无法确认
 * 现有最大序号时从 1 重新开始。
 */
static esp_err_t allocate_next_generation(const char *ns,
                                          const char *key,
                                          const char *backup_key,
                                          gw_storage_value_type_t type,
                                          uint64_t *out_generation)
{
    if (ns == NULL || key == NULL || backup_key == NULL || out_generation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gw_storage_decoded_packet_t main_record = {0};
    gw_storage_decoded_packet_t backup_record = {0};
    esp_err_t main_err = nvs_load_encrypted_blob(ns, key, type, &main_record);
    esp_err_t backup_err = nvs_load_encrypted_blob(ns, backup_key, type, &backup_record);

    if (main_err != ESP_OK && !record_unusable_error(main_err)) {
        decoded_packet_free(&backup_record);
        return main_err;
    }
    if (backup_err != ESP_OK && !record_unusable_error(backup_err)) {
        decoded_packet_free(&main_record);
        return backup_err;
    }

    uint64_t max_generation = 0U;
    if (main_err == ESP_OK) {
        max_generation = main_record.generation;
    }
    if (backup_err == ESP_OK && backup_record.generation > max_generation) {
        max_generation = backup_record.generation;
    }
    decoded_packet_free(&main_record);
    decoded_packet_free(&backup_record);

    if (max_generation == UINT64_MAX) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_generation = max_generation + 1U;
    return ESP_OK;
}

esp_err_t gw_storage_secure_set_blob(const char *ns,
                                     const char *key,
                                     gw_storage_value_type_t type,
                                     const void *data,
                                     size_t len)
{
    if (ns == NULL || key == NULL || data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = secure_storage_lock();
    if (err != ESP_OK) {
        return err;
    }

    char backup_key[GW_NVS_KEY_MAX_BYTES];
    err = make_backup_key(key, backup_key);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backup key invalid: %s", esp_err_to_name(err));
        secure_storage_unlock();
        return err;
    }

    uint64_t generation = 0U;
    err = allocate_next_generation(ns, key, backup_key, type, &generation);
    if (err == ESP_OK) {
        err = nvs_write_encrypted_blob(ns,
                                       backup_key,
                                       type,
                                       data,
                                       len,
                                       generation);
    }
    if (err == ESP_OK) {
        /*
         * 备份记录已经独立 commit 后，新 generation 就成为读取路径认定的事实值。
         * 主记录只是第二份冗余副本：若它写入失败仍返回错误，调用方会回滚 RAM，
         * 但重启又会从较新的备份恢复新值，造成内存与持久态分裂。因此这里把事务
         * 提交点定义在备份 commit；主记录失败只记录降级，后续成功写入会自然修复。
         */
        esp_err_t main_err =
            nvs_write_encrypted_blob(ns, key, type, data, len, generation);
        if (main_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "secure value committed to backup but main copy degraded: ns=%s key=%s err=%s",
                     ns,
                     key,
                     esp_err_to_name(main_err));
        }
    }
    secure_storage_unlock();
    return err;
}

esp_err_t gw_storage_secure_get_blob(const char *ns,
                                     const char *key,
                                     gw_storage_value_type_t expected_type,
                                     void *out,
                                     size_t out_size,
                                     size_t *out_len)
{
    if (ns == NULL || key == NULL || out == NULL || out_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t lock_err = secure_storage_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }

    char backup_key[GW_NVS_KEY_MAX_BYTES];
    esp_err_t key_err = make_backup_key(key, backup_key);
    if (key_err != ESP_OK) {
        secure_storage_unlock();
        return key_err;
    }

    gw_storage_decoded_packet_t main_record = {0};
    gw_storage_decoded_packet_t backup_record = {0};
    esp_err_t main_err = nvs_load_encrypted_blob(ns, key, expected_type, &main_record);
    esp_err_t backup_err =
        nvs_load_encrypted_blob(ns, backup_key, expected_type, &backup_record);

    /*
     * 若某个副本发生瞬时读取错误，不能退回另一个可能更旧的副本；只有明确缺失或
     * 校验失败时才允许降级。这样 generation 比较始终建立在完整读到两个候选之上。
     */
    if (main_err != ESP_OK && !record_unusable_error(main_err)) {
        decoded_packet_free(&backup_record);
        secure_storage_unlock();
        return main_err;
    }
    if (backup_err != ESP_OK && !record_unusable_error(backup_err)) {
        decoded_packet_free(&main_record);
        secure_storage_unlock();
        return backup_err;
    }

    const gw_storage_decoded_packet_t *selected = NULL;
    if (main_err == ESP_OK && backup_err == ESP_OK) {
        /*
         * V1 的 generation 都为 0。旧写入顺序是“副本后主记录”，所以两份有效但
         * 内容不同时副本不会比主记录旧；相同 generation 也优先副本可恢复旧格式
         * 在写完副本、尚未写主记录时掉电的场景。
         */
        selected = backup_record.generation >= main_record.generation
                       ? &backup_record
                       : &main_record;
    } else if (main_err == ESP_OK) {
        selected = &main_record;
    } else if (backup_err == ESP_OK) {
        selected = &backup_record;
    }

    if (selected == NULL) {
        decoded_packet_free(&main_record);
        decoded_packet_free(&backup_record);
        secure_storage_unlock();
        if (backup_err == ESP_ERR_NVS_NOT_FOUND &&
            main_err != ESP_ERR_NVS_NOT_FOUND) {
            /* 主记录存在但损坏时，备份缺失不能伪装成“从未配置”。 */
            return main_err;
        }
        return main_err != ESP_ERR_NVS_NOT_FOUND ? main_err : backup_err;
    }
    if (selected->plain_len > out_size) {
        decoded_packet_free(&main_record);
        decoded_packet_free(&backup_record);
        secure_storage_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out, selected->payload, selected->plain_len);
    if (out_len != NULL) {
        *out_len = selected->plain_len;
    }
    decoded_packet_free(&main_record);
    decoded_packet_free(&backup_record);
    secure_storage_unlock();
    return ESP_OK;
}

esp_err_t gw_storage_secure_set_string(const char *ns, const char *key, const char *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return gw_storage_secure_set_blob(ns,
                                      key,
                                      GW_STORAGE_VALUE_STRING,
                                      value,
                                      strlen(value) + 1U);
}

esp_err_t gw_storage_secure_get_string(const char *ns, const char *key, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t out_len = 0;
    esp_err_t err = gw_storage_secure_get_blob(ns,
                                               key,
                                               GW_STORAGE_VALUE_STRING,
                                               out,
                                               out_size,
                                               &out_len);
    if (err != ESP_OK) {
        return err;
    }

    if (out_len == 0U || out_len > out_size || out[out_len - 1U] != '\0' ||
        memchr(out, '\0', out_len - 1U) != NULL) {
        out[0] = '\0';
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

esp_err_t gw_storage_secure_erase_key(const char *ns, const char *key)
{
    if (ns == NULL || key == NULL || key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = secure_storage_lock();
    if (err != ESP_OK) {
        return err;
    }

    char backup_key[GW_NVS_KEY_MAX_BYTES];
    err = make_backup_key(key, backup_key);
    if (err != ESP_OK) {
        secure_storage_unlock();
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(ns, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        secure_storage_unlock();
        return ESP_OK;
    }
    if (err != ESP_OK) {
        secure_storage_unlock();
        return err;
    }

    esp_err_t main_err = nvs_erase_key(handle, key);
    esp_err_t backup_err = nvs_erase_key(handle, backup_key);
    if (main_err == ESP_ERR_NVS_NOT_FOUND) {
        main_err = ESP_OK;
    }
    if (backup_err == ESP_ERR_NVS_NOT_FOUND) {
        backup_err = ESP_OK;
    }

    err = (main_err != ESP_OK) ? main_err : backup_err;
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    secure_storage_unlock();
    return err;
}

static esp_err_t ensure_string_param(const char *ns,
                                     const char *key,
                                     const char *default_value,
                                     char *out,
                                     size_t out_size)
{
    esp_err_t err = gw_storage_secure_get_string(ns, key, out, out_size);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_INVALID_CRC &&
        err != ESP_ERR_INVALID_SIZE && err != ESP_ERR_INVALID_VERSION &&
        err != ESP_ERR_INVALID_RESPONSE && err != ESP_ERR_NVS_TYPE_MISMATCH &&
        err != ESP_ERR_NVS_INVALID_LENGTH) {
        return err;
    }

    ESP_RETURN_ON_ERROR(gw_storage_secure_set_string(ns, key, default_value),
                        TAG,
                        "write default string failed");

    strlcpy(out, default_value, out_size);
    return ESP_OK;
}

esp_err_t gw_storage_init(void)
{
    memset(&s_identity, 0, sizeof(s_identity));

    ESP_RETURN_ON_ERROR(read_mac_upper(s_identity.mac), TAG, "read mac failed");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /*
         * 生产设备的 schedule、可信时间恢复锁和身份参数共用该 NVS 分区。初始化
         * 异常时自动整区擦除会把可诊断故障扩大成不可恢复的数据丢失，因此失败关闭，
         * 仅允许显式维护/恢复出厂流程决定是否擦除。
         */
        ESP_LOGE(TAG,
                 "nvs init requires explicit recovery; refuse automatic erase: %s",
                 esp_err_to_name(err));
        return err;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "init nvs failed");

    if (s_secure_storage_lock == NULL) {
        s_secure_storage_lock = xSemaphoreCreateMutex();
        if (s_secure_storage_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_RETURN_ON_ERROR(ensure_string_param(GW_NVS_NAMESPACE_FACTORY,
                                            GW_PARAM_KEY_PRODUCT_ID,
                                            GW_PRODUCT_ID_DEFAULT,
                                            s_identity.product_id,
                                            sizeof(s_identity.product_id)),
                        TAG,
                        "ensure product_id failed");

    ESP_RETURN_ON_ERROR(ensure_string_param(GW_NVS_NAMESPACE_FACTORY,
                                            GW_PARAM_KEY_GROUP_ID,
                                            GW_GROUP_ID_DEFAULT,
                                            s_identity.group_id,
                                            sizeof(s_identity.group_id)),
                        TAG,
                        "ensure group_id failed");

    /*
     * 当前协议文档规定设备管理 Topic 固定使用 yx_gw 组。旧版本固件可能已经
     * 在 NVS 中写入过其他 group_id，这里在启动时做一次轻量同步，避免设备订阅
     * 到历史 Topic 后无法通过平台 ACL。
     */
    if (strcmp(s_identity.group_id, GW_GROUP_ID_DEFAULT) != 0) {
        ESP_LOGW(TAG,
                 "factory group_id mismatch: old=%s new=%s",
                 s_identity.group_id,
                 GW_GROUP_ID_DEFAULT);
        ESP_RETURN_ON_ERROR(gw_storage_secure_set_string(GW_NVS_NAMESPACE_FACTORY,
                                                         GW_PARAM_KEY_GROUP_ID,
                                                         GW_GROUP_ID_DEFAULT),
                            TAG,
                            "update fixed group_id failed");
        strlcpy(s_identity.group_id, GW_GROUP_ID_DEFAULT, sizeof(s_identity.group_id));
    }

    ESP_RETURN_ON_ERROR(ensure_string_param(GW_NVS_NAMESPACE_FACTORY,
                                            GW_PARAM_KEY_DEVICE_ID,
                                            s_identity.mac,
                                            s_identity.device_id,
                                            sizeof(s_identity.device_id)),
                        TAG,
                        "ensure device_id failed");

    /*
     * 当前业务协议规定 device_id 必须等于 raw MAC。旧固件可能保存过 MAC+"01"
     * 或其他历史值；启动升级时统一迁移，避免 BLE 身份与 HTTPS/MQTT 业务身份分裂。
     */
    if (strcmp(s_identity.device_id, s_identity.mac) != 0) {
        ESP_LOGW(TAG,
                 "factory device_id migrated to raw mac: old=%s new=%s",
                 s_identity.device_id,
                 s_identity.mac);
        ESP_RETURN_ON_ERROR(gw_storage_secure_set_string(GW_NVS_NAMESPACE_FACTORY,
                                                         GW_PARAM_KEY_DEVICE_ID,
                                                         s_identity.mac),
                            TAG,
                            "migrate device_id to raw mac failed");
        strlcpy(s_identity.device_id, s_identity.mac, sizeof(s_identity.device_id));
    }

    /* 只有身份参数全部读取/迁移成功后，才对其他模块公开“存储已就绪”。 */
    s_identity.nvs_ready = true;
    ESP_LOGI(TAG,
             "storage identity ready: mac=%s product_id=%s group_id=%s device_id=%s",
             s_identity.mac,
             s_identity.product_id,
             s_identity.group_id,
             s_identity.device_id);
    return ESP_OK;
}

esp_err_t gw_storage_get_identity(gw_storage_identity_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_identity.nvs_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    *out = s_identity;
    return ESP_OK;
}
