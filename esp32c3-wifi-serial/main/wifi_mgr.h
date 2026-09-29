/*
 * wifi_mgr.h - Wi-Fi 管理
 *   STA: 连接已配置的 AP, 断线自动重连
 *   AP:  若 ap_en 常开, 以 APSTA 模式提供配网热点 (默认 ESP32C3-Serial-XXXX)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_event.h"

typedef struct {
    bool     sta_connected;      /* STA 是否已连上 */
    char     ip[16];
    bool     ap_started;
    int      sta_retry_count;
} wifi_state_t;

esp_err_t wifi_mgr_init(void);

/* 重新读取配置并尝试连接(配网页保存后调用) */
esp_err_t wifi_mgr_connect(void);

/* 运行时更新 STA 配置(网页保存 Wi-Fi 后调用): 重设 SSID/密码并断线重连 */
esp_err_t wifi_mgr_apply_sta(void);

/* 获取当前状态 */
void      wifi_mgr_get_state(wifi_state_t *out);

/* 供 web_server 等注册 Wi-Fi/STA 事件(可选) */
void      wifi_mgr_register_handler(esp_event_handler_t handler, void *arg);
