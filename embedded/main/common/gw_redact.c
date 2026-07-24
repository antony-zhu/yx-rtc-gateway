#include "gw_redact.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static const char *const SENSITIVE_KEYS[] = {
    "password",
    "passwd",
    "pwd",
    "token",
    "access_token",
    "refresh_token",
    "secret",
    "device_secret",
    "key",
    "api_key",
    "authorization",
    "signature",
    "sign",
};

static bool append_char(char *out, size_t out_size, size_t *pos, char ch)
{
    if (out == NULL || pos == NULL || *pos + 1U >= out_size) {
        return false;
    }
    out[*pos] = ch;
    ++(*pos);
    out[*pos] = '\0';
    return true;
}

static bool append_bytes(char *out, size_t out_size, size_t *pos, const char *text, size_t len)
{
    if (out == NULL || pos == NULL || text == NULL || *pos >= out_size) {
        return false;
    }
    size_t room = out_size - *pos - 1U;
    if (len > room) {
        len = room;
    }
    if (len > 0U) {
        memcpy(out + *pos, text, len);
        *pos += len;
        out[*pos] = '\0';
    }
    return len > 0U;
}

static bool append_mask(char *out, size_t out_size, size_t *pos)
{
    return append_bytes(out, out_size, pos, "***", 3U);
}

static bool ascii_ieq_n(const char *a, const char *b, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (tolower(ca) != tolower(cb)) {
            return false;
        }
    }
    return true;
}

static bool is_word_char(char ch)
{
    unsigned char c = (unsigned char)ch;
    return isalnum(c) || ch == '_' || ch == '-';
}

static bool match_url_prefix(const char *text)
{
    return strncmp(text, "http://", 7) == 0 || strncmp(text, "https://", 8) == 0;
}

static size_t match_sensitive_key(const char *text)
{
    for (size_t i = 0; i < sizeof(SENSITIVE_KEYS) / sizeof(SENSITIVE_KEYS[0]); ++i) {
        const char *key = SENSITIVE_KEYS[i];
        size_t key_len = strlen(key);
        if (ascii_ieq_n(text, key, key_len) && !is_word_char(text[key_len])) {
            return key_len;
        }
    }
    return 0U;
}

static bool key_has_boundary(const char *input, const char *pos)
{
    return pos == input || !is_word_char(pos[-1]);
}

static const char *skip_quoted_value(const char *p, char quote)
{
    if (*p == quote) {
        ++p;
    }
    while (*p != '\0') {
        if (*p == '\\' && p[1] != '\0') {
            p += 2;
            continue;
        }
        if (*p == quote) {
            ++p;
            break;
        }
        ++p;
    }
    return p;
}

static bool redact_value(const char *input,
                         const char **cursor,
                         char *out,
                         size_t out_size,
                         size_t *out_pos,
                         bool quoted_key,
                         char quote_char)
{
    const char *start = *cursor;
    const char *p = start;
    size_t key_len = 0U;

    if (quoted_key) {
        if (*p != quote_char) {
            return false;
        }
        ++p;
        key_len = match_sensitive_key(p);
        if (key_len == 0U || p[key_len] != quote_char) {
            return false;
        }
        p += key_len + 1U;
    } else {
        key_len = match_sensitive_key(p);
        if (key_len == 0U || !key_has_boundary(input, p)) {
            return false;
        }
        p += key_len;
    }

    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p != ':' && *p != '=') {
        return false;
    }
    ++p;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }

    append_bytes(out, out_size, out_pos, start, (size_t)(p - start));
    if (*p == '"' || *p == '\'') {
        char value_quote = *p;
        append_char(out, out_size, out_pos, value_quote);
        append_mask(out, out_size, out_pos);
        p = skip_quoted_value(p, value_quote);
        if (p[-1] == value_quote) {
            append_char(out, out_size, out_pos, value_quote);
        }
    } else {
        append_mask(out, out_size, out_pos);
        while (*p != '\0' && *p != '\r' && *p != '\n' &&
               *p != ',' && *p != ';' && *p != '}' && *p != ']' && *p != '&') {
            ++p;
        }
    }

    *cursor = p;
    return true;
}

static bool redact_url_query(const char **cursor, char *out, size_t out_size, size_t *out_pos)
{
    const char *p = *cursor;

    if (!match_url_prefix(p)) {
        return false;
    }
    while (*p != '\0' && *p != '?' && !isspace((unsigned char)*p) &&
           *p != '"' && *p != '\'' && *p != ')' && *p != ']' && *p != '}') {
        append_char(out, out_size, out_pos, *p++);
    }
    if (*p != '?') {
        *cursor = p;
        return true;
    }

    append_bytes(out, out_size, out_pos, "?***", 4U);
    while (*p != '\0' && !isspace((unsigned char)*p) &&
           *p != '"' && *p != '\'' && *p != ')' && *p != ']' && *p != '}') {
        ++p;
    }
    *cursor = p;
    return true;
}

esp_err_t gw_redact_sensitive_text(const char *input, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    out[0] = '\0';
    if (input == NULL || input[0] == '\0') {
        return ESP_OK;
    }

    const char *p = input;
    size_t out_pos = 0U;
    while (*p != '\0' && out_pos + 1U < out_size) {
        const char *before = p;

        if (redact_url_query(&p, out, out_size, &out_pos)) {
            continue;
        }
        if ((*p == '"' || *p == '\'') &&
            redact_value(input, &p, out, out_size, &out_pos, true, *p)) {
            continue;
        }
        if (redact_value(input, &p, out, out_size, &out_pos, false, '\0')) {
            continue;
        }

        p = before;
        append_char(out, out_size, &out_pos, *p++);
    }

    out[out_pos] = '\0';
    return ESP_OK;
}
