/*
 * app_cfg.h - 运行配置(整块存 NVS blob)与默认值
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define APP_NAME        "esp32c3-wifi-serial"
#define APP_VERSION     "1.0.0"

/* ---------------- 串口侧选择 ---------------- */
typedef enum {
    SERIAL_SIDE_USB_CDC = 0,   /* ESP32-C3 内置 USB Serial/JTAG —— 默认, 无外设即可用 */
    SERIAL_SIDE_UART0   = 1,   /* UART0: TX=GPIO21 / RX=GPIO20 (需关闭串口控制台) */
    SERIAL_SIDE_UART1   = 2,   /* UART1: 引脚自定义 */
} serial_side_t;

/* ---------------- 网络侧模式 ---------------- */
typedef enum {
    NET_MODE_TCP_SERVER = 0,   /* TCP 服务端(多客户端) —— 默认, 端口 2333 */
    NET_MODE_TCP_CLIENT = 1,   /* TCP 客户端(主动连服务器) */
    NET_MODE_TELNET     = 2,   /* Telnet 服务端(带 IAC 过滤), 端口 23 */
    NET_MODE_NONE       = 3,   /* 关闭, 只保留网页终端 */
} net_mode_t;

#define CFG_WOL_MAX 4

typedef struct {
    uint8_t mac[6];
    char    name[20];
    char    ip[16];        /* 可选: 目标单播地址或定向广播, 如 192.168.1.255 */
} wol_target_t;

typedef struct {
    /* ---- 网络 ---- */
    char     wifi_ssid[33];
    char     wifi_pass[65];
    bool     ap_en;            /* 配网热点(AP)是否常开 */
    char     ap_pass[33];      /* 空 = 开放热点(不建议) */
    char     hostname[33];     /* mDNS / DHCP 主机名 */
    char     password[33];     /* 数据通道 + 网页管理密码, 空 = 不校验 */

    /* ---- 串口 ---- */
    uint8_t  serial_side;      /* serial_side_t */
    int32_t  uart_baud;        /* 仅 UART 模式有效 (USB-CDC 波特率无意义) */
    int32_t  uart_tx_gpio;
    int32_t  uart_rx_gpio;
    uint8_t  nl_xlate;         /* 0=原样; 1=串口->网络时把 LF 补成 CRLF */

    /* ---- 网络服务 ---- */
    uint8_t  net_mode;         /* net_mode_t */
    uint16_t net_port;
    char     remote_host[64];  /* TCP 客户端模式的对端 */
    uint16_t remote_port;

    /* ---- MQTT (可选) ---- */
    bool     mqtt_en;
    char     mqtt_uri[96];     /* mqtt://user:pass@host:1883 也可只填 host */
    char     mqtt_user[33];
    char     mqtt_pass[33];
    char     mqtt_prefix[48];  /* 主题前缀, 默认 esp32c3-serial */

    /* ---- 网络唤醒 (Wake-on-LAN) ---- */
    wol_target_t wol[CFG_WOL_MAX];
    uint8_t      wol_count;
} app_cfg_t;

esp_err_t   app_cfg_init(void);
app_cfg_t  *app_cfg(void);
esp_err_t   app_cfg_save(void);
esp_err_t   app_cfg_reset(void);
void        app_cfg_defaults(app_cfg_t *c);

/* 生成默认 AP SSID: ESP32C3-Serial-XXXX */
void        app_cfg_ap_ssid(char *out, size_t len);

bool        app_cfg_parse_mac(const char *s, uint8_t mac[6]);
void        app_cfg_mac_str(const uint8_t mac[6], char out[18]);

const char *app_cfg_serial_side_str(uint8_t side);
const char *app_cfg_net_mode_str(uint8_t mode);

/* 首次启动(或未配置 Wi-Fi)判定 */
bool        app_cfg_need_prov(void);
