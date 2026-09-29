/*
 * bridge.c - 串口 <-> 网络 数据桥接实现
 */
#include "bridge.h"
#include "serial_port.h"
#include "app_cfg.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bridge";

/* 串口读取缓冲与"发往网络"的二次缓冲(换行转换后) */
#define SER_READ_BUF   512
#define NET_OUT_BUF    (SER_READ_BUF * 2)

static bridge_channel_t s_channels[BRIDGE_MAX_CHANNELS];
static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;
static bool              s_nl_xlate;
static bridge_stats_t    s_stats;

/* ------------------------------------------------------------------ */
/* 通道注册表                                                          */
/* ------------------------------------------------------------------ */
bridge_channel_t *bridge_channel_register(bridge_write_fn write, void *owner, const char *name)
{
    if (!write) return NULL;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bridge_channel_t *slot = NULL;
    for (int i = 0; i < BRIDGE_MAX_CHANNELS; i++) {
        if (s_channels[i].write == NULL) {
            slot = &s_channels[i];
            break;
        }
    }
    if (slot) {
        slot->write = write;
        slot->owner = owner;
        slot->name  = name ? name : "?";
    }
    xSemaphoreGive(s_lock);

    if (!slot) ESP_LOGW(TAG, "通道已满, 拒绝注册 %s", name ? name : "?");
    return slot;
}

void bridge_channel_unregister(bridge_channel_t *ch)
{
    if (!ch) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ch->write = NULL;     /* 置空即可, 持锁快照不会再调用 */
    ch->owner = NULL;
    ch->name  = NULL;
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ */
/* 串口 -> 网络: 广播到所有通道                                        */
/* ------------------------------------------------------------------ */
static void fanout_to_net(const uint8_t *data, size_t len)
{
    /* 快照通道列表, 避免回调中注销导致迭代错乱 */
    bridge_channel_t snap[BRIDGE_MAX_CHANNELS];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(snap, s_channels, sizeof(snap));
    xSemaphoreGive(s_lock);

    for (int i = 0; i < BRIDGE_MAX_CHANNELS; i++) {
        if (snap[i].write == NULL) continue;

        /* 整段写, 写不完则丢弃多余部分(避免一个慢客户端阻塞所有人)。
         * 注意传快照地址: 即使原槽位此刻被并发注销, 快照仍自洽。 */
        int n = snap[i].write(&snap[i], data, len);
        if (n < 0 || (size_t)n < len) {
            s_stats.dropped += (uint32_t)(len - (n > 0 ? n : 0));
        }
    }
}

/* ------------------------------------------------------------------ */
/* 网络 -> 串口                                                        */
/* ------------------------------------------------------------------ */
void bridge_net_rx(bridge_channel_t *from, const uint8_t *data, size_t len)
{
    (void)from;
    if (!data || len == 0) return;

    int n = serial_port_write(data, len, 200);
    if (n > 0) {
        s_stats.ser_tx += (uint32_t)n;
        if ((size_t)n < len) s_stats.dropped += (uint32_t)(len - n);
    } else {
        s_stats.dropped += (uint32_t)len;
    }
}

/* ------------------------------------------------------------------ */
/* 换行转换                                                            */
/* ------------------------------------------------------------------ */
static size_t apply_nl(const uint8_t *in, size_t len, uint8_t *out, size_t out_max)
{
    size_t o = 0;
    bool prev_cr = false;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = in[i];
        if (b == '\n' && !prev_cr) {
            if (o + 2 <= out_max) {
                out[o++] = '\r';
                out[o++] = '\n';
            }
        } else {
            if (o + 1 <= out_max) out[o++] = b;
        }
        prev_cr = (b == '\r');
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* 桥接任务: 持续从串口读并分发                                        */
/* ------------------------------------------------------------------ */
static void bridge_task(void *arg)
{
    static uint8_t rbuf[SER_READ_BUF];
    static uint8_t obuf[NET_OUT_BUF];

    for (;;) {
        int n = serial_port_read(rbuf, sizeof(rbuf), 100);
        if (n > 0) {
            size_t send_len = (size_t)n;
            const uint8_t *send_ptr = rbuf;

            if (s_nl_xlate) {
                send_len = apply_nl(rbuf, (size_t)n, obuf, sizeof(obuf));
                send_ptr = obuf;
            }

            s_stats.ser_rx += (uint32_t)n;
            fanout_to_net(send_ptr, send_len);
        } else if (n < 0) {
            /* 串口错误(如 USB 端断开): 不忙等 */
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

/* ------------------------------------------------------------------ */
/* 公共接口                                                            */
/* ------------------------------------------------------------------ */
void bridge_set_nl_xlate(int on)
{
    s_nl_xlate = on ? true : false;
}

void bridge_get_stats(bridge_stats_t *out)
{
    if (out) *out = s_stats;
}

esp_err_t bridge_init(void)
{
    if (s_lock) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    memset(s_channels, 0, sizeof(s_channels));
    memset(&s_stats, 0, sizeof(s_stats));
    s_nl_xlate = app_cfg()->nl_xlate ? true : false;

    BaseType_t ok = xTaskCreate(bridge_task, "bridge", 3072, NULL, 5, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建 bridge 任务失败");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "数据桥接已启动");
    return ESP_OK;
}
