/*
 * web_server.h - 内置 HTTP 服务器
 *   REST API: 配置读写 / 状态查询 / WOL / 重启
 *   WebSocket: 网页串口终端
 *   网页 OTA: 上传 .bin 升级固件
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t web_server_start(void);
esp_err_t web_server_stop(void);

/* 供 REST 处理程序注册(启动时自动调用, 无需手动) */
bool      web_requires_auth(httpd_req_t *req);
