/*
 * mqtt_bridge.h - MQTT 桥 (可选模块)
 *   订阅 <prefix>/cmd  -> 数据写入串口
 *   串口数据          -> 发布到 <prefix>/tx
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool     enabled;
    bool     connected;
    char     uri[96];
    char     prefix[48];
    uint32_t tx_count;      /* 已发布消息数 */
    uint32_t rx_count;      /* 已接收消息数 */
} mqtt_state_t;

esp_err_t mqtt_bridge_init(void);
esp_err_t mqtt_bridge_stop(void);

/* 运行中重连/重配 */
esp_err_t mqtt_bridge_restart(void);

void mqtt_bridge_get_state(mqtt_state_t *out);

/* 发布设备状态 JSON 到 <prefix>/state */
void mqtt_publish_state(const char *json);
