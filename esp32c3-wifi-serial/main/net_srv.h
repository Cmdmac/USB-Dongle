/*
 * net_srv.h - 网络串口服务 (TCP Server / TCP Client / Telnet)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool     running;
    uint8_t  mode;          /* net_mode_t */
    uint16_t port;
    int      client_count;  /* 当前连接的客户端数 */
    char     remote[80];    /* TCP 客户端模式的对端 host:port */
} net_srv_state_t;

esp_err_t net_srv_start(void);
esp_err_t net_srv_stop(void);

/* 运行中重新应用配置(重启内部任务) */
esp_err_t net_srv_restart(void);

void net_srv_get_state(net_srv_state_t *out);

/* 向所有 TCP/Telnet 客户端主动推送一行文本(如设备通知) */
void net_srv_broadcast_line(const char *line);

/* 当前在线客户端数 */
int  net_srv_count(void);
