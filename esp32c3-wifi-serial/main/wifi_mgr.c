/*
 * wifi_mgr.c - Wi-Fi STA + AP(配网) 管理
 */
#include "wifi_mgr.h"
#include "app_cfg.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "wifi";

#define WIFI_RECONNECT_MAX_DELAY_MS 30000
#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_ev;
static esp_timer_handle_t s_retry_timer;
static wifi_state_t  s_state;
static bool          s_inited = false;

/* ------------------------------------------------------------------ */
/* 延迟重连(esp_timer 回调, 避免阻塞事件循环任务)                      */
/* ------------------------------------------------------------------ */
static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (!s_state.sta_connected) {
        ESP_LOGI(TAG, "发起第 %d 次重连", s_state.sta_retry_count);
        esp_wifi_connect();
    }
}

/* ------------------------------------------------------------------ */
/* 事件处理                                                            */
/* ------------------------------------------------------------------ */
static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            s_state.sta_connected = false;

            /* 无 SSID 时静止(纯 AP 模式), 不做无谓重连 */
            if (app_cfg()->wifi_ssid[0] == '\0') break;

            s_state.sta_retry_count++;

            /* 指数退避: 1s,2s,4s,8s...上限 30s (esp_timer 一次性触发, 不阻塞事件循环) */
            uint32_t delay = 1000U << (s_state.sta_retry_count > 5 ? 5 : s_state.sta_retry_count);
            if (delay > WIFI_RECONNECT_MAX_DELAY_MS) delay = WIFI_RECONNECT_MAX_DELAY_MS;

            ESP_LOGW(TAG, "STA 断开, %lums 后第 %d 次重连...",
                     (unsigned long)delay, s_state.sta_retry_count);

            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, (uint64_t)delay * 1000U);
            break;
        }
        case WIFI_EVENT_AP_START:
            s_state.ap_started = true;
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "有设备连入配网热点");
            break;
    default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa((const esp_ip4_addr_t *)&evt->ip_info.ip,
                         s_state.ip, sizeof(s_state.ip));
        s_state.sta_connected = true;
        s_state.sta_retry_count = 0;
        ESP_LOGI(TAG, "已连接, IP: %s", s_state.ip);
        xEventGroupSetBits(s_ev, WIFI_CONNECTED_BIT);
    }
}

/* ------------------------------------------------------------------ */
/* 公共接口                                                            */
/* ------------------------------------------------------------------ */
void wifi_mgr_get_state(wifi_state_t *out)
{
    if (out) *out = s_state;
}

esp_err_t wifi_mgr_connect(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    app_cfg_t *c = app_cfg();
    if (c->wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "未配置 Wi-Fi, 仅启动配网热点");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, c->wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, c->wifi_pass, sizeof(wc.sta.password));
    wc.sta.scan_method = WIFI_FAST_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    s_state.sta_retry_count = 0;
    esp_wifi_connect();
    return ESP_OK;
}

esp_err_t wifi_mgr_apply_sta(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;

    app_cfg_t *c = app_cfg();
    if (c->wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "SSID 为空, 忽略 STA 更新");
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, c->wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, c->wifi_pass, sizeof(wc.sta.password));
    wc.sta.scan_method = WIFI_FAST_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    /* 断开触发自动重连, 立即用新配置 */
    s_state.sta_retry_count = 0;
    esp_wifi_disconnect();
    return ESP_OK;
}

esp_err_t wifi_mgr_register_handler(esp_event_handler_t handler, void *arg)
{
    esp_err_t err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, handler, arg);
    if (err == ESP_OK)
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, handler, arg);
    return err;
}

esp_err_t wifi_mgr_init(void)
{
    if (s_inited) return ESP_OK;

    app_cfg_t *c = app_cfg();

    /* 1. 初始化 TCP/IP 协议栈与事件循环 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 2. 创建 netif (STA 必建; AP 由配置决定) */
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(sta, c->hostname));
    if (c->ap_en) {
        esp_netif_create_default_wifi_ap();
    }

    /* 3. 初始化 Wi-Fi 驱动 */
    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));
    /* 配置由 app_cfg 每次启动时下发, 不使用 Wi-Fi 组件自己的 NVS 存储 */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    /* 4. 模式: APSTA(常开热点) 或 纯 STA */
    ESP_ERROR_CHECK(esp_wifi_set_mode(c->ap_en ? WIFI_MODE_APSTA : WIFI_MODE_STA));

    /* 5. 事件注册(必须在 start 前注册, 否则丢 WIFI_EVENT_STA_START) */
    s_ev = xEventGroupCreate();
    memset(&s_state, 0, sizeof(s_state));

    const esp_timer_create_args_t targs = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));

    /* 6. STA 侧配置 */
    if (c->wifi_ssid[0]) {
        wifi_config_t wc = {0};
        strlcpy((char *)wc.sta.ssid, c->wifi_ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, c->wifi_pass, sizeof(wc.sta.password));
        wc.sta.scan_method = WIFI_FAST_SCAN;
        wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        wc.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    }

    /* 7. AP 侧配置(若开启) */
    if (c->ap_en) {
        wifi_config_t ap = {0};
        app_cfg_ap_ssid((char *)ap.ap.ssid, sizeof(ap.ap.ssid));
        ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
        ap.ap.channel = 1;
        ap.ap.max_connection = 2;
        ap.ap.authmode = (c->ap_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN);
        if (c->ap_pass[0])
            strlcpy((char *)ap.ap.password, c->ap_pass, sizeof(ap.ap.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

        ESP_LOGI(TAG, "配网热点: SSID=%s %s", ap.ap.ssid,
                 c->ap_pass[0] ? "WPA2" : "OPEN");
    }

    /* 8. 启动 */
    ESP_ERROR_CHECK(esp_wifi_start());
    s_inited = true;
    return ESP_OK;
}
