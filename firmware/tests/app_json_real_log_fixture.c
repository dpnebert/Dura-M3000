/* Test-only extracted ESP-IDF v6.0.2 logging implementation.
 * Source: components/log/src/buffer/log_buffers.c; SPDX-License-Identifier: Apache-2.0
 * SDK functions verified in candidate39 independent-app re-review.
 * Router below is always freshly generated from app_main.c by the test runner. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define BYTES_PER_LINE 16
#define ESP_LOG_MODE_BINARY_EN 0
#define ESP_LOG_LEVEL(...) ((void)0)
static int esp_log_util_cvt_hex(unsigned value,int width,char*out){(void)width;return sprintf(out,"%02x",value);}
typedef int esp_log_level_t;
typedef void (*print_line_t)(uintptr_t,const void*,char*,int);
static bool esp_ptr_byte_accessible(const void*p){(void)p;return true;}
__attribute__((unused)) static void log_buffer_hex_line(uintptr_t orig_buff, const void *ptr_line, char *output_str, int buff_len)
{
    (void) orig_buff;
    const unsigned char *ptr = (unsigned char *)ptr_line;
    int i;
    for (i = 0; i < buff_len - 1; i++) {
        output_str += esp_log_util_cvt_hex(ptr[i], 2, output_str);
        *output_str++ = ' ';
    }
    output_str += esp_log_util_cvt_hex(ptr[i], 2, output_str);
    *output_str = '\0';
}
__attribute__((unused)) static void print_buffer(const char *tag, const void *buffer, uint16_t buff_len, esp_log_level_t log_level, char *output_str, print_line_t print_line_func)
{
    if (buff_len == 0) {
        return;
    }
    char temp_buffer[BYTES_PER_LINE + 3]; //for not-byte-accessible memory

    do {
        const char *ptr_line = buffer;
        int bytes_cur_line = MIN(BYTES_PER_LINE, buff_len);
        if (!esp_ptr_byte_accessible(buffer)) {
            //use memcpy to get around alignment issue
            memcpy(temp_buffer, buffer, (bytes_cur_line + 3) / 4 * 4);
            ptr_line = temp_buffer;
        }

        print_line_func((uintptr_t)buffer, ptr_line, output_str, bytes_cur_line);

        ESP_LOG_LEVEL(log_level, tag, "%s", output_str);
        buffer += bytes_cur_line;
        buff_len -= bytes_cur_line;
    } while (buff_len);
}
void esp_log_buffer_hex_internal(const char *tag, const void *buffer, uint16_t buff_len, esp_log_level_t log_level)
{
    // I (954) log_example: 54 68 65 20 77 61 79 20 74 6f 20 67 65 74 20 73
    // I (962) log_example: 74 61 72 74 65 64 20 69 73 20 74 6f 20 71 75 69
    //                      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
#if ESP_LOG_MODE_BINARY_EN
    esp_log(ESP_LOG_CONFIG_INIT(log_level | ESP_LOG_CONFIGS_DEFAULT), tag, __ESP_BUFFER_HEX_FORMAT__ ESP_LOG_ARGS(buff_len, (const char *)buffer, (void *)buffer));
#else
    char output_str[3 * BYTES_PER_LINE];
    print_buffer(tag, buffer, buff_len, log_level, output_str, log_buffer_hex_line);
#endif
}
#include <stdbool.h>
