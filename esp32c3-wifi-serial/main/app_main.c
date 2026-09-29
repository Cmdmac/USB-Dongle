/*
 * app_main.c - 启动入口
 *
 * 启动顺序(重要):
 *   1. log_ring   日志环形缓存(最早的日志也不能丢)
 *   2. NVS + 配置
 *   3. 串口侧     (USB-CDC 或 UART) — 决定透通道
 *   4. Wi-Fi      (STA + 可选配网 AP)
 *   5. bridge     (串口 <-> 网络分发核心)
 *   6. net_srv    (TCP/Telnet 服务, 按配置)
 *   7. MQTT       (可选)
 *   8. Web        (配置页 + WS 终端 + OTA)
 *   9. mDNS       (serial.local)
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "app_cfg.h"
#include "log_ring.h"
#include "serial_port.h"
#include "wifi_mgr.h"
#include "bridge.h"
#include "net_srv.h"
#include "mqtt_bridge.h"
#include "web_server.h"

static const char *TAG = "main";

static void start_mdns(void)
{
    app_cfg_t *c = app_cfg();

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS 初始化失败: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(c->hostname);
    mdns_instance_name(APP_NAME);

    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    if (c->net_mode == NET_MODE_TELNET || c->net_mode == NET_MODE_TCP_SERVER) {
        uint16_t port = c->net_port;
        if (c->net_mode == NET_MODE_TELNET && port == 2333) port = 23;
        mdns_service_add(NULL, "_telnet", "_tcp", port, NULL, 0);
    }
    ESP_LOGI(TAG, "mDNS: http://%s.local", c->hostname);
}

void app_main(void)
{
    /* 1. 日志环形缓存(越早越好, 否则早期日志丢失) */
    log_ring_init();

    ESP_LOGI(TAG, "=== %s v%s ===", APP_NAME, APP_VERSION);
    ESP_LOGI(TAG, "ESP-IDF %s, 最小空闲堆: %u", esp_get_idf_version(),
             (unsigned)esp_get_minimum_free_heap_size());

    /* 2. NVS + 配置 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(app_cfg_init());

    if (app_cfg_need_prov()) {
        ESP_LOGW(TAG, "首次启动: 请连配网热点配置 Wi-Fi (默认密码 12345678)");
    }

    /* 3. 串口侧 */
    ESP_ERROR_CHECK(serial_port_init());

    /* 4. Wi-Fi */
    ESP_ERROR_CHECK(wifi_mgr_init());

    /* 5. 数据桥接 */
    ESP_ERROR_CHECK(bridge_init());

    /* 6. 网络串口服务 */
    net_srv_start();     /* 失败不致命: 网页终端仍可用 */

    /* 7. MQTT (可选) */
    mqtt_bridge_init();

    /* 8. Web 管理 */
    ESP_ERROR_CHECK(web_server_start());

    /* 9. mDNS */
    start_mdns();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    ESP_LOGI(TAG, "启动完成。");
    ESP_LOGI(TAG, "  - 配网热点: ESP32C3-Serial-%02X%02X (密码 12345678)", mac[4], mac[5]);
    ESP_LOGI(TAG, "  - 网页管理: http://%s.local 或 http://<设备IP>/", app_cfg()->hostname);
    ESP_LOGI(TAG, "  - TCP 透传: 端口 %u", (unsigned)app_cfg()->net_port);
}
