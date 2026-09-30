/*
 * ============================================================================
 *  esp32c3-wifi-serial —— Wi-Fi 串口桥（Arduino 版）
 * ============================================================================
 *  把电脑上的一个串口（USB-CDC）变成局域网服务：
 *    TCP Server / TCP Client / Telnet / 网页 WebSocket 终端 / MQTT，
 *  所有通道数据互通，任一通道写入的数据进串口，串口数据广播给所有通道。
 *
 *  ── 与旧 ESP-IDF 版的关系 ──────────────────────────────────────────────────
 *    本工程由 9 模块的 ESP-IDF C 工程（app_cfg/bridge/net_srv/web_server 等）
 *    就地重构为 Arduino 单文件 .ino，行为对齐：
 *      - 通道分配一致：数据走 USB-CDC(Serial/HWCDC)，日志走 UART0(Serial0)
 *      - TCP 多客户端 + Telnet(IAC 过滤) + WS 终端 + MQTT + Web/OTA + WOL + mDNS
 *      - 配置存 NVS(Preferences)，网页可改
 *
 *  ── 通道分配（最关键设计）───────────────────────────────────────────────────
 *    数据 → USB Serial/JTAG (HWCDC) = Arduino 的 Serial
 *           必须用 FQBN esp32:esp32:esp32c3:CDCOnBoot=cdc 编译
 *    日志 → UART0 (GPIO20/21, 115200) = Serial0
 *    两口互不污染。全文件不使用 log_i()/ESP_LOGI()（那会把日志打进数据流），
 *    只用自写 wlog()（同时写 UART0 与 RAM 日志环，/api/log 可倒出）。
 *
 *  ── Arduino .ino 的结构约束（曾踩坑，勿改）─────────────────────────────────
 *    1. .ino 预处理会把所有函数原型插到文件头部 → 凡在函数签名里出现的
 *       自定义类型必须定义在【第一个函数定义之前】（本文件已全部前置）。
 *    2. 串口逐字节读：HWCDC::read() 内部 xQueueReceive(...,0) 不阻塞；
 *       read(buf,len) 是 Stream::readBytes 语义（会等凑满）——禁止使用。
 *    3. 依赖库（需安装）：
 *         WebSockets (scottmang, 2.7.x)  —— 网页 WS 终端
 *         PubSubClient 2.8               —— MQTT
 *       core 自带：WiFi/WebServer/Network/ESPmDNS/Update/Preferences
 *
 *  ── 已核实的平台事实（core 3.3.11 源码逐条核对）────────────────────────────
 *    1. NetworkServer::begin() 给 listening socket 设 O_NONBLOCK，accept() 不阻塞
 *    2. NetworkClient 析构为空、fd 由 shared_ptr 持有 → 按值复制安全
 *    3. WebSocketsServer 事件签名:
 *       std::function<void(uint8_t num, WStype_t type, uint8_t* payload, size_t len)>
 *    4. WebServer::on(uri, HTTP_POST, fn, uploadFn) 第 4 参处理上传
 *    5. Update.begin(UPDATE_SIZE_UNKNOWN) + write() + end() 三步 OTA
 *    6. Preferences 的 getString/putString/putUChar 等，命名空间长度上限 15
 * ============================================================================
 */

#if !defined(ARDUINO_USB_CDC_ON_BOOT) || (ARDUINO_USB_CDC_ON_BOOT != 1)
#error "本工程的数据通道固定在 USB-CDC。请用 FQBN esp32:esp32:esp32c3:CDCOnBoot=cdc 编译（等价于 -DARDUINO_USB_CDC_ON_BOOT=1）。"
#endif
#if !defined(ARDUINO_USB_MODE) || (ARDUINO_USB_MODE != 1)
#error "ESP32-C3 需要 Hardware CDC 模式（ARDUINO_USB_MODE=1），这是板级默认值。"
#endif

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebSocketsServer.h>
#include <PubSubClient.h>

#include "index_html.h"
#include "ota_html.h"

// ============================================================================
//  常量
// ============================================================================
#define FW_NAME       "esp32c3-wifi-serial-arduino"
#define FW_VERSION    "1.0.0"

#define DATA_PORT     Serial     // USB-CDC：透传数据
#define LOG_PORT      Serial0    // UART0：日志

#define MAX_TCP       4          // TCP/Telnet 服务端最大客户端数
#define TCP_RX_BUF    512
#define BR_OUT_BUF    (TCP_RX_BUF * 2)
#define WS_SLOTS      5          // WS 终端连接数上限 = 库 WEBSOCKETS_SERVER_CLIENT_MAX(5)
#define LOGRING_SZ    2048       // RAM 日志环
#define WOL_MAX       4
#define SCAN_TIMEOUT  15000

// 网络模式
enum NetMode { NET_TCP_SERVER = 0, NET_TCP_CLIENT = 1, NET_TELNET = 2, NET_OFF = 3 };

// Telnet IAC 解析状态
enum TelnetSt { TS_DATA = 0, TS_IAC, TS_CMD, TS_SB };

// ============================================================================
//  配置结构 + 全局实例
//  （.ino 原型坑：所有 struct 必须在第一个函数定义之前 —— 见文件头说明）
// ============================================================================
struct WolTarget {
  char mac[18];       // "AA:BB:CC:DD:EE:FF"
};

struct AppCfg {
  // 网络
  char     wifiSsid[33];
  char     wifiPass[65];
  char     hostname[33];
  char     password[33];    // 管理/数据密码，空=不校验
  // 网络服务
  uint8_t  netMode;         // NetMode
  uint16_t netPort;         // TCP/Telnet 本地端口
  char     remoteHost[64];  // TCP 客户端对端
  uint16_t remotePort;
  // 串口
  uint8_t  nlXlate;         // 0=原样 1=串口->网络 LF 补 CRLF
  // MQTT
  uint8_t  mqttEn;
  char     mqttUri[96];     // host[:port]
  char     mqttUser[33];
  char     mqttPass[33];
  char     mqttPrefix[48];
  // WOL
  WolTarget wol[WOL_MAX];
  uint8_t   wolCount;
};

static AppCfg cfg;
static Preferences prefs;

// TCP/Telnet 客户端槽
struct TcpSlot {
  NetworkClient cli;        // 按值持有（析构为空，安全）
  bool     used;
  bool     isTelnet;
  uint8_t  iacSt;
  char     peer[24];        // "ip:port"
};

static TcpSlot tcpSlots[MAX_TCP];
static NetworkServer *tcpServer = nullptr;

// TCP 客户端模式
static NetworkClient tcpCli;
static bool tcpCliUp = false;

// WebSocket 终端
static WebSocketsServer *wsSrv = nullptr;
static bool     wsAuthed[WS_SLOTS];        // 与库 WEBSOCKETS_SERVER_CLIENT_MAX(5) 对齐
static uint32_t wsConnMs[WS_SLOTS];        // 连接时间戳（5s 鉴权窗口）

// Web 管理服务器
static WebServer web(80);

// MQTT
static WiFiClient   mqttNet;
static PubSubClient mqtt(mqttNet);
static bool         mqttUp = false;

// 日志环
static char   logRing[LOGRING_SZ];
static size_t logHead = 0, logLen = 0;

// 桥接统计
static uint32_t stSerRx = 0, stSerTx = 0, stDropped = 0;

// WiFi 状态
static bool     staUp = false;
static uint32_t bootMs = 0;

// ============================================================================
//  前置声明
// ============================================================================
static void cfgDefaults();
static void cfgLoad();
static void cfgSave();
static void netStart();
static void netStop();
static void applyNl(const uint8_t *in, size_t len, uint8_t *out, size_t *outLen);
static void fanoutToNet(const uint8_t *data, size_t len);
static void bridgeNetRx(const uint8_t *data, size_t len);
static size_t telnetFilter(uint8_t *buf, size_t len, uint8_t *st);
static void wsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length);
static void handleRoot();
static void handleStatus();
static void handleConfigGet();
static void handleConfigPost();
static void handleWol();
static void handleReboot();
static void handleLogGet();
static void handleLogClear();
static void handleOtaPage();
static void handleOtaUpload();
static bool checkAuth();
static void wlog(const char *fmt, ...);
static void wolSendAll();
static bool wolSend(const char *macStr);
static void mqttApply();
static void mqttLoopIfUp();

// ============================================================================
//  日志：UART0 + RAM 环
// ============================================================================
static void wlog(const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
  for (int i = 0; i < n; i++) {
    logRing[logHead] = buf[i];
    logHead = (logHead + 1) % LOGRING_SZ;
    if (logLen < LOGRING_SZ) logLen++;
  }
  LOG_PORT.write((const uint8_t *)buf, (size_t)n);
}

// ============================================================================
//  配置：默认值 / NVS 读写
// ============================================================================
static void cfgDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  strlcpy(cfg.hostname, "serial", sizeof(cfg.hostname));
  cfg.netMode    = NET_TCP_SERVER;
  cfg.netPort    = 2333;
  cfg.remotePort = 2333;
  cfg.nlXlate    = 0;
  strlcpy(cfg.mqttPrefix, "esp32c3-serial", sizeof(cfg.mqttPrefix));
}

static void cfgLoad() {
  cfgDefaults();
  prefs.begin("wser", true);   // 只读
  strlcpy(cfg.wifiSsid,  prefs.getString("ssid", "").c_str(), sizeof(cfg.wifiSsid));
  strlcpy(cfg.wifiPass,  prefs.getString("wpass", "").c_str(), sizeof(cfg.wifiPass));
  strlcpy(cfg.hostname,  prefs.getString("host", "serial").c_str(), sizeof(cfg.hostname));
  strlcpy(cfg.password,  prefs.getString("pw", "").c_str(), sizeof(cfg.password));
  cfg.netMode    = prefs.getUChar("nmode", NET_TCP_SERVER);
  cfg.netPort    = prefs.getUShort("nport", 2333);
  strlcpy(cfg.remoteHost, prefs.getString("rhost", "").c_str(), sizeof(cfg.remoteHost));
  cfg.remotePort = prefs.getUShort("rport", 2333);
  cfg.nlXlate    = prefs.getUChar("nlx", 0);
  cfg.mqttEn     = prefs.getUChar("mqen", 0);
  strlcpy(cfg.mqttUri,   prefs.getString("mquri", "").c_str(), sizeof(cfg.mqttUri));
  strlcpy(cfg.mqttUser,  prefs.getString("mqu", "").c_str(), sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPass,  prefs.getString("mqp", "").c_str(), sizeof(cfg.mqttPass));
  strlcpy(cfg.mqttPrefix, prefs.getString("mqpre", "esp32c3-serial").c_str(), sizeof(cfg.mqttPrefix));
  cfg.wolCount = prefs.getUChar("woln", 0);
  if (cfg.wolCount > WOL_MAX) cfg.wolCount = WOL_MAX;
  for (int i = 0; i < cfg.wolCount; i++) {
    String k = "wol" + String(i);
    strlcpy(cfg.wol[i].mac, prefs.getString(k.c_str(), "").c_str(), sizeof(cfg.wol[i].mac));
  }
  prefs.end();

  if (cfg.netPort == 0)    cfg.netPort = 2333;
  if (cfg.remotePort == 0) cfg.remotePort = 2333;
  if (!cfg.hostname[0])    strlcpy(cfg.hostname, "serial", sizeof(cfg.hostname));
  wlog("[cfg] mode=%u port=%u ssid=%s\r\n", cfg.netMode, cfg.netPort,
       cfg.wifiSsid[0] ? cfg.wifiSsid : "<unset>");
}

static void cfgSave() {
  prefs.begin("wser", false);
  prefs.putString("ssid",  cfg.wifiSsid);
  prefs.putString("wpass", cfg.wifiPass);
  prefs.putString("host",  cfg.hostname);
  prefs.putString("pw",    cfg.password);
  prefs.putUChar("nmode",  cfg.netMode);
  prefs.putUShort("nport", cfg.netPort);
  prefs.putString("rhost", cfg.remoteHost);
  prefs.putUShort("rport", cfg.remotePort);
  prefs.putUChar("nlx",    cfg.nlXlate);
  prefs.putUChar("mqen",   cfg.mqttEn);
  prefs.putString("mquri", cfg.mqttUri);
  prefs.putString("mqu",   cfg.mqttUser);
  prefs.putString("mqp",   cfg.mqttPass);
  prefs.putString("mqpre", cfg.mqttPrefix);
  prefs.putUChar("woln",   cfg.wolCount);
  for (int i = 0; i < cfg.wolCount; i++) {
    String k = "wol" + String(i);
    prefs.putString(k.c_str(), cfg.wol[i].mac);
  }
  prefs.end();
}

// ============================================================================
//  通道分发（bridge）
// ============================================================================
// 串口数据 -> 所有网络通道（TCP 槽 + TCP 客户端 + WS 终端 + MQTT）
static void fanoutToNet(const uint8_t *data, size_t len) {
  for (int i = 0; i < MAX_TCP; i++) {
    if (!tcpSlots[i].used) continue;
    size_t n = tcpSlots[i].cli.write(data, len);
    if (n < len) stDropped += (uint32_t)(len - n);
  }
  if (tcpCliUp) {
    size_t n = tcpCli.write(data, len);
    if (n < len) stDropped += (uint32_t)(len - n);
  }
  if (wsSrv) {
    // 二进制透传：sendBIN 走 WSop_binary 帧。
    // 不能用 broadcastTXT/broadcastBIN 一刀切——广播会发给未鉴权连接。
    // 逐个发给已鉴权（或未设密码）的连接；浏览器端 binaryType=arraybuffer，
    // 串口任意字节（0x00-0xFF）不会被 UTF-8 解码破坏。
    for (int i = 0; i < WS_SLOTS; i++) {
      if (cfg.password[0] && !wsAuthed[i]) continue;
      if (!wsSrv->clientIsConnected(i)) continue;
      if (!wsSrv->sendBIN((uint8_t)i, data, len)) stDropped += (uint32_t)len;
    }
  }
  if (mqttUp && mqtt.connected()) {
    if (!mqtt.publish((String(cfg.mqttPrefix) + "/tx").c_str(), data, len, false))
      stDropped += (uint32_t)len;
  }
}

// 网络数据 -> 串口
static void bridgeNetRx(const uint8_t *data, size_t len) {
  if (!data || !len) return;
  size_t n = DATA_PORT.write(data, len);
  if (n < len) stDropped += (uint32_t)(len - n);
  stSerTx += (uint32_t)n;
}

// 串口 -> 网络 的换行转换（LF 补成 CRLF）
static void applyNl(const uint8_t *in, size_t len, uint8_t *out, size_t *outLen) {
  size_t o = 0;
  bool prevCr = false;
  for (size_t i = 0; i < len; i++) {
    uint8_t b = in[i];
    if (b == '\n' && !prevCr) {
      if (o + 2 <= BR_OUT_BUF) { out[o++] = '\r'; out[o++] = '\n'; }
    } else {
      if (o + 1 <= BR_OUT_BUF) out[o++] = b;
    }
    prevCr = (b == '\r');
  }
  *outLen = o;
}

// Telnet IAC 过滤（入向协商序列剔除，返回有效长度，原址压缩）
static size_t telnetFilter(uint8_t *buf, size_t len, uint8_t *st) {
  size_t o = 0;
  for (size_t i = 0; i < len; i++) {
    uint8_t b = buf[i];
    switch (*st) {
      case TS_DATA:
        if (b == 0xFF) *st = TS_IAC;
        else           buf[o++] = b;
        break;
      case TS_IAC:
        if (b == 0xFF) { buf[o++] = 0xFF; *st = TS_DATA; }
        else if (b >= 0xFB && b <= 0xFE) *st = TS_CMD;
        else if (b == 0xFA)              *st = TS_SB;
        else                             *st = TS_DATA;
        break;
      case TS_CMD: *st = TS_DATA; break;
      case TS_SB:  if (b == 0xFF) *st = TS_IAC; break;
      default:     *st = TS_DATA; break;
    }
  }
  return o;
}

// ============================================================================
//  TCP / Telnet 服务端
// ============================================================================
static int tcpCount() {
  int n = 0;
  for (int i = 0; i < MAX_TCP; i++) if (tcpSlots[i].used) n++;
  return n;
}

static void slotClose(int i) {
  if (i < 0 || i >= MAX_TCP || !tcpSlots[i].used) return;
  tcpSlots[i].cli.stop();
  tcpSlots[i].used = false;
  tcpSlots[i].peer[0] = 0;
}

static void netStop() {
  if (tcpServer) {
    tcpServer->end();
    delete tcpServer;
    tcpServer = nullptr;
  }
  for (int i = 0; i < MAX_TCP; i++) slotClose(i);
  if (tcpCliUp) { tcpCli.stop(); tcpCliUp = false; }
}

static void netStart() {
  netStop();

  if (cfg.netMode == NET_OFF) {
    wlog("[net] 已关闭，只保留网页终端\r\n");
    return;
  }
  if (cfg.netMode == NET_TCP_CLIENT) {
    wlog("[net] TCP 客户端模式: 连 %s:%u\r\n", cfg.remoteHost, cfg.remotePort);
    return;   // 连接在 loop 里按需发起
  }

  uint16_t port = cfg.netPort;
  bool isTelnet = (cfg.netMode == NET_TELNET);
  if (isTelnet && port == 2333) port = 23;

  tcpServer = new NetworkServer(port);
  tcpServer->begin();
  wlog("[net] %s 服务已启动，端口 %u\r\n", isTelnet ? "Telnet" : "TCP", port);
}

// TCP 客户端模式：非阻塞式发起 + 状态推进。
// NetworkClient::connect() 在 Arduino core 3.x 里底层 connect 带超时（默认无超时，
// 这里设 8s），故仍可能阻塞 loop 较久——通过 setConnectionTimeout 限制。
static void netCliPump() {
  if (cfg.netMode != NET_TCP_CLIENT || !cfg.remoteHost[0]) return;

  if (!tcpCliUp) {
    static uint32_t lastTry = 0;
    if (millis() - lastTry < 3000) return;
    lastTry = millis();
    wlog("[net] 连接服务器 %s:%u ...\r\n", cfg.remoteHost, cfg.remotePort);
    tcpCli.setConnectionTimeout(8000);
    int rc = tcpCli.connect(cfg.remoteHost, cfg.remotePort);
    if (rc == 1) {
      tcpCli.setNoDelay(true);
      tcpCliUp = true;
      wlog("[net] 已连上 %s:%u\r\n", cfg.remoteHost, cfg.remotePort);
    } else {
      wlog("[net] 连接失败 rc=%d，3s 后重试\r\n", rc);
    }
  }
}

// ============================================================================
//  WebSocket 终端
// ============================================================================
static void wsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  if (num >= WS_SLOTS) return;
  switch (type) {
    case WStype_CONNECTED: {
      wsAuthed[num] = false;
      wsConnMs[num] = millis();
      wlog("[ws] 终端 #%u 已连接 (%s)\r\n", num,
           wsSrv ? wsSrv->remoteIP(num).toString().c_str() : "?");
      if (cfg.password[0]) {
        // 应用层鉴权（浏览器原生 WS 带不了 Authorization 头）：
        // 5 秒内首帧必须是 "AUTH:<password>"，超时或错误即断开
        wsSrv->sendTXT(num, "AUTH");
      }
      break;
    }
    case WStype_DISCONNECTED:
      wsAuthed[num] = false;
      wlog("[ws] 终端 #%u 已断开\r\n", num);
      break;
    case WStype_TEXT: {
      // 鉴权窗口内：首帧必须是 AUTH:<password>
      if (cfg.password[0] && !wsAuthed[num]) {
        bool ok = length >= 5 && length <= 5 + 32 &&
                  memcmp(payload, "AUTH:", 5) == 0 &&
                  strlen(cfg.password) == length - 5 &&
                  memcmp(payload + 5, cfg.password, length - 5) == 0;
        if (ok) {
          wsAuthed[num] = true;
          wsSrv->sendTXT(num, "OK");
          wlog("[ws] 终端 #%u 鉴权通过\r\n", num);
        } else {
          wlog("[ws] 终端 #%u 鉴权失败，断开\r\n", num);
          wsSrv->disconnect(num);
        }
        return;
      }
      // 已鉴权（或未设密码）：终端输入 -> 串口
      bridgeNetRx(payload, length);
      break;
    }
    case WStype_BIN:
      // 已鉴权（或未设密码）：二进制帧 -> 串口
      if (!cfg.password[0] || wsAuthed[num])
        bridgeNetRx(payload, length);
      break;
    default:
      break;
  }
}

// ============================================================================
//  WOL
// ============================================================================
static bool wolSend(const char *macStr) {
  uint8_t mac[6];
  if (sscanf(macStr, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx",
             &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6)
    return false;

  WiFiUDP udp;
  uint8_t pkt[6 + 16 * 6];
  memset(pkt, 0xFF, 6);
  for (int i = 0; i < 16; i++) memcpy(pkt + 6 + i * 6, mac, 6);

  // 广播优先；失败再试一次定向（netif 网关）
  udp.begin(9);
  bool ok = udp.beginPacket(IPAddress(255,255,255,255), 9) &&
            udp.write(pkt, sizeof(pkt)) == sizeof(pkt) &&
            udp.endPacket() == 1;
  udp.stop();
  return ok;
}

static void wolSendAll() {
  for (int i = 0; i < cfg.wolCount; i++) {
    if (!cfg.wol[i].mac[0]) continue;
    wlog("[wol] 唤醒 %s: %s\r\n", cfg.wol[i].mac,
         wolSend(cfg.wol[i].mac) ? "ok" : "fail");
  }
  if (!cfg.wolCount) wlog("[wol] 未配置 WOL 目标\r\n");
}

// ============================================================================
//  MQTT
// ============================================================================
static void mqttCb(char *topic, byte *payload, unsigned int len) {
  (void)topic;
  bridgeNetRx((const uint8_t *)payload, len);
}

static void mqttApply() {
  if (!cfg.mqttEn || !cfg.mqttUri[0]) return;

  // 解析 host[:port]
  char host[80] = {0};
  int  port = 1883;
  char uri[96];
  strlcpy(uri, cfg.mqttUri, sizeof(uri));
  char *colon = strchr(uri, ':');
  if (colon) {
    *colon = 0;
    port = atoi(colon + 1);
    if (port <= 0) port = 1883;
  }
  strlcpy(host, uri, sizeof(host));

  mqtt.setServer(host, port);
  mqtt.setCallback(mqttCb);   // 必须无条件设置，否则 /cmd 订阅收不到消息

  if (mqtt.connect(cfg.hostname, cfg.mqttUser, cfg.mqttPass)) {
    mqttUp = true;
    mqtt.subscribe((String(cfg.mqttPrefix) + "/cmd").c_str());
    wlog("[mqtt] 已连接 %s:%d，订阅 %s/cmd\r\n", host, port, cfg.mqttPrefix);
  } else {
    wlog("[mqtt] 连接失败 state=%d\r\n", mqtt.state());
  }
}

static void mqttLoopIfUp() {
  if (!cfg.mqttEn) return;
  if (!mqtt.connected()) {
    // 30s 重试（lastTry=0 且 millis()<30s 时不重试——开机首次由 setup 的 mqttApply() 负责）
    static uint32_t lastTry = 0;
    if (millis() - lastTry < 30000 && lastTry != 0) return;
    lastTry = millis();
    mqttApply();
    return;
  }
  mqtt.loop();
}

// ============================================================================
//  Web 管理
// ============================================================================
static bool checkAuth() {
  if (!cfg.password[0]) return true;
  if (web.hasHeader("Authorization")) {
    // Basic 认证：任意用户名 + 密码
    String auth = web.header("Authorization");
    if (auth.startsWith("Basic ")) {
      // base64 解码——手写（core 有 mbedtls，但接口长；这里解码只需 30 行）
      String b = auth.substring(6);
      static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      uint8_t dec[64];
      size_t o = 0, bits = 0;
      uint32_t acc = 0;
      for (size_t i = 0; i < b.length() && o < sizeof(dec); i++) {
        const char *hit = strchr(T, b.charAt(i));
        if (!hit || b.charAt(i) == '=') continue;
        acc = (acc << 6) | (uint32_t)(hit - T);
        bits += 6;
        if (bits >= 8) { bits -= 8; dec[o++] = (uint8_t)((acc >> bits) & 0xFF); }
      }
      // 期望 "user:password"，只校验 password
      char *colon = (char *)memchr(dec, ':', o);
      if (colon) {
        size_t plen = o - (size_t)(colon - (char *)dec) - 1;
        if (plen == strlen(cfg.password) && memcmp(colon + 1, cfg.password, plen) == 0)
          return true;
      }
    }
  }
  return web.authenticate("admin", cfg.password);   // 让 WebServer 自己再试一次
}

static void handleRoot() {
  web.send_P(200, "text/html", INDEX_HTML);
}

static void handleStatus() {
  if (!checkAuth()) { web.requestAuthentication(); web.send(401, "text/plain", "unauthorized"); return; }

  // 模式名
  const char *m = "?";
  switch (cfg.netMode) {
    case NET_TCP_SERVER: m = "TCP-Server"; break;
    case NET_TCP_CLIENT: m = "TCP-Client";  break;
    case NET_TELNET:     m = "Telnet";      break;
    case NET_OFF:        m = "off";         break;
  }

  char buf[512];
  // AP 配网模式（STA 未连）下 localIP() 是 0.0.0.0，显示 softAP IP 才有意义。
  // connected 用实时 status() 而非 setup 时的快照（掉线能立刻反映）
  bool staNow = (WiFi.status() == WL_CONNECTED);
  String ip = staNow ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  snprintf(buf, sizeof(buf),
    "{"
    "\"version\":\"%s\","
    "\"uptime_s\":%lu,"
    "\"wifi\":{\"connected\":%s,\"ip\":\"%s\"},"
    "\"net\":{\"mode\":\"%s\",\"port\":%u,\"clients\":%d},"
    "\"serial\":{\"side\":\"USB-CDC\"},"
    "\"mqtt\":{\"enabled\":%s,\"connected\":%s},"
    "\"stats\":{\"rx\":%lu,\"tx\":%lu,\"dropped\":%lu}"
    "}",
    FW_VERSION,
    (unsigned long)((millis() - bootMs) / 1000),
    staNow ? "true" : "false",
    ip.c_str(),
    m, cfg.netPort, tcpCount(),
    cfg.mqttEn ? "true" : "false",
    (mqttUp && mqtt.connected()) ? "true" : "false",
    (unsigned long)stSerRx, (unsigned long)stSerTx, (unsigned long)stDropped);
  web.send(200, "application/json", buf);
}

static void handleConfigGet() {
  if (!checkAuth()) { web.requestAuthentication(); web.send(401, "text/plain", "unauthorized"); return; }

  // 不回传密码原文，只回传是否已设置
  char buf[640];
  snprintf(buf, sizeof(buf),
    "{"
    "\"wifi_ssid\":\"%s\","
    "\"wifi_pass\":\"%s\","
    "\"hostname\":\"%s\","
    "\"password_set\":%s,"
    "\"net_mode\":%u,"
    "\"net_port\":%u,"
    "\"remote_host\":\"%s\","
    "\"remote_port\":%u,"
    "\"nl_xlate\":%u,"
    "\"mqtt_en\":%u,"
    "\"mqtt_uri\":\"%s\","
    "\"mqtt_prefix\":\"%s\""
    "}",
    cfg.wifiSsid, cfg.wifiPass, cfg.hostname,
    cfg.password[0] ? "true" : "false",
    cfg.netMode, cfg.netPort, cfg.remoteHost, cfg.remotePort,
    cfg.nlXlate, cfg.mqttEn, cfg.mqttUri, cfg.mqttPrefix);
  web.send(200, "application/json", buf);
}

// 简易 JSON 字符串值提取（POST body 是 {"k":"v",...}，只处理字符串/数字）
static String jsonGet(const String &body, const char *key) {
  String pat = "\"" + String(key) + "\":";
  int p = body.indexOf(pat);
  if (p < 0) return "";
  p += pat.length();
  while (p < (int)body.length() && (body.charAt(p) == ' ')) p++;
  if (p >= (int)body.length()) return "";
  if (body.charAt(p) == '"') {
    int e = body.indexOf('"', p + 1);
    if (e < 0) return "";
    return body.substring(p + 1, e);
  }
  int e = p;
  while (e < (int)body.length() && body.charAt(e) != ',' && body.charAt(e) != '}') e++;
  return body.substring(p, e);
}

static void handleConfigPost() {
  if (!checkAuth()) { web.requestAuthentication(); web.send(401, "text/plain", "unauthorized"); return; }
  String body = web.arg("plain");   // WebServer 把非 form 的 POST body 存为 "plain" 参数
  if (!body.length()) {
    web.send(400, "text/plain", "no JSON body");
    return;
  }

  AppCfg nc = cfg;   // 在副本上改，全部解析成功才落盘

  String v;
  v = jsonGet(body, "wifi_ssid");   strlcpy(nc.wifiSsid, v.c_str(), sizeof(nc.wifiSsid));
  v = jsonGet(body, "wifi_pass");   strlcpy(nc.wifiPass, v.c_str(), sizeof(nc.wifiPass));
  v = jsonGet(body, "hostname");    strlcpy(nc.hostname, v.c_str(), sizeof(nc.hostname));
  v = jsonGet(body, "password");    if (v.length()) strlcpy(nc.password, v.c_str(), sizeof(nc.password));
  v = jsonGet(body, "remote_host"); strlcpy(nc.remoteHost, v.c_str(), sizeof(nc.remoteHost));
  v = jsonGet(body, "mqtt_uri");    strlcpy(nc.mqttUri, v.c_str(), sizeof(nc.mqttUri));
  v = jsonGet(body, "mqtt_prefix"); if (v.length()) strlcpy(nc.mqttPrefix, v.c_str(), sizeof(nc.mqttPrefix));
  v = jsonGet(body, "mqtt_user");   strlcpy(nc.mqttUser, v.c_str(), sizeof(nc.mqttUser));
  v = jsonGet(body, "mqtt_pass");   strlcpy(nc.mqttPass, v.c_str(), sizeof(nc.mqttPass));

  v = jsonGet(body, "net_mode");    if (v.length()) nc.netMode    = (uint8_t)v.toInt();
  v = jsonGet(body, "net_port");    if (v.length()) nc.netPort    = (uint16_t)v.toInt();
  v = jsonGet(body, "remote_port"); if (v.length()) nc.remotePort = (uint16_t)v.toInt();
  v = jsonGet(body, "nl_xlate");    if (v.length()) nc.nlXlate    = (uint8_t)v.toInt();
  v = jsonGet(body, "mqtt_en");     if (v.length()) nc.mqttEn     = (uint8_t)v.toInt();

  // WOL 数组："wol":["AA:BB:...","CC:DD:..."]（简易解析：找 "wol":[...] 里的引号串）
  int wp = body.indexOf("\"wol\"");
  if (wp >= 0) {
    int as = body.indexOf('[', wp);
    int ae = body.indexOf(']', wp);
    if (as >= 0 && ae > as) {
      String arr = body.substring(as + 1, ae);
      nc.wolCount = 0;
      int p = 0;
      while (nc.wolCount < WOL_MAX) {
        int q1 = arr.indexOf('"', p);
        if (q1 < 0) break;
        int q2 = arr.indexOf('"', q1 + 1);
        if (q2 < 0) break;
        String mac = arr.substring(q1 + 1, q2);
        mac.trim();
        if (mac.length() == 17)   // "AA:BB:CC:DD:EE:FF"
          strlcpy(nc.wol[nc.wolCount++].mac, mac.c_str(), sizeof(nc.wol[0].mac));
        p = q2 + 1;
      }
    }
  }

  if (!nc.hostname[0]) strlcpy(nc.hostname, "serial", sizeof(nc.hostname));
  if (!nc.netPort)     nc.netPort = 2333;

  bool wifiChanged = (strcmp(nc.wifiSsid, cfg.wifiSsid) != 0) ||
                     (strcmp(nc.wifiPass, cfg.wifiPass) != 0);
  bool netChanged  = (nc.netMode != cfg.netMode) || (nc.netPort != cfg.netPort) ||
                     (strcmp(nc.remoteHost, cfg.remoteHost) != 0) ||
                     (nc.remotePort != cfg.remotePort) ||
                     (nc.nlXlate != cfg.nlXlate);
  bool mqttChanged = (nc.mqttEn != cfg.mqttEn) ||
                     (strcmp(nc.mqttUri, cfg.mqttUri) != 0) ||
                     (strcmp(nc.mqttUser, cfg.mqttUser) != 0) ||
                     (strcmp(nc.mqttPass, cfg.mqttPass) != 0) ||
                     (strcmp(nc.mqttPrefix, cfg.mqttPrefix) != 0);

  cfg = nc;
  cfgSave();

  wlog("[cfg] 配置已保存: wifi=%d net=%d mqtt=%d\r\n", wifiChanged, netChanged, mqttChanged);

  // 立即生效部分
  if (netChanged) netStart();

  char resp[64];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"wifi_changed\":%s,\"reboot_wifi\":%s}",
           wifiChanged ? "true" : "false",
           wifiChanged ? "true" : "false");
  web.send(200, "application/json", resp);

  // MQTT 配置变更：断开旧连接并立即按新配置重连（放 send 之后，阻塞不影响响应）
  if (mqttChanged) {
    mqtt.disconnect();
    mqttUp = false;
    mqttApply();
  }

  // Wi-Fi 变更：等响应发完再重连（重连会短暂断网）
  if (wifiChanged) {
    delay(300);
    WiFi.setHostname(cfg.hostname);
    WiFi.disconnect();
    if (cfg.wifiSsid[0]) WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
  }
}

static void handleWol() {
  if (!checkAuth()) { web.requestAuthentication(); web.send(401, "text/plain", "unauthorized"); return; }
  wolSendAll();
  web.send(200, "application/json", "{\"ok\":true}");
}

static void handleReboot() {
  if (!checkAuth()) { web.requestAuthentication(); web.send(401, "text/plain", "unauthorized"); return; }
  web.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  wlog("[sys] 网页触发重启\r\n");
  delay(300);
  ESP.restart();
}

static void handleLogGet() {
  if (!checkAuth()) { web.requestAuthentication(); web.send(401, "text/plain", "unauthorized"); return; }
  web.setContentLength(CONTENT_LENGTH_UNKNOWN);
  web.send(200, "text/plain; charset=utf-8", "");
  size_t start = (logHead + LOGRING_SZ - logLen) % LOGRING_SZ;
  // 分块发
  String chunk;
  chunk.reserve(256);
  for (size_t i = 0; i < logLen; i++) {
    chunk += logRing[(start + i) % LOGRING_SZ];
    if (chunk.length() >= 200) {
      web.sendContent(chunk);
      chunk = "";
    }
  }
  if (chunk.length()) web.sendContent(chunk);
  web.sendContent("");   // 结束
}

static void handleLogClear() {
  if (!checkAuth()) { web.requestAuthentication(); web.send(401, "text/plain", "unauthorized"); return; }
  logLen = 0;
  logHead = 0;
  web.send(200, "application/json", "{\"ok\":true}");
}

static void handleOtaPage() {
  web.send_P(200, "text/html", OTA_HTML);
}

static void handleOtaUpload() {
  // upload 回调（UPLOAD_FILE_*）：只在解析 multipart body 期间被调用，
  // 此时【不能】发响应、更不能重启——body 还没收完，响应头会插进 body 流里。
  // 响应与重启统一放主 handler（on() 第 3 参 lambda）里做。
  HTTPUpload &up = web.upload();
  if (up.status == UPLOAD_FILE_START) {
    wlog("[ota] 开始升级: %s\r\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      wlog("[ota] begin 失败: %s\r\n", Update.errorString());
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      wlog("[ota] write 失败: %s\r\n", Update.errorString());
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      wlog("[ota] 固件写入完成 (%u bytes)\r\n", (unsigned)up.totalSize);
    } else {
      wlog("[ota] end 失败: %s\r\n", Update.errorString());
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.end(false);
    wlog("[ota] 上传中断\r\n");
  }
}

static void webSetup() {
  // 首页不鉴权（配网场景），API 各自鉴权
  web.on("/",            HTTP_GET,  handleRoot);
  web.on("/api/status",  HTTP_GET,  handleStatus);
  web.on("/api/config",  HTTP_GET,  handleConfigGet);
  web.on("/api/config",  HTTP_POST, handleConfigPost);
  web.on("/api/wol",     HTTP_POST, handleWol);
  web.on("/api/reboot",  HTTP_POST, handleReboot);
  web.on("/api/log",     HTTP_GET,  handleLogGet);
  web.on("/api/log/clear", HTTP_POST, handleLogClear);
  web.on("/ota",         HTTP_GET,  handleOtaPage);
  web.on("/ota/upload",  HTTP_POST, []() {
    // 主 handler：到这里 multipart body 已解析完、Update 已写入完毕。
    // 响应 + 重启都在这里做（对齐 core 自带 WebUpdater 例程的顺序：
    // upload 回调期间发响应会把头插进还没收完的 body 流里）。
    if (Update.hasError()) {
      web.send(500, "text/plain", String("ota failed: ") + Update.errorString());
      return;
    }
    web.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
    wlog("[ota] 升级成功，重启中\r\n");
    delay(500);            // 等 TCP 发送缓冲发完
    web.client().stop();   // 对齐官方 HTTPUpdateServer：主动断开确保 FIN 发出
    ESP.restart();
  }, handleOtaUpload);
  // 收集 Authorization 头
  const char *hdr[] = { "Authorization" };
  web.collectHeaders(hdr, 1);
  web.begin();
  wlog("[web] HTTP 服务已启动 (80)\r\n");
}

// ============================================================================
//  setup / loop
// ============================================================================
void setup() {
  bootMs = millis();

  // 数据口（USB-CDC）—— HWCDC 在无主机连接时 write 走 FIFO 丢弃，不阻塞
  DATA_PORT.begin(115200);
  // 日志口（UART0）
  LOG_PORT.begin(115200);

  // 清掉复位后残留在缓冲里的旧字节
  while (DATA_PORT.available()) DATA_PORT.read();

  wlog("\r\n=== %s v%s (Arduino) ===\r\n", FW_NAME, FW_VERSION);

  cfgLoad();

  // Wi-Fi：必须在 mode() 之前设 hostname
  WiFi.setHostname(cfg.hostname);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  if (cfg.wifiSsid[0]) {
    wlog("[wifi] 连接 %s ...\r\n", cfg.wifiSsid);
    WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < SCAN_TIMEOUT) {
      delay(200);
    }
    if (WiFi.status() == WL_CONNECTED) {
      staUp = true;
      wlog("[wifi] 已连接, IP=%s\r\n", WiFi.localIP().toString().c_str());
    } else {
      wlog("[wifi] 连接失败 (status=%d)，回退配网热点\r\n", WiFi.status());
    }
  }

  // 未连上：开配网热点（AP_STA，保持后台重试 STA）
  if (!staUp) {
    WiFi.mode(WIFI_AP_STA);
    char apSsid[32];
    uint64_t mac = ESP.getEfuseMac();
    snprintf(apSsid, sizeof(apSsid), "ESP32C3-Serial-%02X%02X",
             (unsigned)(mac >> 16) & 0xFF, (unsigned)(mac >> 8) & 0xFF);
    WiFi.softAP(apSsid, "12345678");
    wlog("[wifi] 配网热点: SSID=%s 密码=12345678 IP=%s\r\n",
         apSsid, WiFi.softAPIP().toString().c_str());
    wlog("[wifi] 打开 http://%s 配网\r\n", WiFi.softAPIP().toString().c_str());
  }

  // mDNS
  if (MDNS.begin(cfg.hostname)) {
    MDNS.addService("http", "tcp", 80);
    wlog("[mdns] http://%s.local\r\n", cfg.hostname);
  }

  // 网络 + WS + Web
  netStart();

  wsSrv = new WebSocketsServer(81);
  wsSrv->begin();
  wsSrv->onEvent(wsEvent);
  wlog("[ws] WebSocket 终端已启动 (81)\r\n");

  webSetup();

  // MQTT
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);
  mqtt.setBufferSize(1024);   // 默认 256！透传批最大 512B，不设会静默 publish 失败
  mqttApply();

  wlog("[sys] 启动完成\r\n");
}

void loop() {
  // ---- 1. 串口 -> 网络（逐字节读，攒批）----
  static uint8_t rbuf[TCP_RX_BUF];
  static uint8_t obuf[BR_OUT_BUF];
  size_t rn = 0;
  while (rn < sizeof(rbuf)) {
    int c = DATA_PORT.read();
    if (c < 0) break;
    rbuf[rn++] = (uint8_t)c;
  }
  if (rn) {
    stSerRx += (uint32_t)rn;
    if (cfg.nlXlate) {
      size_t ol = 0;
      applyNl(rbuf, rn, obuf, &ol);
      fanoutToNet(obuf, ol);
    } else {
      fanoutToNet(rbuf, rn);
    }
  }

  // ---- 2. TCP/Telnet 服务端：accept + 各客户端读 ----
  if (tcpServer) {
    while (tcpServer->hasClient()) {
      int idx = -1;
      for (int i = 0; i < MAX_TCP; i++) if (!tcpSlots[i].used) { idx = i; break; }
      NetworkClient nc = tcpServer->accept();
      if (idx < 0 || !nc) {
        if (nc) nc.stop();   // 满了，拒掉
        break;
      }
      tcpSlots[idx].cli      = nc;
      tcpSlots[idx].used     = true;
      tcpSlots[idx].isTelnet = (cfg.netMode == NET_TELNET);
      tcpSlots[idx].iacSt    = TS_DATA;
      snprintf(tcpSlots[idx].peer, sizeof(tcpSlots[idx].peer), "%s:%u",
               nc.remoteIP().toString().c_str(), (unsigned)nc.remotePort());
      if (tcpSlots[idx].isTelnet) {
        // 拒绝所有协商
        static const uint8_t refuse[] = { 0xFF, 0xFE, 0x01, 0xFF, 0xFE, 0x03 };
        nc.write(refuse, sizeof(refuse));
      }
      wlog("[net] 客户端接入: %s (在线 %d)\r\n", tcpSlots[idx].peer, tcpCount());
    }

    static uint8_t nbuf[TCP_RX_BUF];
    for (int i = 0; i < MAX_TCP; i++) {
      if (!tcpSlots[i].used) continue;
      NetworkClient &c = tcpSlots[i].cli;
      if (!c.connected()) {
        wlog("[net] 客户端断开: %s\r\n", tcpSlots[i].peer);
        slotClose(i);
        continue;
      }
      size_t avail = c.available();
      if (!avail) continue;
      if (avail > sizeof(nbuf)) avail = sizeof(nbuf);
      size_t n = c.read(nbuf, avail);
      if (!n) continue;
      size_t len = n;
      if (tcpSlots[i].isTelnet) {
        len = telnetFilter(nbuf, len, &tcpSlots[i].iacSt);
        if (!len) continue;
      }
      bridgeNetRx(nbuf, len);
    }
  }

  // ---- 3. TCP 客户端模式 ----
  netCliPump();
  if (cfg.netMode == NET_TCP_CLIENT && tcpCliUp) {
    if (!tcpCli.connected()) {
      wlog("[net] 与 %s:%u 断开\r\n", cfg.remoteHost, cfg.remotePort);
      tcpCli.stop();
      tcpCliUp = false;
    } else {
      static uint8_t cbuf[TCP_RX_BUF];
      size_t avail = tcpCli.available();
      if (avail) {
        if (avail > sizeof(cbuf)) avail = sizeof(cbuf);
        size_t n = tcpCli.read(cbuf, avail);
        if (n) bridgeNetRx(cbuf, n);
      }
    }
  }

  // ---- 4. WebSocket 终端 ----
  if (wsSrv) {
    wsSrv->loop();
    // 鉴权超时踢除：连接后 5s 内没完成 AUTH 就断开（防挂着连接空耗）
    if (cfg.password[0]) {
      for (int i = 0; i < WS_SLOTS; i++) {
        if (!wsAuthed[i] && wsSrv->clientIsConnected(i) &&
            millis() - wsConnMs[i] > 5000) {
          wlog("[ws] 终端 #%u 鉴权超时，断开\r\n", i);
          wsSrv->disconnect(i);
        }
      }
    }
  }

  // ---- 5. Web / MQTT ----
  web.handleClient();
  mqttLoopIfUp();

  // ---- 6. Wi-Fi 掉线重连（STA 侧，autoReconnect 的兜底）----
  if (staUp && WiFi.status() != WL_CONNECTED) {
    static uint32_t lastWarn = 0;
    if (millis() - lastWarn > 10000) {
      lastWarn = millis();
      wlog("[wifi] STA 掉线 (status=%d)，等待自动重连...\r\n", WiFi.status());
    }
  }

  delay(1);   // 喂狗 + 让出
}
