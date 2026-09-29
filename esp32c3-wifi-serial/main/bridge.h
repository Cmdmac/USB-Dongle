/*
 * bridge.h - 数据桥接中枢
 *
 *   串口侧 (USB-CDC / UART) <----> 多个网络通道 (TCP/Telnet/WS/MQTT)
 *
 * 架构: bridge 拥有一个独立任务, 轮询读取串口, 把收到的字节按"通道"分发:
 *   - 串口 -> 网络: 按 nl_xlate 配置做换行转换后广播到所有在线通道
 *   - 网络 -> 串口: 任意通道收到数据都写入串口
 *
 * 一个"通道" =  { write 回调, 属主指针 }, 同一时刻可以并存 TCP 多客户端 +
 * WebSocket 终端 + MQTT, 数据互通。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#define BRIDGE_MAX_CHANNELS 8

typedef struct bridge_channel bridge_channel_t;

/* 通道写回调: 把数据发往该通道的对端。返回实际写入字节数(全部写入返回 len)。 */
typedef int (*bridge_write_fn)(bridge_channel_t *ch, const uint8_t *data, size_t len);

struct bridge_channel {
    bridge_write_fn  write;     /* 必填 */
    void            *owner;     /* 属主(net_srv 的客户端 / web_server 的 ws 会话 / mqtt) */
    const char      *name;      /* 调试用名字, 静态字符串 */
};

/* 初始化 + 启动桥接任务 */
esp_err_t bridge_init(void);

/* 注册/注销通道。返回通道句柄或 NULL(已满)。注销时数据仍在收发也不会崩溃。 */
bridge_channel_t *bridge_channel_register(bridge_write_fn write, void *owner, const char *name);
void              bridge_channel_unregister(bridge_channel_t *ch);

/* 任意网络通道收到数据时调用: 转发到串口(以及可选地回显给其它通道)。 */
void bridge_net_rx(bridge_channel_t *from, const uint8_t *data, size_t len);

/* 桥接统计信息(状态页显示) */
typedef struct {
    uint32_t ser_rx;    /* 串口 -> 网络 字节数 */
    uint32_t ser_tx;    /* 网络 -> 串口 字节数 */
    uint32_t dropped;   /* 因通道写满被丢弃的字节数 */
} bridge_stats_t;

void bridge_get_stats(bridge_stats_t *out);

/* 串口 -> 网络方向的换行转换开关(0=原样 1=LF 补成 CRLF), 运行时可改 */
void bridge_set_nl_xlate(int on);
