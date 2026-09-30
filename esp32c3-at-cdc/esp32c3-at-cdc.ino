/*
 * ============================================================================
 *  esp32c3-at-cdc —— ESP32-C3 上的 AT 固件（AT 命令走 USB-CDC）
 * ============================================================================
 *  把 ESP32-C3 当「Wi-Fi 猫」用：上位机（PC / 单片机）发 AT 文本命令让它联网、
 *  收发 TCP/UDP；命令通道走 USB-CDC，插上电脑就是一个 COM 口。
 *
 *  ── 通道分配（本工程最关键的设计）────────────────────────────────────────
 *    AT 命令 → USB Serial/JTAG (HWCDC)，即 Arduino 的 Serial
 *              必须用 FQBN esp32:esp32:esp32c3:CDCOnBoot=cdc 编译
 *    日志    → UART0 (GPIO20/21, 115200)，即 Serial0
 *              两块互不干扰：AT 响应绝不会被日志污染。
 *
 *  ── 为什么日志绝不走 AT 口 ─────────────────────────────────────────────────
 *    AT 是严格的一问一答协议，日志混进去会让上位机解析失败。因此本文件里
 *    **故意不使用** log_i()/ESP_LOGI() 等 Arduino 日志宏 —— 在 CDCOnBoot=cdc
 *    下 Arduino 的 Serial 就是 AT 口，用那些宏会把日志打进 AT 流。
 *    这里只用自写的 atLog()。
 *
 *  ── 没有 UART0 也能看日志 ──────────────────────────────────────────────────
 *    这块 dongle 没把 UART0 引出来，光靠 Serial0 等于看不见日志。
 *    所以 atLog() 会同时把日志写进 RAM 里一个 2KB 的环形缓冲，
 *    用 `AT+LOG?` 就能通过 AT 通道把它倒出来。AT+SYSLOG 控制 UART0 那份。
 *
 *  ── 命令清单 ───────────────────────────────────────────────────────────────
 *    基础     AT / AT+RST / AT+GMR / ATE0 / ATE1 / AT+RESTORE
 *             AT+HELP / AT+SYSLOG / AT+LOG?
 *    Wi-Fi    AT+CWMODE / AT+CWJAP / AT+CWQAP / AT+CWLAP / AT+CWSAP
 *             AT+CIPSTA / AT+CIFSR / AT+CWHOSTNAME / AT+CWAUTOCONN
 *    TCP/UDP  AT+CIPMUX / AT+CIPDINFO / AT+CIPSTART / AT+CIPSEND
 *             AT+CIPCLOSE / AT+CIPSERVER / AT+CIPSTATUS
 *
 *  ── 数据上报 ───────────────────────────────────────────────────────────────
 *    收到 socket 数据时主动上报：+IPD,<link>,<len>:<原始字节>
 *    （AT+CIPDINFO=1 时附带远端 ip/port）
 *
 *  ── 依赖与已验证的平台事实 ─────────────────────────────────────────────────
 *    arduino-esp32 core 3.3.x，单文件，无第三方库。
 *    以下几条是照着实机核源码核过的，不是猜的：
 *      1. platform.txt 里 esp32c3 的额外宏是
 *         `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT={build.cdc_on_boot}`，
 *         所以 CDCOnBoot=cdc 一定让 Serial 指向 HWCDC。
 *      2. core 3.x 中 WiFiServer/WiFiClient 只是 NetworkServer/NetworkClient
 *         的 typedef，`available()` 已标记 deprecated，应用 `accept()`。
 *      3. NetworkServer::begin() 会给 listening socket 设 O_NONBLOCK，
 *         所以 hasClient()/accept() 都不会阻塞 loop。
 *      4. NetworkClient 的 fd 由 shared_ptr 持有、析构函数为空，
 *         所以 `link.tcp = server.accept()` 这种按值复制是安全的（不会误关 fd）。
 *      5. 没有主机连接时 HWCDC::write() 直接走 FIFO 丢弃策略、不阻塞，
 *         所以拔了 USB 也不会把 loop 卡住。
 *      6. core 3.x 的 wl_status_t 里没有 WL_WRONG_PASSWORD，
 *         密码错只能落在 WL_CONNECT_FAILED，故 CWJAP 的 2 号错误是推断出来的。
 * ============================================================================
 */

#if !defined(ARDUINO_USB_CDC_ON_BOOT) || (ARDUINO_USB_CDC_ON_BOOT != 1)
#error "本工程的 AT 通道固定在 USB-CDC。请用 FQBN esp32:esp32:esp32c3:CDCOnBoot=cdc 编译（等价于 -DARDUINO_USB_CDC_ON_BOOT=1）。"
#endif
#if !defined(ARDUINO_USB_MODE) || (ARDUINO_USB_MODE != 1)
#error "ESP32-C3 需要用 Hardware CDC 模式（ARDUINO_USB_MODE=1），这是板级默认值，一般不会出错。"
#endif

#include <WiFi.h>
#include <Preferences.h>
#include <stdarg.h>

// ============================================================================
//  前置声明（有些函数在定义之前就要被用到）
// ============================================================================
static void cfgDefaults();
static void cfgLoad();
static void cfgSave();
static void cfgWipe();

// ============================================================================
//  基本配置
// ============================================================================
#define AT_PORT   Serial     // USB Serial/JTAG（CDC）—— 只跑 AT，绝不写日志
#define LOG_PORT  Serial0    // UART0 GPIO20/21 115200 —— 只跑日志

#define FW_NAME       "esp32c3-at-cdc"
#define FW_VERSION    "1.0.0"

#define MAX_LINKS     5      // link id 0..4（客户端连接 + 服务器 accept 的连接共用）
#define LINE_MAX      640
#define ARG_STR_MAX   128
#define MAX_ARGS      6
#define TX_CHUNK      512    // TCP 发送攒批大小（避免一字节一次 lwip_send）
#define UDP_TX_MAX    1472   // 单个 UDP 报文上限
#define IPD_CHUNK     1024   // 单次 +IPD 上报的最大字节数
#define SEND_MAX      2048   // AT+CIPSEND 单次允许的最大长度
#define CONN_TIMEOUT  15000  // CWJAP 等待超时
#define LOGRING_SZ    2048   // RAM 日志环大小

// ============================================================================
//  配置
// ============================================================================
struct AtCfg {
  uint8_t  mode;        // 1=STA 2=AP 3=AP+STA
  bool     echo;        // AT 回显
  uint8_t  mux;         // 0=单连接 1=多连接
  bool     autoconn;    // 掉线自动重连
  bool     dinfo;       // +IPD 是否带远端 ip/port
  bool     syslog;      // 是否往 UART0 打日志
  bool     dhcp;        // STA 是否用 DHCP
  char     ssid[65];
  char     pass[65];
  char     host[33];
  char     apSsid[33];
  char     apPass[65];
  uint8_t  apCh;
  uint8_t  apEcn;
  char     ip[16];
  char     gw[16];
  char     mask[16];
  uint16_t serverPort;
};

// ── 注意：以下两个 struct 必须位于【第一个函数定义之前】─────────────────────
//    Arduino 的 .ino 预处理会自动为所有函数生成原型并插到文件头部；若 struct
//    定义在第一个函数之后，生成的原型引用不到类型，编译会报
//    'Args' has not been declared / 'Link' does not name a type。
//    （2026-09-30 用最小探针在本机 arduino-cli + core 3.3.11 实测：
//      struct 放函数后 → 必炸；全部前置 → 干净通过。）
struct Args {
  int  n;
  bool isStr[MAX_ARGS];
  char s[MAX_ARGS][ARG_STR_MAX];
};

struct Link {
  bool          used;
  bool          isUdp;
  bool          isServer;    // 由 CIPSERVER accept 进来的
  NetworkClient tcp;
  NetworkUDP    udp;
  IPAddress     peerIp;
  uint16_t      peerPort;
  uint16_t      localPort;
  bool          udpOpen;
};

static AtCfg g_cfg;
static Preferences g_prefs;

static const char *NVS_NS = "atcfg";

// ============================================================================
//  日志（RAM 环 + 可选 UART0）
// ============================================================================
static char   g_logRing[LOGRING_SZ];
static size_t g_logHead = 0;   // 下一个写入位置
static size_t g_logLen  = 0;   // 环里已有的字节数

static void logRingPush(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    g_logRing[g_logHead] = s[i];
    g_logHead = (g_logHead + 1) % LOGRING_SZ;
    if (g_logLen < LOGRING_SZ) g_logLen++;
  }
}

static void logRingDump() {
  size_t start = (g_logHead + LOGRING_SZ - g_logLen) % LOGRING_SZ;
  for (size_t i = 0; i < g_logLen; i++) {
    AT_PORT.write((uint8_t)g_logRing[(start + i) % LOGRING_SZ]);
  }
}

// 所有日志出口。fmt 请自带 \r\n。
static void atLog(const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
  logRingPush(buf, (size_t)n);
  if (g_cfg.syslog) LOG_PORT.write((const uint8_t *)buf, (size_t)n);
}

// ============================================================================
//  输出
// ============================================================================
static void atOk()    { AT_PORT.print(F("\r\nOK\r\n")); }
static void atError() { AT_PORT.print(F("\r\nERROR\r\n")); }
static void atRaw(const char *s) { AT_PORT.print(s); }

// ============================================================================
//  小工具
// ============================================================================
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static bool ciEqN(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (!a[i] || !b[i]) return false;
    if (up(a[i]) != up(b[i])) return false;
  }
  return true;
}
static bool ciEq(const char *a, const char *b) {
  size_t n = strlen(b);
  return strlen(a) == n && ciEqN(a, b, n);
}
static int  atoiSafe(const char *s) { return (int)strtol(s, nullptr, 10); }

static void macToStr(const uint8_t *mac, char *out /* >=18 */) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// efuse 基址 MAC —— 即 Wi-Fi STA 的 MAC；AP 的 MAC 是它 +1。
// 用 getEfuseMac 而不是 WiFi.macAddress()，因为后者随 AP/STA 模式变化。
static void baseMac(uint8_t *mac) {
  uint64_t c = ESP.getEfuseMac();
  mac[0] = (uint8_t)(c >> 0);
  mac[1] = (uint8_t)(c >> 8);
  mac[2] = (uint8_t)(c >> 16);
  mac[3] = (uint8_t)(c >> 24);
  mac[4] = (uint8_t)(c >> 32);
  mac[5] = (uint8_t)(c >> 40);
}

// ============================================================================
//  参数解析：把 `"a","b",12` 拆成 token；引号内的逗号不切分
//    （struct Args 已前置到文件头部——.ino 自动原型会引用它，见上面注释）
// ============================================================================
static void parseArgs(const char *in, Args &a) {
  a.n = 0;
  const char *p = in ? in : "";
  while (*p && a.n < MAX_ARGS) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;

    size_t k = 0;
    if (*p == '"') {
      p++;
      while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) c = *p++;          // \" \\ 之类
        if (k < ARG_STR_MAX - 1) a.s[a.n][k++] = c;
      }
      a.s[a.n][k] = 0;
      a.isStr[a.n] = true;
      if (*p == '"') p++;
    } else {
      while (*p && *p != ',' && *p != ' ' && *p != '\t') {
        if (k < ARG_STR_MAX - 1) a.s[a.n][k++] = *p;
        p++;
      }
      a.s[a.n][k] = 0;
      a.isStr[a.n] = false;
    }
    a.n++;

    while (*p == ' ' || *p == '\t') p++;
    if (*p == ',') { p++; continue; }
    break;
  }
}

// ============================================================================
//  连接池
//    （struct Link 已前置到文件头部——.ino 自动原型会引用它，见上面注释）
// ============================================================================
static Link g_links[MAX_LINKS];
static NetworkServer g_server;
static bool g_serverOn = false;

static Link *linkGet(int id) {
  if (id < 0 || id >= MAX_LINKS || !g_links[id].used) return nullptr;
  return &g_links[id];
}

static int linkAlloc(int want) {
  if (want >= 0) {
    if (want < MAX_LINKS && !g_links[want].used) return want;
    return -1;
  }
  for (int i = 0; i < MAX_LINKS; i++) if (!g_links[i].used) return i;
  return -1;
}

// 单连接模式下不认调用方给的 link id，直接用唯一在用的那个
static int linkResolve(int want) {
  if (g_cfg.mux) return want;
  for (int i = 0; i < MAX_LINKS; i++) if (g_links[i].used) return i;
  return -1;
}

static int linkCount() {
  int n = 0;
  for (int i = 0; i < MAX_LINKS; i++) if (g_links[i].used) n++;
  return n;
}

static void linkFree(int i) {
  if (i < 0 || i >= MAX_LINKS) return;
  Link &L = g_links[i];
  if (!L.used) return;
  // 这里刻意不 memset：NetworkClient/NetworkUDP 不是 POD，memset 会破坏内部状态
  if (L.isUdp) {
    if (L.udpOpen) L.udp.stop();
  } else {
    L.tcp.stop();
  }
  L.udpOpen   = false;
  L.used      = false;
  L.isUdp     = false;
  L.isServer  = false;
  L.peerIp    = IPAddress();
  L.peerPort  = 0;
  L.localPort = 0;
}

static void linkFreeAll() {
  for (int i = 0; i < MAX_LINKS; i++) linkFree(i);
}

// ============================================================================
//  上行上报
// ============================================================================
static void reportConn(int id) {
  if (g_cfg.mux) AT_PORT.printf("\r\n%d,CONNECT\r\n", id);
  else           atRaw("\r\nCONNECT\r\n");
}

static void reportClosed(int id) {
  if (g_cfg.mux) AT_PORT.printf("\r\n%d,CLOSED\r\n", id);
  else           atRaw("\r\nCLOSED\r\n");
}

static void reportIpd(int id, const uint8_t *data, size_t len,
                      const IPAddress &rip, uint16_t rport) {
  if (g_cfg.mux) {
    if (g_cfg.dinfo) AT_PORT.printf("\r\n+IPD,%d,%u,\"%s\",%u:", id, (unsigned)len, rip.toString().c_str(), (unsigned)rport);
    else             AT_PORT.printf("\r\n+IPD,%d,%u:", id, (unsigned)len);
  } else {
    if (g_cfg.dinfo) AT_PORT.printf("\r\n+IPD,%u,\"%s\",%u:", (unsigned)len, rip.toString().c_str(), (unsigned)rport);
    else             AT_PORT.printf("\r\n+IPD,%u:", (unsigned)len);
  }
  AT_PORT.write(data, len);
}

// ============================================================================
//  发送攒批
// ============================================================================
static uint8_t g_txChunk[TX_CHUNK];
static size_t  g_txChunkLen = 0;
static uint8_t g_udpTx[UDP_TX_MAX];
static size_t  g_udpTxLen = 0;

static bool flushTxChunk(Link &L) {
  if (g_txChunkLen == 0) return true;
  size_t w = L.tcp.write(g_txChunk, g_txChunkLen);
  g_txChunkLen = 0;
  return w > 0;
}

static bool udpSendNow(Link &L, const uint8_t *data, size_t len) {
  if (!L.udpOpen) return false;
  if (!L.udp.beginPacket(L.peerIp, L.peerPort)) return false;
  if (len) L.udp.write(data, len);
  return L.udp.endPacket() == 1;
}

// ============================================================================
//  AT 状态机
// ============================================================================
enum AtState { ST_CMD, ST_SEND };

static AtState g_state       = ST_CMD;
static char    g_line[LINE_MAX];
static size_t  g_lineLen     = 0;
static bool    g_lineOver    = false;   // 上一行超长，丢弃到行尾
static int     g_sendLink    = -1;
static size_t  g_sendRemain  = 0;

static void finishSend() {
  Link *L = linkGet(g_sendLink);
  bool ok = true;
  if (L) {
    if (L->isUdp) ok = udpSendNow(*L, g_udpTx, g_udpTxLen);
    else          ok = flushTxChunk(*L);
  } else {
    ok = false;
  }
  if (L && !L->isUdp && ok && !L->tcp.connected()) ok = false;

  g_txChunkLen = 0;
  g_udpTxLen   = 0;
  g_sendLink   = -1;
  g_sendRemain = 0;
  g_state      = ST_CMD;

  atLog("[AT] CIPSEND done ok=%d\r\n", (int)ok);
  if (ok) atRaw("\r\nSEND OK\r\n");
  else    atRaw("\r\nSEND FAIL\r\n");
}

static void abortSend() {
  g_txChunkLen = 0;
  g_udpTxLen   = 0;
  g_sendLink   = -1;
  g_sendRemain = 0;
  g_state      = ST_CMD;
  atRaw("\r\nSEND FAIL\r\n");
}

static void sendDataByte(uint8_t c) {
  Link *L = linkGet(g_sendLink);
  if (!L) { abortSend(); return; }

  if (L->isUdp) {
    if (g_udpTxLen < sizeof(g_udpTx)) g_udpTx[g_udpTxLen++] = c;
  } else {
    g_txChunk[g_txChunkLen++] = c;
    if (g_txChunkLen >= sizeof(g_txChunk)) {
      if (!flushTxChunk(*L)) { abortSend(); return; }
    }
  }
  if (g_sendRemain > 0) {
    g_sendRemain--;
    if (g_sendRemain == 0) finishSend();
  }
}

static void atEchoByte(uint8_t c) {
  if (!g_cfg.echo) return;
  if (c == '\r') AT_PORT.write("\r\n", 2);
  else if (c == '\n') { /* \r 已经换过行了，避免 CRLF 回显两次 */ }
  else if (c == 0x08 || c == 0x7F) AT_PORT.write("\b \b", 3);
  else AT_PORT.write(c);
}

// ============================================================================
//  Wi-Fi
// ============================================================================
static void wifiSyncHostname();

static void applyAp() {
  if (g_cfg.mode == 2 || g_cfg.mode == 3) {
    wifi_auth_mode_t am = WIFI_AUTH_WPA2_PSK;
    switch (g_cfg.apEcn) {
      case 0: am = WIFI_AUTH_OPEN; break;
      case 2: am = WIFI_AUTH_WPA_PSK; break;
      case 3: am = WIFI_AUTH_WPA2_PSK; break;
      default: am = WIFI_AUTH_WPA2_PSK; break;
    }
    const char *pwd = (g_cfg.apEcn == 0) ? nullptr : g_cfg.apPass;
    WiFi.softAP(g_cfg.apSsid, pwd, g_cfg.apCh, 0, 4, false, am);
    atLog("[wifi] softAP ssid=%s ch=%u ecn=%u\r\n", g_cfg.apSsid, g_cfg.apCh, g_cfg.apEcn);
  }
}

// 按当前配置把 Wi-Fi 模式与静态 IP 落到硬件上
static void applyWifiConfig() {
  wifiSyncHostname();

  wifi_mode_t m = WIFI_MODE_STA;
  if (g_cfg.mode == 2) m = WIFI_MODE_AP;
  else if (g_cfg.mode == 3) m = WIFI_MODE_APSTA;

  if (WiFi.getMode() != m) {
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      // 换模式前先断开，否则旧模式的 socket 会留下
      WiFi.disconnect(true, false);
      delay(120);
    }
    WiFi.mode(m);
    delay(120);
  }

  if (!g_cfg.dhcp && g_cfg.mode != 2) {
    IPAddress ip, gw, mask;
    if (ip.fromString(g_cfg.ip) && gw.fromString(g_cfg.gw) && mask.fromString(g_cfg.mask)) {
      WiFi.config(ip, gw, mask);
      atLog("[wifi] static ip=%s gw=%s mask=%s\r\n", g_cfg.ip, g_cfg.gw, g_cfg.mask);
    }
  }

  WiFi.setAutoReconnect(g_cfg.autoconn);
  if (g_cfg.mode == 2 || g_cfg.mode == 3) applyAp();
}

// setHostname 必须在 mode() 之前调用才生效
static void wifiSyncHostname() {
  if (g_cfg.host[0]) WiFi.setHostname(g_cfg.host);
}

// 返回值：0 成功；1 超时；2 密码错/认证失败；3 找不到 AP
static int wifiConnect(const char *ssid, const char *pass, uint32_t timeoutMs) {
  // 连之前必须处于 STA 或 APSTA
  if (g_cfg.mode == 2) {
    g_cfg.mode = 3;
    applyWifiConfig();
  }
  WiFi.begin(ssid, (pass && *pass) ? pass : nullptr);

  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) return 0;
    if (st == WL_NO_SSID_AVAIL) return 3;
    // core 3.x 的 wl_status_t 没有 WL_WRONG_PASSWORD，
    // 认证失败只能落在这里，按「密码错」报给上位机
    if (st == WL_CONNECT_FAILED) return 2;
    delay(50);
  }
  return 1;
}

// ============================================================================
//  AT 命令处理
// ============================================================================
static void cmdRst(const char *arg, bool q, bool s);
static void cmdGmr(const char *arg, bool q, bool s);
static void cmdRestore(const char *arg, bool q, bool s);
static void cmdHelp(const char *arg, bool q, bool s);
static void cmdSyslog(const char *arg, bool q, bool s);
static void cmdLogDump(const char *arg, bool q, bool s);

static void cmdCwmode(const char *arg, bool q, bool s);
static void cmdCwjap(const char *arg, bool q, bool s);
static void cmdCwqap(const char *arg, bool q, bool s);
static void cmdCwlap(const char *arg, bool q, bool s);
static void cmdCwsap(const char *arg, bool q, bool s);
static void cmdCipsta(const char *arg, bool q, bool s);
static void cmdCifsr(const char *arg, bool q, bool s);
static void cmdCwhostname(const char *arg, bool q, bool s);
static void cmdCwautoconn(const char *arg, bool q, bool s);

static void cmdCipmux(const char *arg, bool q, bool s);
static void cmdCipdinfo(const char *arg, bool q, bool s);
static void cmdCipstart(const char *arg, bool q, bool s);
static void cmdCipsend(const char *arg, bool q, bool s);
static void cmdCipclose(const char *arg, bool q, bool s);
static void cmdCipserver(const char *arg, bool q, bool s);
static void cmdCipstatus(const char *arg, bool q, bool s);

struct AtCmd {
  const char *name;
  void (*fn)(const char *, bool, bool);
};

// 名字必须大写；比较时不区分大小写
static const AtCmd AT_CMDS[] = {
  { "RST",        cmdRst },
  { "GMR",        cmdGmr },
  { "RESTORE",    cmdRestore },
  { "HELP",       cmdHelp },
  { "SYSLOG",     cmdSyslog },
  { "LOG",        cmdLogDump },

  { "CWMODE",     cmdCwmode },
  { "CWJAP",      cmdCwjap },
  { "CWQAP",      cmdCwqap },
  { "CWLAP",      cmdCwlap },
  { "CWSAP",      cmdCwsap },
  { "CIPSTA",     cmdCipsta },
  { "CIFSR",      cmdCifsr },
  { "CWHOSTNAME", cmdCwhostname },
  { "CWAUTOCONN", cmdCwautoconn },

  { "CIPMUX",     cmdCipmux },
  { "CIPDINFO",   cmdCipdinfo },
  { "CIPSTART",   cmdCipstart },
  { "CIPSEND",    cmdCipsend },
  { "CIPCLOSE",   cmdCipclose },
  { "CIPSERVER",  cmdCipserver },
  { "CIPSTATUS",  cmdCipstatus },
};
static const size_t AT_CMD_N = sizeof(AT_CMDS) / sizeof(AT_CMDS[0]);

// ---- 行解析 ----
static void handleLine(const char *line) {
  while (*line == ' ' || *line == '\t') line++;

  if (!ciEqN(line, "AT", 2)) { atLog("[AT] not AT: %s\r\n", line); atError(); return; }

  const char *p = line + 2;
  if (*p == 0) { atOk(); return; }

  if (ciEq(p, "E0")) { g_cfg.echo = false; cfgSave(); atOk(); return; }
  if (ciEq(p, "E1")) { g_cfg.echo = true;  cfgSave(); atOk(); return; }

  if (*p != '+') { atLog("[AT] bad cmd: %s\r\n", line); atError(); return; }
  p++;

  const char *nameStart = p;
  while (*p && *p != '?' && *p != '=') p++;
  size_t nameLen = (size_t)(p - nameStart);
  if (nameLen == 0) { atError(); return; }

  bool query = (*p == '?');
  bool set   = (*p == '=');
  const char *arg = set ? (p + 1) : "";

  for (size_t i = 0; i < AT_CMD_N; i++) {
    if (strlen(AT_CMDS[i].name) == nameLen && ciEqN(nameStart, AT_CMDS[i].name, nameLen)) {
      atLog("[AT] %s q=%d s=%d arg=%s\r\n", AT_CMDS[i].name, (int)query, (int)set, set ? arg : "");
      AT_CMDS[i].fn(arg, query, set);
      return;
    }
  }
  atLog("[AT] unknown: %.*s\r\n", (int)nameLen, nameStart);
  atError();
}

static void feedAtByte(uint8_t c) {
  if (g_state == ST_SEND) { sendDataByte(c); return; }

  atEchoByte(c);

  if (c == '\r' || c == '\n') {
    if (g_lineLen == 0) {
      if (g_lineOver) { g_lineOver = false; atError(); }
      return;                                   // 空行不回任何东西
    }
    g_line[g_lineLen] = 0;
    g_lineLen = 0;
    handleLine(g_line);
    return;
  }
  if (c == 0x08 || c == 0x7F) {                 // 退格
    if (g_lineLen) g_lineLen--;
    return;
  }
  if (g_lineLen >= LINE_MAX - 1) { g_lineOver = true; return; }
  g_line[g_lineLen++] = (char)c;
}

// 逐字节读，保证绝不阻塞：
// HWCDC::read() 内部是 xQueueReceive(rx_queue, &c, 0)，超时参数为 0。
// 这里刻意不用 read(buf, size)——那是 Stream::readBytes 语义，会等到凑满或超时。
static void pumpAtPort() {
  static uint8_t rb[256];
  size_t n = 0;
  while (n < sizeof(rb)) {
    int c = AT_PORT.read();
    if (c < 0) break;
    rb[n++] = (uint8_t)c;
  }
  for (size_t i = 0; i < n; i++) feedAtByte(rb[i]);
}

static void pumpServer() {
  if (!g_serverOn) return;
  // listening socket 是 O_NONBLOCK，hasClient/accept 都不会阻塞
  if (!g_server.hasClient()) return;

  NetworkClient nc = g_server.accept();
  if (!nc) return;

  int id = linkAlloc(-1);
  if (id < 0) {
    atLog("[srv] link pool full, reject\r\n");
    nc.stop();
    return;
  }
  Link &L = g_links[id];
  L.used      = true;
  L.isServer  = true;
  L.isUdp     = false;
  L.tcp       = nc;               // fd 由 shared_ptr 共享，nc 析构不会关掉它
  L.peerIp    = nc.remoteIP();
  L.peerPort  = nc.remotePort();
  L.localPort = g_cfg.serverPort;
  L.udpOpen   = false;
  L.tcp.setNoDelay(true);

  atLog("[srv] accept link=%d from %s:%u\r\n", id, L.peerIp.toString().c_str(), L.peerPort);
  reportConn(id);
}

static void pumpLinks() {
  for (int i = 0; i < MAX_LINKS; i++) {
    Link &L = g_links[i];
    if (!L.used) continue;

    if (L.isUdp) {
      if (!L.udpOpen) continue;
      int sz = L.udp.parsePacket();
      if (sz <= 0) continue;
      IPAddress rip = L.udp.remoteIP();
      uint16_t  rp  = L.udp.remotePort();
      uint8_t   buf[IPD_CHUNK];
      int want = sz > (int)sizeof(buf) ? (int)sizeof(buf) : sz;
      int got = L.udp.read(buf, want);
      if (got > 0) reportIpd(i, buf, (size_t)got, rip, rp);
      continue;
    }

    if (!L.tcp.connected()) {
      atLog("[link] %d closed by peer\r\n", i);
      reportClosed(i);
      linkFree(i);
      continue;
    }

    int avail = L.tcp.available();
    if (avail <= 0) continue;

    // 只在 available() 保证有货时读，且要的字节数不超过 available，
    // 这样 _rxBuffer 能直接满足，不会触发带 SO_RCVTIMEO 的阻塞 recv
    size_t want = (size_t)avail;
    if (want > IPD_CHUNK) want = IPD_CHUNK;      // 每轮每个 link 只上报一块
    uint8_t buf[IPD_CHUNK];
    int got = L.tcp.read(buf, want);
    if (got <= 0) continue;
    reportIpd(i, buf, (size_t)got, L.peerIp, L.peerPort);
  }
}

// ============================================================================
//  持久化
// ============================================================================
static void cfgDefaults() {
  memset(&g_cfg, 0, sizeof(g_cfg));
  g_cfg.mode       = 1;
  g_cfg.echo       = true;
  g_cfg.mux        = 0;
  g_cfg.autoconn   = true;
  g_cfg.dinfo      = false;
  g_cfg.syslog     = true;
  g_cfg.dhcp       = true;
  g_cfg.ssid[0]    = 0;
  g_cfg.pass[0]    = 0;
  strcpy(g_cfg.host, FW_NAME);
  strcpy(g_cfg.apSsid, "ESP_AT");
  strcpy(g_cfg.apPass, "12345678");
  g_cfg.apCh       = 1;
  g_cfg.apEcn      = 3;
  g_cfg.ip[0]      = 0;
  g_cfg.gw[0]      = 0;
  g_cfg.mask[0]    = 0;
  g_cfg.serverPort = 0;
}

static void cfgSave() {
  if (!g_prefs.begin(NVS_NS, false)) return;
  g_prefs.putUChar("mode",   g_cfg.mode);
  g_prefs.putBool ("echo",   g_cfg.echo);
  g_prefs.putUChar("mux",    g_cfg.mux);
  g_prefs.putBool ("autocn", g_cfg.autoconn);
  g_prefs.putBool ("dinfo",  g_cfg.dinfo);
  g_prefs.putBool ("syslog", g_cfg.syslog);
  g_prefs.putBool ("dhcp",   g_cfg.dhcp);
  g_prefs.putString("ssid",  g_cfg.ssid);
  g_prefs.putString("pass",  g_cfg.pass);
  g_prefs.putString("host",  g_cfg.host);
  g_prefs.putString("apssid",g_cfg.apSsid);
  g_prefs.putString("appass",g_cfg.apPass);
  g_prefs.putUChar("apch",   g_cfg.apCh);
  g_prefs.putUChar("apecn",  g_cfg.apEcn);
  g_prefs.putString("ip",    g_cfg.ip);
  g_prefs.putString("gw",    g_cfg.gw);
  g_prefs.putString("mask",  g_cfg.mask);
  g_prefs.putUShort("sport", g_cfg.serverPort);
  g_prefs.end();
}

static void copyPrefStr(Preferences &p, const char *key, char *dst, size_t cap) {
  String v = p.getString(key, "");
  if (v.length() >= cap) v = v.substring(0, cap - 1);
  strncpy(dst, v.c_str(), cap - 1);
  dst[cap - 1] = 0;
}

static void cfgLoad() {
  if (!g_prefs.begin(NVS_NS, true)) return;
  if (g_prefs.isKey("mode")) {
    g_cfg.mode     = g_prefs.getUChar("mode",   g_cfg.mode);
    g_cfg.echo     = g_prefs.getBool ("echo",   g_cfg.echo);
    g_cfg.mux      = g_prefs.getUChar("mux",    g_cfg.mux);
    g_cfg.autoconn = g_prefs.getBool ("autocn", g_cfg.autoconn);
    g_cfg.dinfo    = g_prefs.getBool ("dinfo",  g_cfg.dinfo);
    g_cfg.syslog   = g_prefs.getBool ("syslog", g_cfg.syslog);
    g_cfg.dhcp     = g_prefs.getBool ("dhcp",   g_cfg.dhcp);
    copyPrefStr(g_prefs, "ssid",   g_cfg.ssid,   sizeof(g_cfg.ssid));
    copyPrefStr(g_prefs, "pass",   g_cfg.pass,   sizeof(g_cfg.pass));
    copyPrefStr(g_prefs, "host",   g_cfg.host,   sizeof(g_cfg.host));
    copyPrefStr(g_prefs, "apssid", g_cfg.apSsid, sizeof(g_cfg.apSsid));
    copyPrefStr(g_prefs, "appass", g_cfg.apPass, sizeof(g_cfg.apPass));
    g_cfg.apCh     = g_prefs.getUChar ("apch",  g_cfg.apCh);
    g_cfg.apEcn    = g_prefs.getUChar ("apecn", g_cfg.apEcn);
    copyPrefStr(g_prefs, "ip",   g_cfg.ip,   sizeof(g_cfg.ip));
    copyPrefStr(g_prefs, "gw",   g_cfg.gw,   sizeof(g_cfg.gw));
    copyPrefStr(g_prefs, "mask", g_cfg.mask, sizeof(g_cfg.mask));
    g_cfg.serverPort = g_prefs.getUShort("sport", g_cfg.serverPort);
    atLog("[cfg] loaded\r\n");
  }
  g_prefs.end();
}

static void cfgWipe() {
  if (g_prefs.begin(NVS_NS, false)) {
    g_prefs.clear();
    g_prefs.end();
  }
}

// ============================================================================
//  基础命令
// ============================================================================
static void cmdRst(const char *, bool, bool) {  atLog("[sys] restart\r\n");
  delay(50);
  atRaw("\r\nOK\r\n");
  delay(80);
  ESP.restart();
}

static void cmdGmr(const char *, bool, bool) {
  uint8_t mac[6];
  baseMac(mac);
  char ms[18];
  macToStr(mac, ms);
  AT_PORT.printf("\r\nAT version:%s(%s)\r\n", FW_VERSION, FW_NAME);
  AT_PORT.printf("SDK version:%s\r\n", ESP.getSdkVersion());
  AT_PORT.printf("compile time:%s %s\r\n", __DATE__, __TIME__);
  AT_PORT.printf("chip:ESP32-C3  mac:%s\r\n", ms);
  atRaw("Bin version:" FW_VERSION "\r\n\r\nOK\r\n");
}

static void cmdRestore(const char *, bool, bool) {
  atLog("[cfg] factory restore\r\n");
  linkFreeAll();
  cfgWipe();
  delay(50);
  atRaw("\r\nOK\r\n");
  delay(80);
  ESP.restart();
}

static void cmdHelp(const char *, bool, bool) {
  atRaw("\r\n");
  atRaw("AT                      测试\n");
  atRaw("AT+RST                  重启\n");
  atRaw("AT+GMR                  版本信息\n");
  atRaw("ATE0 / ATE1             关/开回显\n");
  atRaw("AT+RESTORE              恢复出厂并重启\n");
  atRaw("AT+SYSLOG=<0|1>         UART0 日志开关\n");
  atRaw("AT+LOG?                 导出 RAM 日志环\n");
  atRaw("AT+CWMODE=<1|2|3>       1STA 2AP 3AP+STA\n");
  atRaw("AT+CWJAP=\"ssid\"[,\"pwd\"]  连 Wi-Fi\n");
  atRaw("AT+CWJAP?               查询当前连接\n");
  atRaw("AT+CWQAP                断开 Wi-Fi\n");
  atRaw("AT+CWLAP                扫描（会短暂断开）\n");
  atRaw("AT+CWSAP=\"ssid\",\"pwd\",ch,ecn  配置 AP\n");
  atRaw("AT+CIPSTA?              查询本机 IP\n");
  atRaw("AT+CIPSTA=\"dhcp\"        用 DHCP\n");
  atRaw("AT+CIPSTA=\"ip\",\"gw\",\"mask\"  静态 IP\n");
  atRaw("AT+CIFSR                查 IP/MAC\n");
  atRaw("AT+CWHOSTNAME=\"name\"    主机名（连接前设）\n");
  atRaw("AT+CWAUTOCONN=<0|1>     掉线自动重连\n");
  atRaw("AT+CIPMUX=<0|1>         单/多连接\n");
  atRaw("AT+CIPDINFO=<0|1>       +IPD 是否带远端信息\n");
  atRaw("AT+CIPSTART=[link,]\n");
  atRaw("  \"TCP|UDP\",\"host\",port[,local]  建连接\n");
  atRaw("AT+CIPSEND=[link,]<len> 发数据\n");
  atRaw("AT+CIPCLOSE[=link]      关连接\n");
  atRaw("AT+CIPSERVER=<0|1>[,port]  开/关 TCP 服务器\n");
  atRaw("AT+CIPSTATUS            连接状态\n");
  atOk();
}

static void cmdSyslog(const char *arg, bool q, bool s) {
  if (q) { AT_PORT.printf("\r\n+SYSLOG:%u\r\n", (unsigned)(g_cfg.syslog ? 1 : 0)); atOk(); return; }
  if (!s) { atError(); return; }
  Args a; parseArgs(arg, a);
  if (a.n < 1) { atError(); return; }
  g_cfg.syslog = atoiSafe(a.s[0]) != 0;
  cfgSave();
  atOk();
}

static void cmdLogDump(const char *, bool, bool) {
  AT_PORT.print(F("\r\n+LOG:\r\n"));
  logRingDump();
  atRaw("\r\nOK\r\n");
}

// ============================================================================
//  Wi-Fi 命令
// ============================================================================
static void cmdCwmode(const char *arg, bool q, bool s) {
  if (q) { AT_PORT.printf("\r\n+CWMODE:%u\r\n", (unsigned)g_cfg.mode); atOk(); return; }
  if (!s) { atError(); return; }
  Args a; parseArgs(arg, a);
  if (a.n < 1) { atError(); return; }
  int m = atoiSafe(a.s[0]);
  if (m < 1 || m > 3) { atError(); return; }
  g_cfg.mode = (uint8_t)m;
  linkFreeAll();
  if (g_serverOn) { g_server.end(); g_serverOn = false; }
  applyWifiConfig();
  cfgSave();
  atOk();
}

static void cmdCwjap(const char *arg, bool q, bool s) {
  if (q) {
    if (WiFi.status() != WL_CONNECTED) { atRaw("\r\n+CWJAP:not connected\r\n"); atError(); return; }
    AT_PORT.printf("\r\n+CWJAP:\"%s\",\"%s\",%d,%d\r\n",
                   WiFi.SSID().c_str(), WiFi.BSSIDstr().c_str(),
                   (int)WiFi.channel(), (int)WiFi.RSSI());
    atOk();
    return;
  }
  if (!s) { atError(); return; }

  Args a; parseArgs(arg, a);
  if (a.n < 1 || !a.s[0][0]) { atError(); return; }
  const char *ssid = a.s[0];
  const char *pass = (a.n >= 2) ? a.s[1] : "";

  atLog("[wifi] join \"%s\"\r\n", ssid);
  linkFreeAll();
  int err = wifiConnect(ssid, pass, CONN_TIMEOUT);
  if (err != 0) {
    atLog("[wifi] join failed err=%d\r\n", err);
    AT_PORT.printf("\r\n+CWJAP:%d\r\n", err);
    atError();
    return;
  }

  strncpy(g_cfg.ssid, ssid, sizeof(g_cfg.ssid) - 1);
  g_cfg.ssid[sizeof(g_cfg.ssid) - 1] = 0;
  strncpy(g_cfg.pass, pass, sizeof(g_cfg.pass) - 1);
  g_cfg.pass[sizeof(g_cfg.pass) - 1] = 0;
  cfgSave();

  atRaw("\r\nWIFI CONNECTED\r\n");
  atRaw("\r\nWIFI GOT IP\r\n");
  atLog("[wifi] joined ip=%s rssi=%d\r\n", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  atOk();
}

static void cmdCwqap(const char *, bool, bool) {
  linkFreeAll();
  WiFi.disconnect(true, false);
  atLog("[wifi] disconnected\r\n");
  atOk();
}

static void cmdCwlap(const char *, bool, bool) {
  atLog("[wifi] scan start\r\n");
  int n = WiFi.scanNetworks(false, true);   // 阻塞扫描；扫描期间连接会短暂中断

  if (n < 0) {                              // WIFI_SCAN_FAILED
    WiFi.scanDelete();
    atLog("[wifi] scan failed\r\n");
    atError();
    return;
  }

  AT_PORT.print(F("\r\n"));
  for (int i = 0; i < n; i++) {
    int ecn = 0;
    switch (WiFi.encryptionType(i)) {
      case WIFI_AUTH_OPEN:            ecn = 0; break;
      case WIFI_AUTH_WEP:             ecn = 1; break;
      case WIFI_AUTH_WPA_PSK:         ecn = 2; break;
      case WIFI_AUTH_WPA2_PSK:        ecn = 3; break;
      case WIFI_AUTH_WPA_WPA2_PSK:    ecn = 4; break;
      case WIFI_AUTH_WPA2_ENTERPRISE: ecn = 5; break;
      case WIFI_AUTH_WPA3_PSK:        ecn = 6; break;
      case WIFI_AUTH_WPA2_WPA3_PSK:   ecn = 7; break;
      default:                        ecn = 0; break;
    }
    String ssid = WiFi.SSID(i);
    ssid.replace("\"", "'");           // 防止 SSID 里的引号破坏格式
    AT_PORT.printf("+CWLAP:(%d,\"%s\",%d,\"%s\",%d)\r\n",
                   ecn, ssid.c_str(), (int)WiFi.RSSI(i),
                   WiFi.BSSIDstr(i).c_str(), (int)WiFi.channel(i));
    delay(2);
  }
  WiFi.scanDelete();
  atLog("[wifi] scan done n=%d\r\n", n);
  atOk();
}

static void cmdCwsap(const char *arg, bool q, bool s) {
  if (q) {
    AT_PORT.printf("\r\n+CWSAP:\"%s\",\"%s\",%u,%u\r\n",
                   g_cfg.apSsid, g_cfg.apPass, (unsigned)g_cfg.apCh, (unsigned)g_cfg.apEcn);
    atOk();
    return;
  }
  if (!s) { atError(); return; }
  Args a; parseArgs(arg, a);
  if (a.n < 4) { atError(); return; }
  strncpy(g_cfg.apSsid, a.s[0], sizeof(g_cfg.apSsid) - 1);
  g_cfg.apSsid[sizeof(g_cfg.apSsid) - 1] = 0;
  strncpy(g_cfg.apPass, a.s[1], sizeof(g_cfg.apPass) - 1);
  g_cfg.apPass[sizeof(g_cfg.apPass) - 1] = 0;
  int ch = atoiSafe(a.s[2]);
  g_cfg.apCh = (uint8_t)((ch >= 1 && ch <= 13) ? ch : 1);
  int ecn = atoiSafe(a.s[3]);
  g_cfg.apEcn = (uint8_t)((ecn >= 0 && ecn <= 3) ? ecn : 3);
  if (g_cfg.mode == 1) g_cfg.mode = 3;      // 只有 STA 的话自动升到 AP+STA
  applyWifiConfig();
  cfgSave();
  atOk();
}

static void cmdCipsta(const char *arg, bool q, bool s) {
  // 裸命令也当查询（比官方宽松一点，方便手工敲）
  if (q || !s) {
    if (WiFi.status() == WL_CONNECTED || g_cfg.mode != 2) {
      AT_PORT.printf("\r\n+CIPSTA:ip:\"%s\"\r\n", WiFi.localIP().toString().c_str());
      AT_PORT.printf("+CIPSTA:gateway:\"%s\"\r\n", WiFi.gatewayIP().toString().c_str());
      AT_PORT.printf("+CIPSTA:netmask:\"%s\"\r\n", WiFi.subnetMask().toString().c_str());
    } else {
      AT_PORT.printf("\r\n+CIPSTA:ip:\"%s\"\r\n", WiFi.softAPIP().toString().c_str());
    }
    atOk();
    return;
  }
  Args a; parseArgs(arg, a);
  if (a.n < 1) { atError(); return; }

  if (a.n == 1 && a.isStr[0] && ciEq(a.s[0], "dhcp")) {
    g_cfg.dhcp = true;
    cfgSave();
    atOk();
    return;
  }
  if (a.n < 3) { atError(); return; }

  IPAddress ip, gw, mask;
  if (!ip.fromString(a.s[0]) || !gw.fromString(a.s[1]) || !mask.fromString(a.s[2])) { atError(); return; }

  strncpy(g_cfg.ip, a.s[0], sizeof(g_cfg.ip) - 1);       g_cfg.ip[sizeof(g_cfg.ip) - 1] = 0;
  strncpy(g_cfg.gw, a.s[1], sizeof(g_cfg.gw) - 1);       g_cfg.gw[sizeof(g_cfg.gw) - 1] = 0;
  strncpy(g_cfg.mask, a.s[2], sizeof(g_cfg.mask) - 1);   g_cfg.mask[sizeof(g_cfg.mask) - 1] = 0;
  g_cfg.dhcp = false;
  cfgSave();

  // 静态 IP 必须在关联前下发才生效：断开重连一次
  linkFreeAll();
  WiFi.disconnect(false, false);
  delay(120);
  applyWifiConfig();
  if (g_cfg.ssid[0]) {
    int err = wifiConnect(g_cfg.ssid, g_cfg.pass, CONN_TIMEOUT);
    atLog("[wifi] rejoin after static ip err=%d\r\n", err);
    if (err != 0) { AT_PORT.printf("\r\n+CWJAP:%d\r\n", err); atError(); return; }
  }
  atOk();
}

static void cmdCifsr(const char *, bool, bool) {
  uint8_t mac[6];
  baseMac(mac);
  char ms[18];
  macToStr(mac, ms);

  atRaw("\r\n");
  if (g_cfg.mode == 2) {
    AT_PORT.printf("+CIFSR:APIP,\"%s\"\r\n", WiFi.softAPIP().toString().c_str());
    mac[5] = (uint8_t)(mac[5] + 1);            // AP 的 MAC 是基址 +1
    macToStr(mac, ms);
    AT_PORT.printf("+CIFSR:APMAC,\"%s\"\r\n", ms);
  } else {
    AT_PORT.printf("+CIFSR:STAIP,\"%s\"\r\n", WiFi.localIP().toString().c_str());
    AT_PORT.printf("+CIFSR:STAMAC,\"%s\"\r\n", ms);
    if (g_cfg.mode == 3) {
      AT_PORT.printf("+CIFSR:APIP,\"%s\"\r\n", WiFi.softAPIP().toString().c_str());
    }
  }
  atOk();
}

static void cmdCwhostname(const char *arg, bool q, bool s) {
  if (q) { AT_PORT.printf("\r\n+CWHOSTNAME:\"%s\"\r\n", g_cfg.host); atOk(); return; }
  if (!s) { atError(); return; }
  Args a; parseArgs(arg, a);
  if (a.n < 1) { atError(); return; }
  strncpy(g_cfg.host, a.s[0], sizeof(g_cfg.host) - 1);
  g_cfg.host[sizeof(g_cfg.host) - 1] = 0;
  wifiSyncHostname();
  cfgSave();
  atOk();
}

static void cmdCwautoconn(const char *arg, bool q, bool s) {
  if (q) { AT_PORT.printf("\r\n+CWAUTOCONN:%u\r\n", (unsigned)(g_cfg.autoconn ? 1 : 0)); atOk(); return; }
  if (!s) { atError(); return; }
  Args a; parseArgs(arg, a);
  if (a.n < 1) { atError(); return; }
  g_cfg.autoconn = atoiSafe(a.s[0]) != 0;
  WiFi.setAutoReconnect(g_cfg.autoconn);
  cfgSave();
  atOk();
}

// ============================================================================
//  TCP/UDP 命令
// ============================================================================
static void cmdCipmux(const char *arg, bool q, bool s) {
  if (q) { AT_PORT.printf("\r\n+CIPMUX:%u\r\n", (unsigned)g_cfg.mux); atOk(); return; }
  if (!s) { atError(); return; }
  Args a; parseArgs(arg, a);
  if (a.n < 1) { atError(); return; }
  int m = atoiSafe(a.s[0]);
  if (m != 0 && m != 1) { atError(); return; }
  if (m != g_cfg.mux && linkCount() > 0) { atError(); return; }   // 有连接时不许切
  if (g_serverOn) { g_server.end(); g_serverOn = false; }
  g_cfg.mux = (uint8_t)m;
  cfgSave();
  atOk();
}

static void cmdCipdinfo(const char *arg, bool q, bool s) {
  if (q) { AT_PORT.printf("\r\n+CIPDINFO:%u\r\n", (unsigned)(g_cfg.dinfo ? 1 : 0)); atOk(); return; }
  if (!s) { atError(); return; }
  Args a; parseArgs(arg, a);
  if (a.n < 1) { atError(); return; }
  g_cfg.dinfo = atoiSafe(a.s[0]) != 0;
  cfgSave();
  atOk();
}

static void cmdCipstart(const char *arg, bool q, bool s) {
  if (!s) { atError(); return; }

  Args a; parseArgs(arg, a);
  int idx = 0;
  int want = -1;
  if (a.n >= 1 && !a.isStr[0]) { want = atoiSafe(a.s[0]); idx = 1; }   // 前面给了 link id

  if (g_cfg.mux == 0) {
    // 单连接模式：不认 link id，但必须没有别的连接
    if (linkCount() > 0) { atRaw("\r\nALREADY CONNECTED\r\n"); atError(); return; }
    want = -1;
  } else if (want < 0) {
    atError(); return;                       // 多连接必须显式给 link
  }

  if (g_cfg.mode == 2) { atError(); return; }               // 纯 AP 模式没有 STA 出口
  if (WiFi.status() != WL_CONNECTED) {
    // 有保存的 SSID 就顺手连一次
    if (!g_cfg.ssid[0]) { atRaw("\r\nNOT CONNECTED\r\n"); atError(); return; }
    if (wifiConnect(g_cfg.ssid, g_cfg.pass, CONN_TIMEOUT) != 0) {
      atRaw("\r\nNOT CONNECTED\r\n"); atError(); return;
    }
  }

  if (a.n < idx + 3) { atError(); return; }
  const char *proto = a.s[idx];
  const char *host  = a.s[idx + 1];
  int port = atoiSafe(a.s[idx + 2]);
  int lport = (a.n >= idx + 4) ? atoiSafe(a.s[idx + 3]) : 0;

  if (port <= 0 || port > 65535) { atError(); return; }

  bool isUdp = ciEq(proto, "UDP");
  if (!isUdp && !ciEq(proto, "TCP")) { atError(); return; }

  int id = linkAlloc(want);
  if (id < 0) { atRaw("\r\nALREADY CONNECTED\r\n"); atError(); return; }

  Link &L = g_links[id];
  L.used     = true;
  L.isUdp    = isUdp;
  L.isServer = false;
  L.udpOpen  = false;
  L.peerPort = (uint16_t)port;
  L.localPort = (uint16_t)lport;

  if (isUdp) {
    if (!L.udp.begin((uint16_t)lport)) {
      linkFree(id);
      atRaw("\r\nCONNECT FAIL\r\n"); atError(); return;
    }
    L.udpOpen = true;
    if (!L.peerIp.fromString(host)) {
      // 域名：让 beginPacket 每次去解析
      IPAddress resolved;
      if (WiFi.hostByName(host, resolved)) L.peerIp = resolved;
      else {
        linkFree(id);
        atRaw("\r\nCONNECT FAIL\r\n"); atError(); return;
      }
    }
    atLog("[link] udp %d -> %s:%d local=%d\r\n", id, L.peerIp.toString().c_str(), port, lport);
    reportConn(id);
    atOk();
    return;
  }

  // TCP
  L.tcp.setConnectionTimeout(CONN_TIMEOUT);
  int rc = L.tcp.connect(host, (uint16_t)port);
  if (rc != 1) {
    linkFree(id);
    atLog("[link] tcp connect fail rc=%d host=%s:%d\r\n", rc, host, port);
    atRaw("\r\nCONNECT FAIL\r\n"); atError(); return;
  }
  L.tcp.setNoDelay(true);
  L.peerIp   = L.tcp.remoteIP();
  L.peerPort = L.tcp.remotePort();
  L.localPort = L.tcp.localPort();

  atLog("[link] tcp %d -> %s:%u (local %u)\r\n", id, L.peerIp.toString().c_str(), L.peerPort, L.localPort);
  reportConn(id);
  atOk();
}

static void cmdCipsend(const char *arg, bool q, bool s) {
  if (!s) { atError(); return; }

  Args a; parseArgs(arg, a);
  int    id  = -1;
  long   len = -1;

  if (g_cfg.mux) {
    if (a.n < 2 || a.isStr[0]) { atError(); return; }
    id  = atoiSafe(a.s[0]);
    len = strtol(a.s[1], nullptr, 10);
  } else {
    if (a.n < 1) { atError(); return; }
    id  = linkResolve(-1);
    len = strtol(a.s[0], nullptr, 10);
  }

  Link *L = linkGet(id);
  if (!L) { atError(); return; }

  long limit = L->isUdp ? (long)UDP_TX_MAX : (long)SEND_MAX;
  if (len <= 0 || len > limit) { atError(); return; }

  g_state      = ST_SEND;
  g_sendLink   = id;
  g_sendRemain = (size_t)len;
  g_txChunkLen = 0;
  g_udpTxLen   = 0;

  atLog("[link] CIPSEND link=%d len=%ld\r\n", id, len);
  AT_PORT.print(F("\r\nOK\r\n> "));     // 官方格式：OK 之后跟 '>' 提示符
}

static void cmdCipclose(const char *arg, bool q, bool s) {
  (void)q;
  if (!s) {
    // 不带参数：单连接关那一个；多连接全关
    if (g_cfg.mux == 0) {
      int id = linkResolve(-1);
      if (id < 0) { atError(); return; }
      linkFree(id);
    } else {
      linkFreeAll();
    }
    atOk();
    return;
  }
  Args a; parseArgs(arg, a);
  if (a.n < 1) { atError(); return; }
  int id = atoiSafe(a.s[0]);
  if (!linkGet(id)) { atError(); return; }
  linkFree(id);
  atOk();
}

static void cmdCipserver(const char *arg, bool q, bool s) {
  (void)q;
  if (!s) { atError(); return; }
  Args a; parseArgs(arg, a);
  if (a.n < 1) { atError(); return; }
  int on = atoiSafe(a.s[0]);

  if (on == 0) {
    if (!g_serverOn) { atError(); return; }
    g_server.end();
    g_serverOn = false;
    for (int i = 0; i < MAX_LINKS; i++) if (g_links[i].used && g_links[i].isServer) linkFree(i);
    atLog("[srv] stopped\r\n");
    atOk();
    return;
  }

  if (g_cfg.mux == 0) { atError(); return; }        // 服务器要求多连接（同官方）
  if (WiFi.status() != WL_CONNECTED) { atRaw("\r\nNOT CONNECTED\r\n"); atError(); return; }

  int port = (a.n >= 2) ? atoiSafe(a.s[1]) : (int)g_cfg.serverPort;
  if (port <= 0 || port > 65535) { atError(); return; }

  if (g_serverOn) { g_server.end(); g_serverOn = false; }

  g_server.setNoDelay(true);       // 让 accept 出来的连接默认 TCP_NODELAY
  g_server.begin((uint16_t)port, 1);
  if (!g_server) { atError(); return; }

  g_serverOn = true;
  g_cfg.serverPort = (uint16_t)port;
  cfgSave();
  atLog("[srv] listening on %u\r\n", (unsigned)port);
  atOk();
}

static void cmdCipstatus(const char *, bool, bool) {
  int st = 5;                                   // 5: 未连接
  if (WiFi.status() == WL_CONNECTED) st = (linkCount() > 0) ? 3 : 2;  // 2: 拿到 IP 3: 已建连接
  else if (g_cfg.mode == 3 || g_cfg.mode == 2) st = 4;                // 4: AP 已就绪

  AT_PORT.printf("\r\nSTATUS:%d\r\n", st);
  for (int i = 0; i < MAX_LINKS; i++) {
    Link &L = g_links[i];
    if (!L.used) continue;
    AT_PORT.printf("+CIPSTATUS:%d,\"%s\",\"%s\",%u,%u,%s\r\n",
                   i,
                   L.isUdp ? "UDP" : "TCP",
                   L.peerIp.toString().c_str(),
                   (unsigned)L.peerPort,
                   (unsigned)L.localPort,
                   L.isServer ? "server" : "client");
  }
  atOk();
}

// ============================================================================
//  setup / loop
// ============================================================================
void setup() {
  // 1) AT 口：把 RX 环加大到 1024。CWJAP/CWLAP 是阻塞的，
  //    期间上位机继续发的命令得先在环里等着。
  AT_PORT.setRxBufferSize(1024);
  AT_PORT.begin(115200);

  // 2) 日志口 UART0
  LOG_PORT.begin(115200);

  // 3) 丢掉上电瞬间的残字节（例如复位前的半条命令）
  delay(30);
  while (AT_PORT.available()) AT_PORT.read();

  // 4) 配置
  cfgDefaults();
  cfgLoad();

  // 5) Wi-Fi：setHostname 必须在 mode() 之前
  wifiSyncHostname();
  WiFi.setSleep(false);          // 关省电，否则局域网延迟抖动且容易掉包
  WiFi.persistent(false);        // 我们不靠 SDK 自己存 SSID，配置全在 Preferences
  applyWifiConfig();

  atLog("\r\n==== %s v%s ====\r\n", FW_NAME, FW_VERSION);
  atLog("[cfg] mode=%u mux=%u echo=%d autoconn=%d dhcp=%d ssid=\"%s\" host=%s\r\n",
        (unsigned)g_cfg.mode, (unsigned)g_cfg.mux, (int)g_cfg.echo,
        (int)g_cfg.autoconn, (int)g_cfg.dhcp, g_cfg.ssid, g_cfg.host);

  // 6) 有保存的 SSID 就自动连一次
  if (g_cfg.ssid[0] && g_cfg.mode != 2) {
    atLog("[wifi] auto join \"%s\"\r\n", g_cfg.ssid);
    int err = wifiConnect(g_cfg.ssid, g_cfg.pass, CONN_TIMEOUT);
    if (err == 0) atLog("[wifi] joined ip=%s\r\n", WiFi.localIP().toString().c_str());
    else          atLog("[wifi] auto join failed err=%d\r\n", err);
  }

  atRaw("\r\nready\r\n");
}

void loop() {
  pumpAtPort();     // 收 AT 命令
  pumpServer();     // 收新进来的 TCP 连接
  pumpLinks();      // 收 socket 数据 → +IPD
  delay(1);
}
