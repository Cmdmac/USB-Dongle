/*
 * net_srv.c - TCP 服务端(多客户端) / TCP 客户端 / Telnet 服务端
 *
 * 服务端模式: 单任务 + select() 多路复用, 同时处理 accept 与所有客户端读;
 *             每个客户端注册为一个 bridge 通道。
 * Telnet 模式: 在服务端基础上过滤入向 IAC 协商序列, 并拒绝所有选项协商。
 */
#include "net_srv.h"
#include "bridge.h"
#include "app_cfg.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "net";

#define NET_MAX_CLIENTS    4
#define NET_RX_BUF         512
#define NET_LISTEN_BACKLOG 4
#define NET_TASK_STACK     4096

typedef struct {
    int               fd;         /* -1 = 空槽 */
    bool              is_telnet;  /* 是否做 IAC 过滤 */
    uint8_t           iac_state;  /* telnet IAC 解析状态, 见 TS_* */
    bridge_channel_t *ch;         /* bridge 通道 */
    char              peer[24];   /* "ip:port" */
} client_slot_t;

/* Telnet IAC 解析状态 */
enum { TS_DATA = 0, TS_IAC, TS_CMD, TS_SB };

static client_slot_t s_clients[NET_MAX_CLIENTS];
static int           s_listen_fd = -1;
static int           s_cli_fd = -1;            /* TCP 客户端模式的连接 */
static char          s_cli_peer[80];
static TaskHandle_t  s_task;
static volatile bool s_running = false;
static SemaphoreHandle_t s_lock;

/* ------------------------------------------------------------------ */
/* 内部工具                                                            */
/* ------------------------------------------------------------------ */
int net_srv_count(void)
{
    int n = 0;
    for (int i = 0; i < NET_MAX_CLIENTS; i++)
        if (s_clients[i].fd >= 0) n++;
    return n;
}

/* bridge 写回调: 把串口数据发往对应客户端 socket (owner = 槽位指针) */
static int client_write_cb(bridge_channel_t *ch, const uint8_t *data, size_t len)
{
    client_slot_t *slot = (client_slot_t *)ch->owner;
    int n = lwip_write(slot->fd, data, len);
    return (n < 0) ? -1 : n;
}

static void slot_close(int idx)
{
    client_slot_t *c = &s_clients[idx];
    if (c->fd < 0) return;

    if (c->ch) {
        bridge_channel_unregister(c->ch);
        c->ch = NULL;
    }
    close(c->fd);
    c->fd = -1;
    c->peer[0] = '\0';
}

static void close_all_clients(void)
{
    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        slot_close(i);
        xSemaphoreGive(s_lock);
    }
}

/* Telnet: 过滤入向 IAC 序列, 返回有效数据长度(原址压缩) */
static size_t telnet_filter(uint8_t *buf, size_t len, uint8_t *st)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = buf[i];
        switch (*st) {
        case TS_DATA:
            if (b == 0xFF) *st = TS_IAC;
            else           buf[o++] = b;
            break;
        case TS_IAC:
            if (b == 0xFF) {                 /* IAC IAC = 字面 0xFF */
                buf[o++] = 0xFF;
                *st = TS_DATA;
            } else if (b >= 0xFB && b <= 0xFE) {
                *st = TS_CMD;                /* WILL/WONT/DO/DONT + 选项字节 */
            } else if (b == 0xFA) {
                *st = TS_SB;                 /* 子协商直到 IAC SE */
            } else {
                *st = TS_DATA;               /* NOP/DM 等单字节命令 */
            }
            break;
        case TS_CMD:
            *st = TS_DATA;                   /* 吃掉选项字节 */
            break;
        case TS_SB:
            if (b == 0xFF) *st = TS_IAC;
            break;
        default:
            *st = TS_DATA;
            break;
        }
    }
    return o;
}

/* Telnet: 拒绝所有协商(不回显由客户端自己处理, 我们只透传) */
static void telnet_send_refuse(int fd)
{
    static const uint8_t refuse[] = {
        0xFF, 0xFE, 0x01,   /* IAC DONT ECHO */
        0xFF, 0xFE, 0x03,   /* IAC DONT SGA  */
    };
    lwip_write(fd, refuse, sizeof(refuse));
}

/* 处理一个客户端 socket 上的可读数据, 返回 false 表示应断开 */
static bool handle_client_data(client_slot_t *c)
{
    static uint8_t buf[NET_RX_BUF];

    int n = lwip_read(c->fd, buf, sizeof(buf));
    if (n == 0) return false;                       /* 对端关闭 */
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        return false;
    }

    size_t len = (size_t)n;
    if (c->is_telnet) {
        len = telnet_filter(buf, len, &c->iac_state);
        if (len == 0) return true;                  /* 纯协商包, 无数据 */
    }

    bridge_net_rx(c->ch, buf, len);                 /* -> 串口 */
    return true;
}

/* ------------------------------------------------------------------ */
/* 服务端任务: select 多路复用 accept + 所有客户端                     */
/* ------------------------------------------------------------------ */
static void server_task(void *arg)
{
    app_cfg_t *c = app_cfg();
    bool is_telnet = (c->net_mode == NET_MODE_TELNET);
    uint16_t port = c->net_port;
    if (is_telnet && port == 2333) port = 23;       /* telnet 默认端口 */

    s_listen_fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen_fd < 0) {
        ESP_LOGE(TAG, "socket 创建失败: errno=%d", errno);
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind :%u 失败: errno=%d", port, errno);
        goto fail;
    }
    if (listen(s_listen_fd, NET_LISTEN_BACKLOG) < 0) {
        ESP_LOGE(TAG, "listen 失败: errno=%d", errno);
        goto fail;
    }

    ESP_LOGI(TAG, "%s 服务已启动, 端口 %u",
             is_telnet ? "Telnet" : "TCP-Server", port);

    while (s_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s_listen_fd, &rfds);
        int maxfd = s_listen_fd;

        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            if (s_clients[i].fd >= 0) {
                FD_SET(s_clients[i].fd, &rfds);
                if (s_clients[i].fd > maxfd) maxfd = s_clients[i].fd;
            }
        }

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int rv = lwip_select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "select 失败: errno=%d", errno);
            break;
        }
        if (rv == 0) continue;                      /* 超时, 回头检查 s_running */

        /* 新连接 */
        if (FD_ISSET(s_listen_fd, &rfds)) {
            struct sockaddr_in cli = {0};
            socklen_t clen = sizeof(cli);
            int fd = lwip_accept(s_listen_fd, (struct sockaddr *)&cli, &clen);
            if (fd >= 0) {
                char peer[24];
                snprintf(peer, sizeof(peer), "%s:%u",
                         inet_ntoa(cli.sin_addr), (unsigned)ntohs(cli.sin_port));

                /* 设置非阻塞式超时, 避免慢客户端拖垮 select 循环 */
                struct timeval rtv = { .tv_sec = 0, .tv_usec = 200000 };
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

                xSemaphoreTake(s_lock, portMAX_DELAY);
                int idx = -1;
                for (int i = 0; i < NET_MAX_CLIENTS; i++)
                    if (s_clients[i].fd < 0) { idx = i; break; }
                if (idx >= 0) {
                    s_clients[idx].fd = fd;
                    s_clients[idx].is_telnet = is_telnet;
                    s_clients[idx].iac_state = TS_DATA;
                    strlcpy(s_clients[idx].peer, peer, sizeof(s_clients[idx].peer));
                    s_clients[idx].ch = bridge_channel_register(
                        client_write_cb, &s_clients[idx], s_clients[idx].peer);
                    if (!s_clients[idx].ch) slot_close(idx);
                }
                xSemaphoreGive(s_lock);

                if (idx < 0) {
                    ESP_LOGW(TAG, "客户端已满, 拒绝 %s", peer);
                    close(fd);
                } else {
                    if (is_telnet) telnet_send_refuse(fd);
                    ESP_LOGI(TAG, "客户端接入: %s (在线 %d)", peer, net_srv_count());
                }
            }
        }

        /* 已有客户端数据 */
        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            if (s_clients[i].fd >= 0 && FD_ISSET(s_clients[i].fd, &rfds)) {
                bool keep = handle_client_data(&s_clients[i]);
                if (!keep) {
                    ESP_LOGI(TAG, "客户端断开: %s (在线 %d)",
                             s_clients[i].peer, net_srv_count() - 1);
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    slot_close(i);
                    xSemaphoreGive(s_lock);
                }
            }
        }
    }

fail:
    if (s_listen_fd >= 0) { close(s_listen_fd); s_listen_fd = -1; }
    close_all_clients();
    ESP_LOGI(TAG, "网络服务已停止");
    s_running = false;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* TCP 客户端模式任务                                                  */
/* ------------------------------------------------------------------ */
static void client_mode_task(void *arg)
{
    app_cfg_t *c = app_cfg();
    static client_slot_t fake;
    static uint8_t buf[NET_RX_BUF];

    snprintf(s_cli_peer, sizeof(s_cli_peer), "%s:%u", c->remote_host, c->remote_port);

    while (s_running) {
        struct sockaddr_in srv = {0};
        srv.sin_family = AF_INET;
        srv.sin_port = htons(c->remote_port);
        if (inet_aton(c->remote_host, &srv.sin_addr) != 1) {
            ESP_LOGE(TAG, "无效的服务器地址: %s", c->remote_host);
            break;
        }

        int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(3000)); continue; }

        ESP_LOGI(TAG, "连接服务器 %s ...", s_cli_peer);
        if (lwip_connect(fd, (struct sockaddr *)&srv, sizeof(srv)) == 0) {
            struct timeval rtv = { .tv_sec = 1, .tv_usec = 0 };
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

            s_cli_fd = fd;
            fake.fd = fd;
            fake.is_telnet = false;
            strlcpy(fake.peer, s_cli_peer, sizeof(fake.peer));
            fake.ch = bridge_channel_register(client_write_cb, &fake, "tcp-client");
            ESP_LOGI(TAG, "已连上 %s", s_cli_peer);

            while (s_running) {
                int n = lwip_read(fd, buf, sizeof(buf));
                if (n == 0) break;                          /* 对端关闭 */
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    break;
                }
                bridge_net_rx(fake.ch, buf, (size_t)n);     /* -> 串口 */
            }

            if (fake.ch) { bridge_channel_unregister(fake.ch); fake.ch = NULL; }
            s_cli_fd = -1;
        }

        close(fd);
        ESP_LOGW(TAG, "与 %s 断开, 3s 后重试", s_cli_peer);
        for (int i = 0; i < 30 && s_running; i++) vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_cli_peer[0] = '\0';
    s_running = false;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* 公共接口                                                            */
/* ------------------------------------------------------------------ */
void net_srv_get_state(net_srv_state_t *out)
{
    if (!out) return;
    app_cfg_t *c = app_cfg();
    out->running = s_running;
    out->mode    = c->net_mode;
    out->port    = c->net_port;
    out->client_count = net_srv_count();
    strlcpy(out->remote, s_cli_peer, sizeof(out->remote));
}

void net_srv_broadcast_line(const char *line)
{
    static char buf[256];
    int len = snprintf(buf, sizeof(buf), "%s\r\n", line);
    if (len <= 0) return;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;

    for (int i = 0; i < NET_MAX_CLIENTS; i++) {
        if (s_clients[i].fd >= 0)
            lwip_write(s_clients[i].fd, buf, (size_t)len);
    }
    if (s_cli_fd >= 0)
        lwip_write(s_cli_fd, buf, (size_t)len);
}

esp_err_t net_srv_start(void)
{
    if (s_running) return ESP_OK;

    app_cfg_t *c = app_cfg();
    if (c->net_mode == NET_MODE_NONE) {
        ESP_LOGI(TAG, "网络服务已关闭(NET_MODE_NONE), 只保留网页终端");
        return ESP_OK;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        for (int i = 0; i < NET_MAX_CLIENTS; i++) {
            s_clients[i].fd = -1;
            s_clients[i].ch = NULL;
        }
    }
    s_running = true;

    if (c->net_mode == NET_MODE_TCP_CLIENT) {
        if (c->remote_host[0] == '\0') {
            ESP_LOGW(TAG, "TCP 客户端模式但未配置 remote_host, 不启动");
            s_running = false;
            return ESP_ERR_INVALID_STATE;
        }
        if (xTaskCreate(client_mode_task, "net_cli", NET_TASK_STACK, NULL, 4,
                        &s_task) != pdPASS) {
            s_running = false;
            return ESP_FAIL;
        }
    } else {
        if (xTaskCreate(server_task, "net_srv", NET_TASK_STACK, NULL, 4,
                        &s_task) != pdPASS) {
            s_running = false;
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t net_srv_stop(void)
{
    if (!s_running) return ESP_OK;

    s_running = false;
    /* 只置标志 + shutdown 唤醒; fd 的 close 由任务自身的退出路径统一完成,
     * 避免与任务并发 close 同一 fd 造成误关后续复用的 fd */
    if (s_listen_fd >= 0) {
        shutdown(s_listen_fd, 0);
    }
    if (s_cli_fd >= 0) {
        shutdown(s_cli_fd, 0);
    }
    close_all_clients();
    /* 任务在下一次 select 超时/错误后自行退出 */
    return ESP_OK;
}

esp_err_t net_srv_restart(void)
{
    net_srv_stop();
    vTaskDelay(pdMS_TO_TICKS(300));
    return net_srv_start();
}
