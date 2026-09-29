/*
 * mqtt_bridge.c - MQTT 串口桥
 *
 * 主题设计:
 *   <prefix>/tx    设备 -> 云端 (串口数据, 二进制安全的 base64)
 *   <prefix>/cmd   云端 -> 设备 (下发数据, 原样写入串口; 支持 base64:"..." 包裹)
 *   <prefix>/state 设备 -> 云端 (周期/事件性的状态 JSON)
 *   <prefix>/get   云端 -> 设备 (要求立即上报一次 state)
 */
#include "mqtt_bridge.h"
#include "bridge.h"
#include "app_cfg.h"
#include "wifi_mgr.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "mqtt";

#define MQTT_TX_LIMIT  4096        /* 单条发布最大长度 */
#define MQTT_BASE64_CHUNK  512     /* 分段 base64 发布阈值 */

static esp_mqtt_client_handle_t s_client;
static bool s_started  = false;
static bool s_connected = false;
static mqtt_state_t s_state;

static char s_topic_tx[64];
static char s_topic_cmd[64];
static char s_topic_state[64];
static char s_topic_get[64];

/* base64 快速编码(标准字母表) */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t len, char *out, size_t out_max)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        if (o + 4 > out_max) return 0;
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? B64[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? B64[v & 0x3F] : '=';
    }
    if (o < out_max) out[o] = '\0';
    return o;
}

/* ------------------------------------------------------------------ */
/* bridge 通道: 串口数据 -> MQTT <prefix>/tx                           */
/* ------------------------------------------------------------------ */
static int mqtt_channel_write(bridge_channel_t *ch, const uint8_t *data, size_t len)
{
    (void)ch;
    if (!s_connected || !s_client) return -1;

    /* 小包直接发原文; 大包或含不可见字符则 base64 */
    bool printable = true;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (b < 0x20 && b != '\r' && b != '\n' && b != '\t') { printable = false; break; }
    }

    int qos = 0;
    if (printable && len <= MQTT_TX_LIMIT) {
        int r = esp_mqtt_client_publish(s_client, s_topic_tx, (const char *)data,
                                        (int)len, qos, 0);
        if (r >= 0) s_state.tx_count++;
        return (r >= 0) ? (int)len : -1;
    }

    /* base64 分段(每段独立消息, 对端按顺序拼接即可) */
    static char b64buf[((MQTT_BASE64_CHUNK / 3) + 1) * 4 + 1];
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > MQTT_BASE64_CHUNK) chunk = MQTT_BASE64_CHUNK;
        size_t n = b64_encode(data + off, chunk, b64buf, sizeof(b64buf) - 1);
        if (n == 0) break;
        if (esp_mqtt_client_publish(s_client, s_topic_tx, b64buf, (int)n, qos, 0) < 0)
            return -1;
        s_state.tx_count++;
        off += chunk;
    }
    return (int)len;
}

/* base64 解码(cmd 主题可选 base64:"..." 形式) */
static int b64_dec(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char *in, size_t len, uint8_t *out, size_t out_max)
{
    size_t o = 0, bits = 0;
    uint32_t acc = 0;
    for (size_t i = 0; i < len; i++) {
        char c = in[i];
        if (c == '=') break;
        int v = b64_dec(c);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_max) return 0;
            out[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* MQTT 事件                                                            */
/* ------------------------------------------------------------------ */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t ev = event_data;
    (void)handler_args;
    (void)base;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        s_state.connected = true;
        ESP_LOGI(TAG, "MQTT 已连接 %s", s_state.uri);

        char topic[80];
        snprintf(topic, sizeof(topic), "%s/cmd", s_state.prefix);
        esp_mqtt_client_subscribe(s_client, topic, 1);
        snprintf(topic, sizeof(topic), "%s/get", s_state.prefix);
        esp_mqtt_client_subscribe(s_client, topic, 1);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        s_state.connected = false;
        ESP_LOGW(TAG, "MQTT 断开");
        break;

    case MQTT_EVENT_DATA: {
        /* ev->topic / ev->data 可能因缓冲不足被截断, 长度字段才是权威 */
        if (ev->data_len == 0) break;

        bool is_cmd = (ev->topic_len >= 4 &&
                       strncmp(ev->topic + ev->topic_len - 4, "/cmd", 4) == 0);
        bool is_get = (ev->topic_len >= 4 &&
                       strncmp(ev->topic + ev->topic_len - 4, "/get", 4) == 0);

        if (is_get) {
            mqtt_publish_state(NULL);   /* 立即上报一次状态 */
            break;
        }
        if (!is_cmd) break;

        s_state.rx_count++;

        /* base64:"...." 或原文 */
        if (ev->data_len > 8 && strncmp(ev->data, "base64:", 7) == 0) {
            static uint8_t dec[1024];
            size_t n = b64_decode(ev->data + 7, (size_t)ev->data_len - 7,
                                  dec, sizeof(dec));
            if (n > 0) bridge_net_rx(NULL, dec, n);
        } else {
            bridge_net_rx(NULL, (const uint8_t *)ev->data, (size_t)ev->data_len);
        }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* 公共接口                                                            */
/* ------------------------------------------------------------------ */
void mqtt_publish_state(const char *json)
{
    if (!s_connected || !s_client) return;

    char payload[256];
    if (json) {
        strlcpy(payload, json, sizeof(payload));
    } else {
        wifi_state_t w;
        wifi_mgr_get_state(&w);
        bridge_stats_t bs;
        bridge_get_stats(&bs);
        snprintf(payload, sizeof(payload),
                 "{\"ip\":\"%s\",\"connected\":%s,\"ser_rx\":%lu,\"ser_tx\":%lu}",
                 w.ip, w.sta_connected ? "true" : "false",
                 (unsigned long)bs.ser_rx, (unsigned long)bs.ser_tx);
    }
    esp_mqtt_client_publish(s_client, s_topic_state, payload, 0, 0, 0);
}

void mqtt_bridge_get_state(mqtt_state_t *out)
{
    if (out) *out = s_state;
}

esp_err_t mqtt_bridge_init(void)
{
    app_cfg_t *c = app_cfg();
    if (!c->mqtt_en || c->mqtt_uri[0] == '\0') {
        ESP_LOGI(TAG, "MQTT 未启用");
        return ESP_OK;
    }

    /* 主题缓冲 */
    snprintf(s_topic_tx,    sizeof(s_topic_tx),    "%s/tx",    c->mqtt_prefix);
    snprintf(s_topic_cmd,   sizeof(s_topic_cmd),   "%s/cmd",   c->mqtt_prefix);
    snprintf(s_topic_state, sizeof(s_topic_state), "%s/state", c->mqtt_prefix);
    snprintf(s_topic_get,   sizeof(s_topic_get),   "%s/get",   c->mqtt_prefix);

    strlcpy(s_state.uri, c->mqtt_uri, sizeof(s_state.uri));
    strlcpy(s_state.prefix, c->mqtt_prefix, sizeof(s_state.prefix));
    s_state.enabled = true;

    esp_mqtt_client_config_t mc = {0};
    mc.broker.address.uri = c->mqtt_uri;
    if (c->mqtt_user[0]) {
        mc.credentials.username = c->mqtt_user;
        mc.credentials.authentication.password = c->mqtt_pass;
    }
    mc.session.keepalive = 30;
    mc.buffer.size = 2048;

    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        ESP_LOGE(TAG, "MQTT 客户端创建失败");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    /* 注册为 bridge 通道: 串口数据会推到 <prefix>/tx */
    if (!bridge_channel_register(mqtt_channel_write, NULL, "mqtt")) {
        ESP_LOGW(TAG, "bridge 通道已满, MQTT tx 不工作");
    }

    esp_mqtt_client_start(s_client);
    s_started = true;
    ESP_LOGI(TAG, "MQTT 客户端已启动: %s (prefix=%s)", c->mqtt_uri, c->mqtt_prefix);
    return ESP_OK;
}

esp_err_t mqtt_bridge_stop(void)
{
    if (!s_started) return ESP_OK;
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_unsubscribe(s_client, s_topic_cmd);
    esp_mqtt_client_unsubscribe(s_client, s_topic_get);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_started = false;
    s_connected = false;
    s_state.connected = false;
    return ESP_OK;
}

esp_err_t mqtt_bridge_restart(void)
{
    mqtt_bridge_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    return mqtt_bridge_init();
}
