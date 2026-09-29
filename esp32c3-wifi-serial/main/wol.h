/*
 * wol.h - Wake-on-LAN 网络唤醒
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "app_cfg.h"

/* 按配置表索引唤醒(校验 MAC 非全 0); 返回 ESP_OK 表示包已发出 */
esp_err_t wol_wake(uint8_t index);

/* 广播唤醒配置表里的全部目标 */
esp_err_t wol_wake_all(void);

/* 立即按 MAC 唤醒(不查配置表), ip 可为 NULL -> 255.255.255.255 */
esp_err_t wol_wake_mac(const uint8_t mac[6], const char *ip);
