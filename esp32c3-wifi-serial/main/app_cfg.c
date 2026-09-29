/*
 * app_cfg.c - 配置读写 (NVS 单 blob 存储, 带 magic/version 校验)
 */
#include "app_cfg.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "cfg";

#define CFG_NVS_NS   "wfs"
#define CFG_NVS_KEY  "cfg"
#define CFG_MAGIC    0x53455231u     /* "SER1" */
#define CFG_VER      1

typedef struct {
    uint32_t   magic;
    uint16_t   ver;
    uint16_t   len;
    app_cfg_t  cfg;
} cfg_blob_t;

static app_cfg_t s_cfg;
static bool      s_dirty_default = false;

/* ------------------------------------------------------------------ */
/* 默认值                                                              */
/* ------------------------------------------------------------------ */
void app_cfg_defaults(app_cfg_t *c)
{
    memset(c, 0, sizeof(*c));

    /* 网络 */
    c->ap_en = true;
    strlcpy(c->ap_pass, "12345678", sizeof(c->ap_pass));   /* 建议首次连上后修改 */
    strlcpy(c->hostname, "serial", sizeof(c->hostname));
    c->password[0] = '\0';                                  /* 默认不校验 */

    /* 串口: 默认用芯片内置 USB CDC —— 无外设即可透传 */
    c->serial_side  = SERIAL_SIDE_USB_CDC;
    c->uart_baud    = 115200;
    c->uart_tx_gpio = 4;
    c->uart_rx_gpio = 5;
    c->nl_xlate     = 0;

    /* 网络服务 */
    c->net_mode    = NET_MODE_TCP_SERVER;
    c->net_port    = 2333;
    c->remote_port = 2333;

    /* MQTT */
    c->mqtt_en = false;
    strlcpy(c->mqtt_prefix, "esp32c3-serial", sizeof(c->mqtt_prefix));

    /* 唤醒目标: 默认空(MAC 全 0 视为未配置) */
    c->wol_count = 0;
}

/* ------------------------------------------------------------------ */
/* 工具                                                                */
/* ------------------------------------------------------------------ */
static int hex_nibble(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool app_cfg_parse_mac(const char *s, uint8_t mac[6])
{
    if (!s) return false;
    int idx = 0, hi = -1;
    for (const char *p = s; *p && idx < 6; p++) {
        if (*p == ':' || *p == '-' || *p == '.' || *p == ' ') continue;
        int v = hex_nibble(*p);
        if (v < 0) return false;
        if (hi < 0) {
            hi = v;
        } else {
            mac[idx++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    return (idx == 6 && hi < 0);
}

void app_cfg_mac_str(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const char *app_cfg_serial_side_str(uint8_t side)
{
    switch (side) {
    case SERIAL_SIDE_USB_CDC: return "USB-CDC";
    case SERIAL_SIDE_UART0:   return "UART0";
    case SERIAL_SIDE_UART1:   return "UART1";
    default:                  return "?";
    }
}

const char *app_cfg_net_mode_str(uint8_t mode)
{
    switch (mode) {
    case NET_MODE_TCP_SERVER: return "TCP-Server";
    case NET_MODE_TCP_CLIENT: return "TCP-Client";
    case NET_MODE_TELNET:     return "Telnet";
    case NET_MODE_NONE:       return "None";
    default:                  return "?";
    }
}

void app_cfg_ap_ssid(char *out, size_t len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, len, "ESP32C3-Serial-%02X%02X", mac[4], mac[5]);
}

bool app_cfg_need_prov(void)
{
    return (s_cfg.wifi_ssid[0] == '\0');
}

/* ------------------------------------------------------------------ */
/* 初始化 / 保存                                                       */
/* ------------------------------------------------------------------ */
static void apply_sanity(app_cfg_t *c)
{
    /* 防止手改 NVS 后出现非法值 */
    if (c->serial_side > SERIAL_SIDE_UART1) c->serial_side = SERIAL_SIDE_USB_CDC;
    if (c->net_mode > NET_MODE_NONE)        c->net_mode    = NET_MODE_TCP_SERVER;
    if (c->net_port == 0)                   c->net_port    = 2333;
    if (c->remote_port == 0)                c->remote_port = 2333;
    if (c->uart_baud <= 0)                  c->uart_baud   = 115200;
    if (c->hostname[0] == '\0')             strlcpy(c->hostname, "serial", sizeof(c->hostname));
    if (c->mqtt_prefix[0] == '\0')          strlcpy(c->mqtt_prefix, "esp32c3-serial", sizeof(c->mqtt_prefix));
    if (c->wol_count > CFG_WOL_MAX)         c->wol_count   = CFG_WOL_MAX;

    /* 保证字符串以 0 结尾 */
    c->wifi_ssid[sizeof(c->wifi_ssid) - 1]   = '\0';
    c->wifi_pass[sizeof(c->wifi_pass) - 1]   = '\0';
    c->ap_pass[sizeof(c->ap_pass) - 1]       = '\0';
    c->password[sizeof(c->password) - 1]     = '\0';
    c->remote_host[sizeof(c->remote_host) - 1] = '\0';
    c->mqtt_uri[sizeof(c->mqtt_uri) - 1]     = '\0';
    c->mqtt_user[sizeof(c->mqtt_user) - 1]   = '\0';
    c->mqtt_pass[sizeof(c->mqtt_pass) - 1]   = '\0';
}

esp_err_t app_cfg_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open 失败: %s", esp_err_to_name(err));
        app_cfg_defaults(&s_cfg);
        return err;
    }

    cfg_blob_t blob;
    size_t sz = sizeof(blob);
    err = nvs_get_blob(h, CFG_NVS_KEY, &blob, &sz);
    if (err == ESP_OK && sz == sizeof(blob) &&
        blob.magic == CFG_MAGIC && blob.ver == CFG_VER && blob.len == sizeof(app_cfg_t)) {
        s_cfg = blob.cfg;
        apply_sanity(&s_cfg);
        ESP_LOGI(TAG, "配置已加载 (串口侧=%s, 网络=%s:%u, SSID=%s)",
                 app_cfg_serial_side_str(s_cfg.serial_side),
                 app_cfg_net_mode_str(s_cfg.net_mode), (unsigned)s_cfg.net_port,
                 s_cfg.wifi_ssid[0] ? s_cfg.wifi_ssid : "<未配置>");
    } else {
        ESP_LOGW(TAG, "配置缺失/版本不符(0x%x sz=%u), 使用默认值", (unsigned)err, (unsigned)sz);
        app_cfg_defaults(&s_cfg);
        s_dirty_default = true;
        nvs_close(h);
        app_cfg_save();
        return ESP_OK;
    }
    nvs_close(h);
    return ESP_OK;
}

app_cfg_t *app_cfg(void)
{
    return &s_cfg;
}

esp_err_t app_cfg_save(void)
{
    apply_sanity(&s_cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    cfg_blob_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = CFG_MAGIC;
    blob.ver   = CFG_VER;
    blob.len   = sizeof(app_cfg_t);
    blob.cfg   = s_cfg;

    err = nvs_set_blob(h, CFG_NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) ESP_LOGI(TAG, "配置已保存");
    else               ESP_LOGE(TAG, "配置保存失败: %s", esp_err_to_name(err));
    return err;
}

esp_err_t app_cfg_reset(void)
{
    app_cfg_defaults(&s_cfg);
    return app_cfg_save();
}
