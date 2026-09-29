/*
 * web_server.c - 内置 HTTP 服务器
 *
 * 端点一览:
 *   GET  /              单页前端(内嵌 HTML)
 *   GET  /api/status    状态 JSON
 *   GET  /api/config    配置 JSON
 *   POST /api/config    保存配置(JSON), 应用并按需重启相关服务
 *   POST /api/wol       { "index": N } 唤醒指定目标
 *   POST /api/reboot    重启设备
 *   GET  /api/log       环形日志
 *   POST /api/log/clear 清空日志
 *   GET  /ota           OTA 页面
 *   POST /ota/upload    固件上传(裸 .bin)
 *   WS   /ws            WebSocket 串口终端
 */
#include "web_server.h"
#include "app_cfg.h"
#include "bridge.h"
#include "serial_port.h"
#include "wifi_mgr.h"
#include "net_srv.h"
#include "mqtt_bridge.h"
#include "wol.h"
#include "log_ring.h"

#include <string.h>
#include <stdio.h>
#include <sys/param.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "http";

#define REST_BUF_SIZE 1024

static httpd_handle_t s_server = NULL;

/* WebSocket 会话(允许多个, 每个是一个 bridge 通道) */
#define WS_MAX_SESSIONS 4
typedef struct {
    int               fd;   /* socket fd, -1 = 空槽 */
    bridge_channel_t *ch;
    httpd_handle_t    hd;
} ws_session_t;
static ws_session_t s_ws[WS_MAX_SESSIONS];

/* ------------------------------------------------------------------ */
/* 鉴权: 密码为空则放行; 否则要求 Basic 或 ?pw= 参数                    */
/* ------------------------------------------------------------------ */
static bool web_requires_auth_impl(httpd_req_t *req)
{
    app_cfg_t *c = app_cfg();
    if (c->password[0] == '\0') return true;    /* 无密码, 直接放行 */

    /* 1. ?pw=xxx */
    char qpw[40] = {0};
    if (httpd_req_get_url_query_str(req, qpw, sizeof(qpw)) == ESP_OK) {
        char val[40] = {0};
        if (httpd_query_key_value(qpw, "pw", val, sizeof(val)) == ESP_OK &&
            strcmp(val, c->password) == 0)
            return true;
    }

    /* 2. Basic 认证 */
    char hdr[96];
    if (httpd_req_get_hdr_value_len(req, "Authorization") > 0 &&
        httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK) {
        /* 形如 "Basic dXNlcjpwYXNz" — 校验 base64(任意用户名:密码) */
        if (strncmp(hdr, "Basic ", 6) == 0) {
            /* 就地解码 base64 */
            const char *b = hdr + 6;
            static const char T[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            uint8_t dec[64];
            size_t o = 0, bits = 0; uint32_t acc = 0;
            for (const char *p = b; *p && *p != '=' && o < sizeof(dec); p++) {
                const char *hit = strchr(T, *p);
                if (!hit) continue;
                acc = (acc << 6) | (uint32_t)(hit - T);
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    dec[o++] = (uint8_t)((acc >> bits) & 0xFF);
                }
            }
            /* 期望 "user:password", 只校验 password 部分 */
            char *colon = memchr(dec, ':', o);
            if (colon) {
                size_t plen = o - (size_t)(colon - (char *)dec) - 1;
                if (plen == strlen(c->password) &&
                    memcmp(colon + 1, c->password, plen) == 0)
                    return true;
            }
        }
    }
    return false;
}

bool web_requires_auth(httpd_req_t *req)
{
    if (web_requires_auth_impl(req)) return true;

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"serial\"");
    httpd_resp_sendstr(req, "unauthorized");
    return false;
}

/* ------------------------------------------------------------------ */
/* 工具: JSON 应答                                                     */
/* ------------------------------------------------------------------ */
static esp_err_t resp_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t resp_err(httpd_req_t *req, const char *msg)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, "400 Bad Request");
    return resp_json(req, buf);
}

static esp_err_t resp_ok(httpd_req_t *req)
{
    return resp_json(req, "{\"ok\":true}");
}

/* 读取 POST body */
static char *read_body(httpd_req_t *req)
{
    size_t total = req->content_len;
    if (total == 0 || total > REST_BUF_SIZE) return NULL;
    char *buf = malloc(total + 1);
    if (!buf) return NULL;
    size_t got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, buf + got, total - got);
        if (n <= 0) { free(buf); return NULL; }
        got += (size_t)n;
    }
    buf[total] = '\0';
    return buf;
}

/* 安全读字符串字段 */
static void json_str(cJSON *root, const char *key, char *dst, size_t dst_sz)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(v) && v->valuestring)
        strlcpy(dst, v->valuestring, dst_sz);
}

static void json_int(cJSON *root, const char *key, int *dst)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (cJSON_IsNumber(v)) *dst = (int)v->valuedouble;
}

static void json_bool(cJSON *root, const char *key, bool *dst)
{
    cJSON *v = cJSON_GetObjectItem(root, key);
    if (cJSON_IsBool(v)) *dst = cJSON_IsTrue(v);
}

/* ------------------------------------------------------------------ */
/* /api/status                                                         */
/* ------------------------------------------------------------------ */
static esp_err_t handler_status(httpd_req_t *req)
{
    if (!web_requires_auth(req)) return ESP_OK;

    wifi_state_t w;
    wifi_mgr_get_state(&w);
    net_srv_state_t ns;
    net_srv_get_state(&ns);
    mqtt_state_t mq;
    mqtt_bridge_get_state(&mq);
    bridge_stats_t bs;
    bridge_get_stats(&bs);

    char *j = malloc(768);
    if (!j) return resp_err(req, "oom");

    snprintf(j, 768,
        "{\"version\":\"%s\",\"uptime_s\":%lld,"
        "\"wifi\":{\"connected\":%s,\"ip\":\"%s\",\"ap\":\"%s\",\"retries\":%d},"
        "\"net\":{\"running\":%s,\"mode\":\"%s\",\"port\":%u,\"clients\":%d,\"remote\":\"%s\"},"
        "\"mqtt\":{\"enabled\":%s,\"connected\":%s},"
        "\"serial\":{\"side\":\"%s\",\"desc\":\"%s\",\"nl_xlate\":%d},"
        "\"bridge\":{\"ser_rx\":%lu,\"ser_tx\":%lu,\"dropped\":%lu}}",
        APP_VERSION, (long long)(esp_timer_get_time() / 1000000),
        w.sta_connected ? "true" : "false", w.ip,
        app_cfg()->ap_en ? "on" : "off", w.sta_retry_count,
        ns.running ? "true" : "false",
        app_cfg_net_mode_str(ns.mode), (unsigned)ns.port,
        ns.client_count, ns.remote,
        mq.enabled ? "true" : "false", mq.connected ? "true" : "false",
        app_cfg_serial_side_str(app_cfg()->serial_side),
        serial_port_desc(), app_cfg()->nl_xlate,
        (unsigned long)bs.ser_rx, (unsigned long)bs.ser_tx,
        (unsigned long)bs.dropped);

    esp_err_t r = resp_json(req, j);
    free(j);
    return r;
}

/* ------------------------------------------------------------------ */
/* /api/config  GET/POST                                               */
/* ------------------------------------------------------------------ */
static esp_err_t handler_config_get(httpd_req_t *req)
{
    if (!web_requires_auth(req)) return ESP_OK;

    app_cfg_t *c = app_cfg();
    char *j = malloc(2048);
    if (!j) return resp_err(req, "oom");

    /* wol 数组 */
    char wol[600] = "[]";
    if (c->wol_count > 0) {
        char *p = wol;
        size_t left = sizeof(wol) - 1;
        int w = snprintf(p, left, "[");
        p += w; left -= (size_t)w;
        for (int i = 0; i < c->wol_count && left > 60; i++) {
            char mac[18];
            app_cfg_mac_str(c->wol[i].mac, mac);
            w = snprintf(p, left, "%s{\"name\":\"%s\",\"mac\":\"%s\",\"ip\":\"%s\"}",
                         i ? "," : "", c->wol[i].name, mac, c->wol[i].ip);
            p += w; left -= (size_t)w;
        }
        snprintf(p, left, "]");
    }

    snprintf(j, 2048,
        "{\"wifi_ssid\":\"%s\",\"wifi_pass\":\"%s\",\"ap_en\":%s,\"ap_pass\":\"%s\","
        "\"hostname\":\"%s\",\"password\":\"%s\","
        "\"serial_side\":%u,\"uart_baud\":%d,\"uart_tx_gpio\":%d,\"uart_rx_gpio\":%d,\"nl_xlate\":%d,"
        "\"net_mode\":%u,\"net_port\":%u,\"remote_host\":\"%s\",\"remote_port\":%u,"
        "\"mqtt_en\":%s,\"mqtt_uri\":\"%s\",\"mqtt_user\":\"%s\",\"mqtt_prefix\":\"%s\","
        "\"wol\":%s,\"wol_count\":%d}",
        c->wifi_ssid, c->wifi_pass, c->ap_en ? "true" : "false", c->ap_pass,
        c->hostname, c->password,
        (unsigned)c->serial_side, (int)c->uart_baud, (int)c->uart_tx_gpio,
        (int)c->uart_rx_gpio, (int)c->nl_xlate,
        (unsigned)c->net_mode, (unsigned)c->net_port, c->remote_host,
        (unsigned)c->remote_port,
        c->mqtt_en ? "true" : "false", c->mqtt_uri, c->mqtt_user, c->mqtt_prefix,
        wol, (int)c->wol_count);

    esp_err_t r = resp_json(req, j);
    free(j);
    return r;
}

/* POST /api/config: 应用差量并按需重启服务 */
static esp_err_t handler_config_post(httpd_req_t *req)
{
    if (!web_requires_auth(req)) return ESP_OK;

    char *body = read_body(req);
    if (!body) return resp_err(req, "bad body");

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return resp_err(req, "bad json");

    app_cfg_t *c = app_cfg();
    app_cfg_t old = *c;   /* 对比哪些块变化了 */

    json_str(root, "wifi_ssid", c->wifi_ssid, sizeof(c->wifi_ssid));
    json_str(root, "wifi_pass", c->wifi_pass, sizeof(c->wifi_pass));
    json_bool(root, "ap_en", &c->ap_en);
    json_str(root, "ap_pass", c->ap_pass, sizeof(c->ap_pass));
    json_str(root, "hostname", c->hostname, sizeof(c->hostname));
    json_str(root, "password", c->password, sizeof(c->password));

    int side = c->serial_side, baud = (int)c->uart_baud, tx = (int)c->uart_tx_gpio,
        rx = (int)c->uart_rx_gpio, nl = (int)c->nl_xlate;
    json_int(root, "serial_side", &side);
    json_int(root, "uart_baud", &baud);
    json_int(root, "uart_tx_gpio", &tx);
    json_int(root, "uart_rx_gpio", &rx);
    json_int(root, "nl_xlate", &nl);
    if (side >= 0) c->serial_side = (uint8_t)side;
    if (baud > 0) c->uart_baud = baud;
    if (tx >= 0) c->uart_tx_gpio = tx;
    if (rx >= 0) c->uart_rx_gpio = rx;
    if (nl >= 0) c->nl_xlate = (uint8_t)nl;

    int mode = c->net_mode, port = (int)c->net_port, rport = (int)c->remote_port;
    json_int(root, "net_mode", &mode);
    json_int(root, "net_port", &port);
    json_int(root, "remote_port", &rport);
    if (mode >= 0) c->net_mode = (uint8_t)mode;
    if (port > 0) c->net_port = (uint16_t)port;
    if (rport > 0) c->remote_port = (uint16_t)rport;
    json_str(root, "remote_host", c->remote_host, sizeof(c->remote_host));

    /* MQTT 块 */
    cJSON *mq = cJSON_GetObjectItem(root, "mqtt");
    if (mq) {
        json_bool(mq, "enabled", &c->mqtt_en);
        json_str(mq, "uri", c->mqtt_uri, sizeof(c->mqtt_uri));
        json_str(mq, "user", c->mqtt_user, sizeof(c->mqtt_user));
        json_str(mq, "pass", c->mqtt_pass, sizeof(c->mqtt_pass));
        json_str(mq, "prefix", c->mqtt_prefix, sizeof(c->mqtt_prefix));
    }
    /* WOL 块 */
    cJSON *wolv = cJSON_GetObjectItem(root, "wol");
    if (wolv && cJSON_IsArray(wolv)) {
        int n = cJSON_GetArraySize(wolv);
        if (n > CFG_WOL_MAX) n = CFG_WOL_MAX;
        c->wol_count = 0;
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(wolv, i);
            if (!it) continue;
            wol_target_t *t = &c->wol[c->wol_count];
            memset(t, 0, sizeof(*t));
            char mac[24] = {0};
            json_str(it, "name", t->name, sizeof(t->name));
            json_str(it, "mac", mac, sizeof(mac));
            json_str(it, "ip", t->ip, sizeof(t->ip));
            if (app_cfg_parse_mac(mac, t->mac)) c->wol_count++;
        }
    }

    cJSON_Delete(root);

    /* 保存 */
    esp_err_t err = app_cfg_save();
    if (err != ESP_OK) return resp_err(req, "save failed");

    /* 按差量应用 */
    if (strcmp(old.wifi_ssid, c->wifi_ssid) != 0 ||
        strcmp(old.wifi_pass, c->wifi_pass) != 0) {
        wifi_mgr_apply_sta();
    }
    if (memcmp(&old.serial_side, &c->serial_side,
               offsetof(app_cfg_t, net_mode) - offsetof(app_cfg_t, serial_side)) != 0) {
        ESP_LOGW(TAG, "串口参数变更需重启后生效");
    }
    if (old.net_mode != c->net_mode || old.net_port != c->net_port ||
        strcmp(old.remote_host, c->remote_host) != 0 || old.remote_port != c->remote_port) {
        net_srv_restart();
    }
    if (old.mqtt_en != c->mqtt_en ||
        strcmp(old.mqtt_uri, c->mqtt_uri) != 0 ||
        strcmp(old.mqtt_prefix, c->mqtt_prefix) != 0) {
        mqtt_bridge_restart();
    }
    bridge_set_nl_xlate(c->nl_xlate);

    return resp_ok(req);
}

/* ------------------------------------------------------------------ */
/* /api/wol  /api/reboot  /api/log                                     */
/* ------------------------------------------------------------------ */
static esp_err_t handler_wol(httpd_req_t *req)
{
    if (!web_requires_auth(req)) return ESP_OK;

    char *body = read_body(req);
    if (!body) return resp_err(req, "bad body");
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return resp_err(req, "bad json");

    esp_err_t err;
    cJSON *all = cJSON_GetObjectItem(root, "all");
    if (all && cJSON_IsTrue(all)) {
        err = wol_wake_all();
    } else {
        int idx = -1;
        json_int(root, "index", &idx);
        err = (idx >= 0) ? wol_wake((uint8_t)idx) : ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(root);

    if (err == ESP_OK) return resp_ok(req);
    if (err == ESP_ERR_NOT_FOUND) return resp_err(req, "no target");
    return resp_err(req, "send failed");
}

static esp_err_t handler_reboot(httpd_req_t *req)
{
    if (!web_requires_auth(req)) return ESP_OK;
    resp_ok(req);
    ESP_LOGW(TAG, "网页请求重启, 1s 后执行...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handler_log(httpd_req_t *req)
{
    if (!web_requires_auth(req)) return ESP_OK;
    char *buf = malloc(4096);
    if (!buf) return resp_err(req, "oom");
    size_t n = log_ring_dump(buf, 4096);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, buf, (ssize_t)n);
    free(buf);
    return ESP_OK;
}

static esp_err_t handler_log_clear(httpd_req_t *req)
{
    if (!web_requires_auth(req)) return ESP_OK;
    log_ring_clear();
    return resp_ok(req);
}

/* ------------------------------------------------------------------ */
/* WebSocket 串口终端 (/ws)                                            */
/* ------------------------------------------------------------------ */
static void ws_cleanup(int fd);   /* 前向声明: 写失败时自清理要用 */

static int ws_write_cb(bridge_channel_t *ch, const uint8_t *data, size_t len)
{
    ws_session_t *ws = (ws_session_t *)ch->owner;

    /* 串口数据推给浏览器: 文本帧, base64 后 JSON 包装 */
    static char b64[1025];
    size_t n = 0;
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < len && n + 4 < sizeof(b64) - 1; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        b64[n++] = T[(v >> 18) & 0x3F];
        b64[n++] = T[(v >> 12) & 0x3F];
        b64[n++] = (i + 1 < len) ? T[(v >> 6) & 0x3F] : '=';
        b64[n++] = (i + 2 < len) ? T[v & 0x3F] : '=';
    }
    b64[n] = '\0';

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)b64,
        .len = n,
    };
    if (httpd_ws_send_frame_async(ws->hd, ws->fd, &frame) != ESP_OK) {
        /* 浏览器异常断开(无 CLOSE 帧): 写失败即回收会话, 防止槽位泄漏 */
        ws_cleanup(ws->fd);
        return -1;
    }
    return (int)len;
}

static int ws_find_slot(int fd)
{
    for (int i = 0; i < WS_MAX_SESSIONS; i++)
        if (s_ws[i].fd == fd) return i;
    return -1;
}

static void ws_cleanup(int fd)
{
    int i = ws_find_slot(fd);
    if (i < 0) return;
    if (s_ws[i].ch) bridge_channel_unregister(s_ws[i].ch);
    s_ws[i].fd = -1;
    s_ws[i].ch = NULL;
}

static esp_err_t handler_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* ---- WS 握手阶段 ---- */
        if (!web_requires_auth(req)) return ESP_OK;

        int slot = -1;
        for (int i = 0; i < WS_MAX_SESSIONS; i++) {
            if (s_ws[i].fd < 0) { slot = i; break; }
        }
        if (slot < 0) {
            httpd_resp_set_status(req, "503 Service Unavailable");
            return httpd_resp_sendstr(req, "ws sessions full");
        }

        s_ws[slot].fd = httpd_req_to_sockfd(req);
        s_ws[slot].hd = req->handle;
        s_ws[slot].ch = bridge_channel_register(ws_write_cb, &s_ws[slot], "ws");
        if (!s_ws[slot].ch) {
            s_ws[slot].fd = -1;
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "bridge full");
        }

        /* 注意: 此处不能发 WS 帧 — 101 握手响应还未发出, 异步帧会导致协议错误 */
        ESP_LOGI(TAG, "WebSocket 终端接入 fd=%d", s_ws[slot].fd);
        return ESP_OK;   /* 返回后框架自动完成 101 握手 */
    }

    /* ---- POST: WS 数据帧(握手后) ---- */
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) {
        return ESP_FAIL;
    }

    int fd = httpd_req_to_sockfd(req);

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ws_cleanup(fd);
        return httpd_ws_send_frame_async(req->handle, fd, &frame);
    }

    if (frame.len == 0) return ESP_OK;

    uint8_t *buf = malloc(frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    if (httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) {
        free(buf);
        return ESP_FAIL;
    }
    buf[frame.len] = '\0';

    if (frame.type == HTTPD_WS_TYPE_TEXT) {
        static const char T[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        static uint8_t dec[1024];
        size_t o = 0, bits = 0;
        uint32_t acc = 0;
        for (char *p = (char *)buf; *p && o < sizeof(dec); p++) {
            if (*p == '=') break;
            const char *hit = strchr(T, *p);
            if (!hit) continue;
            acc = (acc << 6) | (uint32_t)(hit - T);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (o >= sizeof(dec)) break;
                dec[o++] = (uint8_t)((acc >> bits) & 0xFF);
            }
        }
        if (o > 0) bridge_net_rx(NULL, dec, o);
    }

    free(buf);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 网页 OTA 升级 (/ota, /ota/upload)                                   */
/* ------------------------------------------------------------------ */
#define OTA_BUF_SIZE  4096

static const char OTA_PAGE[] =
"<!doctype html><meta charset=utf-8>"
"<title>固件升级</title>"
"<body style=font-family:sans-serif;max-width:520px;margin:40px auto>"
"<h3>固件 OTA 升级</h3>"
"<p>选择编译产物 build/xxx.bin 后点击升级。升级期间请勿断电。</p>"
"<input type=file id=f accept=.bin><br><br>"
"<button onclick=up()>开始升级</button> <span id=m></span>"
"<script>async function up(){let f=document.getElementById('f').files[0];if(!f){alert('请选择文件');return;}"
"document.getElementById('m').textContent='上传中...';"
"let r=await fetch('/ota/upload',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});"
"document.getElementById('m').textContent=r.ok?'完成, 设备重启中':'失败 '+r.status;}"
"</script>";

static esp_err_t handler_ota_page(httpd_req_t *req)
{
    if (!web_requires_auth(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, OTA_PAGE);
}

static esp_err_t handler_ota_upload(httpd_req_t *req)
{
    if (!web_requires_auth(req)) return ESP_OK;

    esp_err_t err;
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        return resp_err(req, "no ota partition");
    }

    esp_ota_handle_t handle;
    err = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin 失败: %s", esp_err_to_name(err));
        return resp_err(req, "ota begin failed");
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(handle);
        return resp_err(req, "oom");
    }

    int  received = 0;
    bool ota_ok = true;

    while ((received = httpd_req_recv(req, buf, OTA_BUF_SIZE)) > 0) {
        err = esp_ota_write(handle, buf, (size_t)received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write 失败: %s", esp_err_to_name(err));
            ota_ok = false;
            break;
        }
    }
    free(buf);

    if (received < 0) {
        esp_ota_abort(handle);
        return resp_err(req, "recv failed");
    }

    if (!ota_ok) {
        esp_ota_abort(handle);
        return resp_err(req, "ota write failed");
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end 失败: %s", esp_err_to_name(err));
        return resp_err(req, "ota end failed (image invalid)");
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition 失败: %s", esp_err_to_name(err));
        return resp_err(req, "set boot failed");
    }

    ESP_LOGW(TAG, "OTA 升级成功, 2s 后重启...");
    resp_json(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 内嵌单页前端 (/)                                                    */
/* ------------------------------------------------------------------ */
static const char INDEX_HTML[] =
"<!doctype html><html lang=zh><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>ESP32C3 无线串口</title>"
"<style>"
"*{box-sizing:border-box}"
"body{font-family:-apple-system,sans-serif;margin:0;background:#f2f3f5;color:#222}"
".wrap{max-width:820px;margin:0 auto;padding:16px}"
"h2{margin:8px 0 16px}"
".card{background:#fff;border-radius:10px;padding:16px;margin-bottom:14px;box-shadow:0 1px 3px rgba(0,0,0,.08)}"
"h3{margin:0 0 12px;font-size:15px}"
".grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}"
"label{font-size:12px;color:#666;display:block;margin-bottom:4px}"
"input,select{width:100%;padding:7px;border:1px solid #ccd;border-radius:6px;font-size:13px}"
"button{padding:8px 16px;border:0;border-radius:6px;background:#2b6cff;color:#fff;cursor:pointer;font-size:13px}"
"button.sec{background:#e4e8ef;color:#333}"
"button.danger{background:#e05252}"
"#term{width:100%;height:260px;background:#101317;color:#d8f0d8;border:0;border-radius:6px;"
"font:12px/1.5 Menlo,monospace;padding:10px;overflow:auto;white-space:pre-wrap}"
".row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}"
".pill{font-size:12px;padding:3px 9px;border-radius:12px;background:#eef;color:#335}"
".st{font-size:13px;line-height:1.9}"
"#msg{font-size:12px;color:#2b7;min-height:16px}"
"</style></head><body><div class=wrap>"
"<h2>ESP32-C3 无线串口 <span class=pill id=ver></span></h2>"

"<div class=card><h3>运行状态</h3><div class=st id=status>加载中...</div></div>"

"<div class=card><h3>网页串口终端 <button class=sec onclick=termToggle() id=termBtn>连接</button></h3>"
"<div id=term></div></div>"

"<div class=card><h3>网络唤醒</h3><div class=row>"
"<button class=sec onclick=wolAll()>全部唤醒</button></div></div>"

"<div class=card><h3>配置</h3>"
"<div class=grid>"
"<div><label>Wi-Fi SSID</label><input id=wifi_ssid></div>"
"<div><label>Wi-Fi 密码</label><input id=wifi_pass></div>"
"<div><label>主机名</label><input id=hostname></div>"
"<div><label>管理/数据密码</label><input id=password></div>"
"<div><label>网络模式</label><select id=net_mode>"
"<option value=0>TCP 服务端</option><option value=1>TCP 客户端</option>"
"<option value=2>Telnet</option><option value=3>关闭</option></select></div>"
"<div><label>本地端口</label><input id=net_port type=number></div>"
"<div><label>远端主机</label><input id=remote_host></div>"
"<div><label>远端端口</label><input id=remote_port type=number></div>"
"<div><label>串口侧</label><select id=serial_side>"
"<option value=0>USB-CDC</option><option value=1>UART0</option><option value=2>UART1</option></select></div>"
"<div><label>波特率</label><input id=uart_baud type=number></div>"
"</div><p></p>"
"<div class=row><button onclick=saveCfg()>保存配置</button>"
"<button class=sec onclick=location.href='/ota'>固件升级</button>"
"<button class=danger onclick=doReboot()>重启设备</button>"
"<span id=msg></span></div></div>"
"</div>"

"<script>"
"let ws=null,termOn=false;"
"async function getJSON(u){let r=await fetch(u);return r.json();}"
"async function load(){"
"let s=await getJSON('/api/status');"
"document.getElementById('ver').textContent='v'+s.version;"
"document.getElementById('status').innerHTML="
"'IP: <b>'+s.wifi.ip+'</b> &nbsp; Wi-Fi: '+s.wifi.connected+"
"' &nbsp; 网络: '+s.net.mode+':'+s.net.port+' ('+s.net.clients+')'+"
"' &nbsp; 串口: '+s.serial.side+' &nbsp; 运行: '+s.uptime_s+'s';"
"let c=await getJSON('/api/config');"
"for(let k of ['wifi_ssid','wifi_pass','hostname','password','net_port','remote_host','remote_port','uart_baud'])"
"document.getElementById(k).value=c[k]||'';"
"document.getElementById('net_mode').value=c.net_mode;"
"document.getElementById('serial_side').value=c.serial_side;"
"}"
"function b64ToStr(b){return decodeURIComponent(escape(atob(b)));}"
"function termToggle(){if(termOn){ws&&ws.close();termOn=false;document.getElementById('termBtn').textContent='连接';return;}"
"let proto=location.protocol==='https:'?'wss':'ws';"
"ws=new WebSocket(proto+'://'+location.host+'/ws');termOn=true;"
"document.getElementById('termBtn').textContent='断开';"
"ws.onmessage=e=>{if(e.data==='connected')return;let t=document.getElementById('term');"
"t.textContent+=b64ToStr(e.data);t.scrollTop=t.scrollHeight;};"
"}"
"async function saveCfg(){let c={};"
"for(let k of ['wifi_ssid','wifi_pass','hostname','password','net_port','remote_host','remote_port','uart_baud'])"
"c[k]=document.getElementById(k).value;"
"c.net_mode=+document.getElementById('net_mode').value;"
"c.serial_side=+document.getElementById('serial_side').value;"
"let r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(c)});"
"document.getElementById('msg').textContent=r.ok?'已保存':'失败';setTimeout(()=>load(),800);}"
"async function wolAll(){await fetch('/api/wol',{method:'POST',headers:{'Content-Type':'application/json'},body:'{\"all\":true}'});}"
"async function doReboot(){if(confirm('确认重启?'))await fetch('/api/reboot',{method:'POST'});}"
"load();setInterval(load,5000);"
"</script></body></html>";

static esp_err_t handler_index(httpd_req_t *req)
{
    /* 首页无需鉴权(配网场景), 但配置/状态 API 各自鉴权 */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, INDEX_HTML);
}

/* ------------------------------------------------------------------ */
/* URI 注册 / 启动                                                     */
/* ------------------------------------------------------------------ */
esp_err_t web_server_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务器启动失败");
        return ESP_FAIL;
    }

    for (int i = 0; i < WS_MAX_SESSIONS; i++) {
        s_ws[i].fd = -1;
        s_ws[i].ch = NULL;
    }

    static const httpd_uri_t uri_index = {
        .uri = "/", .method = HTTP_GET, .handler = handler_index };
    static const httpd_uri_t uri_status = {
        .uri = "/api/status", .method = HTTP_GET, .handler = handler_status };
    static const httpd_uri_t uri_cfg_get = {
        .uri = "/api/config", .method = HTTP_GET, .handler = handler_config_get };
    static const httpd_uri_t uri_cfg_post = {
        .uri = "/api/config", .method = HTTP_POST, .handler = handler_config_post };
    static const httpd_uri_t uri_wol = {
        .uri = "/api/wol", .method = HTTP_POST, .handler = handler_wol };
    static const httpd_uri_t uri_reboot = {
        .uri = "/api/reboot", .method = HTTP_POST, .handler = handler_reboot };
    static const httpd_uri_t uri_log = {
        .uri = "/api/log", .method = HTTP_GET, .handler = handler_log };
    static const httpd_uri_t uri_log_clear = {
        .uri = "/api/log/clear", .method = HTTP_POST, .handler = handler_log_clear };
    static const httpd_uri_t uri_ota_page = {
        .uri = "/ota", .method = HTTP_GET, .handler = handler_ota_page };
    static const httpd_uri_t uri_ota_upload = {
        .uri = "/ota/upload", .method = HTTP_POST, .handler = handler_ota_upload };
    static const httpd_uri_t uri_ws = {
        .uri = "/ws", .method = HTTP_GET, .handler = handler_ws,
        .is_websocket = true };

    httpd_register_uri_handler(s_server, &uri_index);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_cfg_get);
    httpd_register_uri_handler(s_server, &uri_cfg_post);
    httpd_register_uri_handler(s_server, &uri_wol);
    httpd_register_uri_handler(s_server, &uri_reboot);
    httpd_register_uri_handler(s_server, &uri_log);
    httpd_register_uri_handler(s_server, &uri_log_clear);
    httpd_register_uri_handler(s_server, &uri_ota_page);
    httpd_register_uri_handler(s_server, &uri_ota_upload);
    httpd_register_uri_handler(s_server, &uri_ws);

    ESP_LOGI(TAG, "Web 管理界面已启动 (端口 80)");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (!s_server) return ESP_OK;
    httpd_stop(s_server);
    s_server = NULL;
    return ESP_OK;
}
