#pragma once
/* Bounded streaming JSON-template formatter. Only JSON producers use this;
 * ordinary printf/control paths are deliberately unaffected. Each template
 * fragment starts outside a string. %s inside quotes is escaped, outside quotes
 * accepts only the fixed JSON tokens used by callers. No escaped-value arrays. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_err.h"

static inline esp_err_t dura_json_fail(char *out, size_t cap, int written)
{
    const char *text = written == -2 ?
        "{\"ok\":false,\"error\":{\"code\":\"RESPONSE_FORMAT\"}}" :
        "{\"ok\":false,\"error\":{\"code\":\"RESPONSE_CAPACITY\"}}";
    if (out && cap) {
        if (strlen(text) < cap) memcpy(out, text, strlen(text) + 1);
        else if (cap >= 3) memcpy(out, "{}", 3);
        else out[0] = 0; /* No complete JSON value fits in a one-byte buffer. */
    }
    return written == -2 ? ESP_ERR_INVALID_ARG : ESP_ERR_NO_MEM;
}

static inline void dura_json_byte(char *out, size_t cap, size_t *used, char c)
{
    if (*used < cap && cap - *used > 1) out[*used] = c;
    ++*used;
}

/* Validate output strings only: never normalize or alter accepted/stored state.
 * Check continuation bytes sequentially so a truncated string stops at NUL. */
static inline bool dura_json_utf8_valid(const unsigned char *s)
{
    while (*s) {
        unsigned char c = *s++;
        unsigned remaining, low = 0x80, high = 0xbf;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) remaining = 1;
        else if (c >= 0xe0 && c <= 0xef) {
            remaining = 2;
            if (c == 0xe0) low = 0xa0;  /* No overlong encoding. */
            if (c == 0xed) high = 0x9f; /* No UTF-16 surrogates. */
        } else if (c >= 0xf0 && c <= 0xf4) {
            remaining = 3;
            if (c == 0xf0) low = 0x90;
            if (c == 0xf4) high = 0x8f; /* At most U+10FFFF. */
        } else return false;
        while (remaining--) {
            if (*s < low || *s > high) return false;
            ++s;
            low = 0x80; high = 0xbf;
        }
    }
    return true;
}

static inline int dura_json_snprintf(char *out, size_t cap, const char *fmt, ...)
{
    if (out == NULL || cap == 0 || fmt == NULL) return -2;
    size_t used = 0;
    bool quoted = false, escaped = false;
    int failure = 0;
    va_list ap;
    va_start(ap, fmt);
    while (*fmt && !failure) {
        if (*fmt != '%') {
            char c = *fmt++;
            dura_json_byte(out, cap, &used, c);
            if (c == '"' && !escaped) quoted = !quoted;
            if (c == '\\' && !escaped) escaped = true; else escaped = false;
            continue;
        }
        char spec[24]; size_t n = 0; int longs = 0;
        spec[n++] = *fmt++;
        while (*fmt && strchr("0123456789.-+ #l", *fmt)) {
            if (n + 2 >= sizeof(spec)) { failure = -2; break; }
            if (*fmt == 'l') ++longs;
            spec[n++] = *fmt++;
        }
        if (failure || !*fmt) { failure = -2; break; }
        char type = *fmt++; spec[n++] = type; spec[n] = 0;
        if (type == 's' && n == 2) {
            const unsigned char *s = (const unsigned char *)va_arg(ap, const char *);
            if (!s || (quoted && !dura_json_utf8_valid(s))) { failure = -2; break; }
            if (!quoted && strcmp((const char *)s, "true") && strcmp((const char *)s, "false") &&
                strcmp((const char *)s, "null") && strcmp((const char *)s, ",") && *s) {
                failure = -2; break;
            }
            for (; *s; ++s) {
                if (quoted && (*s == '"' || *s == '\\')) {
                    dura_json_byte(out, cap, &used, '\\');
                    dura_json_byte(out, cap, &used, (char)*s);
                } else if (quoted && *s < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    const char prefix[] = "\\u00";
                    for (size_t i = 0; i < 4; ++i) dura_json_byte(out, cap, &used, prefix[i]);
                    dura_json_byte(out, cap, &used, hex[*s >> 4]);
                    dura_json_byte(out, cap, &used, hex[*s & 15]);
                } else dura_json_byte(out, cap, &used, (char)*s);
            }
        } else {
            char number[96]; int count = -1;
            if (type == 'f') {
                double value = va_arg(ap, double);
                if (!isfinite(value)) { failure = -2; break; }
                count = snprintf(number, sizeof(number), spec, value);
            } else if (type == 'd' || type == 'i') {
                if (longs == 2) count = snprintf(number, sizeof(number), spec, va_arg(ap, long long));
                else if (longs == 1) count = snprintf(number, sizeof(number), spec, va_arg(ap, long));
                else count = snprintf(number, sizeof(number), spec, va_arg(ap, int));
            } else if (type == 'u' || type == 'x' || type == 'X') {
                if (longs == 2) count = snprintf(number, sizeof(number), spec, va_arg(ap, unsigned long long));
                else if (longs == 1) count = snprintf(number, sizeof(number), spec, va_arg(ap, unsigned long));
                else count = snprintf(number, sizeof(number), spec, va_arg(ap, unsigned int));
            } else if (type == '%' && n == 2) { number[0] = '%'; count = 1; }
            if (count < 0) { failure = -2; break; }
            if ((size_t)count >= sizeof(number)) { failure = -1; break; }
            for (int i = 0; i < count; ++i) dura_json_byte(out, cap, &used, number[i]);
        }
        if (used >= cap) failure = -1;
    }
    va_end(ap);
    if (quoted && !failure) failure = -2;
    if (failure || used >= cap) {
        if (out && cap) out[0] = 0;
        return failure ? failure : -1;
    }
    if (out && cap) out[used] = 0;
    return (int)used;
}

static inline esp_err_t dura_json_result(char *out, size_t cap, int written)
{
    return written >= 0 && (size_t)written < cap ? ESP_OK : dura_json_fail(out, cap, written);
}
