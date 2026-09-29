/*
 * log_ring.c - esp_log vprintf 钩子 + 4KB 环形日志缓存
 */
#include "log_ring.h"

#include <stdarg.h>
#include <string.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LOGRING_SIZE 4096

static char                    s_buf[LOGRING_SIZE];
static size_t                  s_head;        /* 下一个写入位置 */
static size_t                  s_len;         /* 已用长度 */
static bool                    s_wrapped;
static SemaphoreHandle_t       s_mtx;
static vprintf_like_t          s_orig;
static portMUX_TYPE            s_fallback_lock = portMUX_INITIALIZER_UNLOCKED;

/* 在持锁/不持锁两种情况下都安全地写入 */
static void ring_put(const char *data, size_t n)
{
    if (n == 0) return;
    if (n > LOGRING_SIZE) {
        data += (n - LOGRING_SIZE);
        n = LOGRING_SIZE;
    }

    if (s_mtx) {
        if (xSemaphoreTake(s_mtx, 0) != pdTRUE) return;   /* 忙则丢弃, 绝不阻塞 */
    }

    size_t first = LOGRING_SIZE - s_head;
    if (first > n) first = n;
    memcpy(&s_buf[s_head], data, first);
    if (n > first) memcpy(s_buf, data + first, n - first);

    s_head = (s_head + n) % LOGRING_SIZE;
    if (s_len + n >= LOGRING_SIZE) {
        s_len = LOGRING_SIZE;
        s_wrapped = true;
    } else {
        s_len += n;
    }

    if (s_mtx) xSemaphoreGive(s_mtx);
}

static int log_hook(const char *fmt, va_list ap)
{
    /* 保持原有输出行为(控制台) */
    va_list cp;
    va_copy(cp, ap);
    int ret = s_orig ? s_orig(fmt, cp) : 0;
    va_end(cp);

    char tmp[256];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (n > 0) {
        if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
        ring_put(tmp, (size_t)n);
    }
    return ret;
}

void log_ring_init(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    s_len = 0;
    s_head = 0;
    s_wrapped = false;
    memset(s_buf, 0, sizeof(s_buf));

    s_orig = esp_log_set_vprintf(log_hook);
    (void)s_fallback_lock;
}

size_t log_ring_dump(char *out, size_t max)
{
    if (!out || max == 0) return 0;

    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);

    size_t n = 0;
    if (s_wrapped) {
        /* 最旧数据从 s_head 开始 */
        size_t first = LOGRING_SIZE - s_head;
        for (size_t i = 0; i < first && n < max - 1; i++) out[n++] = s_buf[s_head + i];
        for (size_t i = 0; i < s_head && n < max - 1; i++) out[n++] = s_buf[i];
    } else {
        for (size_t i = 0; i < s_len && n < max - 1; i++) out[n++] = s_buf[i];
    }
    out[n] = '\0';

    if (s_mtx) xSemaphoreGive(s_mtx);
    return n;
}

void log_ring_clear(void)
{
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_head = 0;
    s_len = 0;
    s_wrapped = false;
    if (s_mtx) xSemaphoreGive(s_mtx);
}
