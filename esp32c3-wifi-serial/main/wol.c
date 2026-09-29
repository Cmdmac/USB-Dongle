/*
 * wol.c - Wake-on-LAN: 构造魔术包(6x 0xFF + 16x MAC) 并 UDP 广播 9 端口
 */
#include "wol.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "wol";

/* 魔术包: 6 字节 0xFF + 目标 MAC 重复 16 次 = 102 字节 */
static int build_magic(const uint8_t mac[6], uint8_t *out /* >=102 */)
{
    memset(out, 0xFF, 6);
    for (int i = 0; i < 16; i++)
        memcpy(out + 6 + i * 6, mac, 6);
    return 102;
}

esp_err_t wol_wake_mac(const uint8_t mac[6], const char *ip)
{
    if (!mac) return ESP_ERR_INVALID_ARG;

    /* 全 0 MAC 视为未配置 */
    uint8_t zero[6] = {0};
    if (memcmp(mac, zero, 6) == 0) return ESP_ERR_INVALID_STATE;

    uint8_t pkt[102];
    int len = build_magic(mac, pkt);

    int fd = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket 创建失败: errno=%d", errno);
        return ESP_FAIL;
    }

    /* 允许广播 */
    int bcast = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(9);        /* WOL 标准端口 9 (0 备选) */
    dst.sin_addr.s_addr = (ip && ip[0])
                          ? inet_addr(ip)
                          : htonl(INADDR_BROADCAST);

    char macstr[18];
    app_cfg_mac_str(mac, macstr);

    int n = sendto(fd, pkt, len, 0, (struct sockaddr *)&dst, sizeof(dst));
    close(fd);

    if (n < 0) {
        ESP_LOGE(TAG, "WOL 发送失败(%s): errno=%d", macstr, errno);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WOL 已发送 -> %s (%s)", macstr,
             (ip && ip[0]) ? ip : "255.255.255.255");
    return ESP_OK;
}

esp_err_t wol_wake(uint8_t index)
{
    app_cfg_t *c = app_cfg();
    if (index >= c->wol_count) return ESP_ERR_NOT_FOUND;
    return wol_wake_mac(c->wol[index].mac, c->wol[index].ip);
}

esp_err_t wol_wake_all(void)
{
    app_cfg_t *c = app_cfg();
    if (c->wol_count == 0) return ESP_ERR_NOT_FOUND;

    esp_err_t ret = ESP_OK;
    for (uint8_t i = 0; i < c->wol_count; i++) {
        esp_err_t err = wol_wake_mac(c->wol[i].mac, c->wol[i].ip);
        if (err != ESP_OK) ret = err;
    }
    return ret;
}
