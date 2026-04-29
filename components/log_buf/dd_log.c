// In-RAM ring buffer that captures ESP_LOG output for the Web UI / curl
// debugging — the device exposes the tail at /api/system/logs.
//
// We register a vprintf hook via esp_log_set_vprintf. ESP-IDF's logging path
// serialises calls to that hook (no concurrent invocations from different
// tasks), but we still take a mutex around buffer mutation so dd_log_read_tail
// from an HTTP handler doesn't race with a logger task in flight.
//
// The hook re-emits to the original UART vprintf so `make monitor` still
// shows the same stream — capture is purely additive.

#include "dd_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LOG_LINE_MAX 256

static char           s_buf[DD_LOG_BUF_BYTES];
static size_t         s_head;          // next-write offset
static size_t         s_used;          // bytes currently held (≤ DD_LOG_BUF_BYTES)
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_orig;          // forward to UART so make monitor still works
static bool           s_inited;

static void buf_write(const char *src, size_t n)
{
    if (n == 0) return;
    if (n >= DD_LOG_BUF_BYTES) {
        // Single line bigger than the whole buffer: keep the tail.
        src += n - DD_LOG_BUF_BYTES;
        n = DD_LOG_BUF_BYTES;
        memcpy(s_buf, src, n);
        s_head = 0;
        s_used = DD_LOG_BUF_BYTES;
        return;
    }
    size_t first = DD_LOG_BUF_BYTES - s_head;
    if (n <= first) {
        memcpy(s_buf + s_head, src, n);
    } else {
        memcpy(s_buf + s_head, src, first);
        memcpy(s_buf, src + first, n - first);
    }
    s_head = (s_head + n) % DD_LOG_BUF_BYTES;
    s_used = (s_used + n > DD_LOG_BUF_BYTES) ? DD_LOG_BUF_BYTES : s_used + n;
}

static int log_vprintf(const char *fmt, va_list args)
{
    // Forward to original first — we want the UART stream to be the source of
    // truth even if our buffer mutex is briefly contended.
    va_list copy;
    va_copy(copy, args);
    int forwarded = s_orig ? s_orig(fmt, copy) : 0;
    va_end(copy);

    char line[LOG_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n < 0) return forwarded;
    if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;  // truncated

    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        buf_write(line, (size_t)n);
        xSemaphoreGive(s_lock);
    }
    return forwarded;
}

esp_err_t dd_log_init(void)
{
    if (s_inited) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_orig = esp_log_set_vprintf(log_vprintf);
    s_inited = true;
    return ESP_OK;
}

size_t dd_log_read_tail(char *out, size_t cap)
{
    if (!out || cap == 0 || !s_lock) return 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return 0;

    size_t take = (s_used < cap) ? s_used : cap;
    // The most-recent `take` bytes end at s_head and walk backward.
    size_t start = (s_head + DD_LOG_BUF_BYTES - take) % DD_LOG_BUF_BYTES;
    size_t first = DD_LOG_BUF_BYTES - start;
    if (take <= first) {
        memcpy(out, s_buf + start, take);
    } else {
        memcpy(out, s_buf + start, first);
        memcpy(out + first, s_buf, take - first);
    }
    xSemaphoreGive(s_lock);
    return take;
}

void dd_log_clear(void)
{
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    s_head = 0;
    s_used = 0;
    xSemaphoreGive(s_lock);
}
