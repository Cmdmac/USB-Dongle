/*
 * ============================================================================
 *  esp32c3-ble-scan —— ESP32-C3 上的 BLE 扫描 AT 固件（AT 走 USB-CDC）
 * ============================================================================
 *  把 ESP32-C3 当「BLE 扫描猫」用：上位机（PC / 单片机）发 AT 文本命令
 *  让它扫描 BLE 广播，扫描结果通过 +BLESCAN:... URC 主动推给上位机。
 *  命令通道走 USB-CDC，插上电脑就是一个 COM 口。
 *
 *  ── 通道分配（与 esp32c3-at-cdc 一致）─────────────────────────────────────
 *    AT 命令 → USB Serial/JTAG (HWCDC)，即 Arduino 的 Serial
 *              必须用 FQBN esp32:esp32:esp32c3:CDCOnBoot=cdc 编译
 *    日志    → UART0 (GPIO20/21, 115200)，即 Serial0
 *              两块互不干扰：AT 响应绝不会被日志污染。
 *
 *  ── 命令清单 ───────────────────────────────────────────────────────────────
 *    基础     AT / AT+RST / AT+GMR / ATE0 / ATE1 / AT+RESTORE
 *             AT+HELP / AT+SYSLOG=<0|1> / AT+SYSLOG? / AT+LOG?
 *    BLE 扫描 AT+BLEINIT?              查询 BLE 初始化状态
 *             AT+BLEINIT=<0|1>         0=deinit 1=init（扫描前必须 init）
 *             AT+BLESCAN?              查询扫描状态
 *             AT+BLESCAN=1[,<秒>]      开始扫描，可选时长（默认 10s，0=不限）
 *             AT+BLESCAN=0             立即停止扫描
 *             AT+BLESCANRSL            导出本次扫描已缓存的结果
 *             AT+BLESCANPARM?          查询扫描参数（active/interval/window）
 *             AT+BLESCANPARM=<a>,<i>,<w>  设置扫描参数
 *    GATT C  AT+BLECONN?              查询 GATT 连接状态
 *             AT+BLECONN=<0|1>[,<mac>][,<addr_type>]
 *                                      1=连接（需带 mac，可选 addr_type 0/1）
 *                                      0=断开
 *             AT+BLEGATTC_DISC?        查询发现状态
 *             AT+BLEGATTC_DISC=<s|c|d|all>
 *                                      发现 service/characteristic/descriptor/全部
 *             AT+BLEGATTC_READ=<handle> 读指定 handle 的特征值
 *             AT+BLEGATTC_WRITE=<handle>,<hex>     写（带响应）
 *             AT+BLEGATTC_WRITENR=<handle>,<hex>   写（无响应）
 *             AT+BLEGATTC_SUB=<handle>,<1|0>       订阅/取消订阅 notify/indicate
 *
 *  ── 数据上报 ───────────────────────────────────────────────────────────────
 *    扫描（同前）：
 *      +BLESCAN:<addr_type>,<addr>,<rssi>,<name>,<adv_hex>
 *      +BLESCAN:done
 *    GATT 连接状态：
 *      +BLECONN:connected,<addr>           连接成功
 *      +BLECONN:disconnected,<addr>        断开（含对端主动断）
 *    发现结果（按 AT+BLEGATTC_DISC 类型逐条上报，最后发 done）：
 *      +BLEGATTC_SVC:<start_h>,<end_h>,<uuid>            service
 *      +BLEGATTC_CHR:<decl_h>,<value_h>,<props>,<uuid>   characteristic
 *      +BLEGATTC_DESC:<handle>,<uuid>                     descriptor
 *      +BLEGATTC_DISC:done
 *      props: 10 进制位掩码（read=2 wr=8 wnr=4 notify=0x10 indicate=0x20）
 *    读响应：
 *      +BLEGATTC_READ:<handle>,<hex>
 *    Notify/Indicate 主动上报：
 *      +BLEGATTC_NOTIF:<handle>,<hex>       notify
 *      +BLEGATTC_IND:<handle>,<hex>         indicate
 *
 *  ── 依赖与已验证的平台事实 ─────────────────────────────────────────────────
 *    arduino-esp32 core 3.3.x；库：BLEDevice.h（core 自带）。
 *    ESP32-C3 支持 BLE 5.0；Bluedroid 栈在 core 3.x 下仍可用。
 *    .ino 预处理坑：struct Args 必须前置到第一个函数定义之前（与
 *    esp32c3-at-cdc 同一原因，详见其注释第 9 条）。
 * ============================================================================
 */

#if !defined(ARDUINO_USB_CDC_ON_BOOT) || (ARDUINO_USB_CDC_ON_BOOT != 1)
#error "本工程的 AT 通道固定在 USB-CDC。请用 FQBN esp32:esp32:esp32c3:CDCOnBoot=cdc 编译（等价于 -DARDUINO_USB_CDC_ON_BOOT=1）。"
#endif
#if !defined(ARDUINO_USB_MODE) || (ARDUINO_USB_MODE != 1)
#error "ESP32-C3 需要用 Hardware CDC 模式（ARDUINO_USB_MODE=1），这是板级默认值。"
#endif

#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>
#include <BLERemoteDescriptor.h>
#include <Preferences.h>
#include <stdarg.h>
// Wi-Fi 局域网访问（与 USB-CDC 并存）
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WebSocketsServer.h>

// ============================================================================
//  前置声明
// ============================================================================
static void cfgDefaults();
static void cfgLoad();
static void cfgSave();
static void cfgWipe();
static void feedAtByte(uint8_t c);

// ============================================================================
//  MultiStream：AT 输出 fanout 到 USB-CDC + 所有 WebSocket 客户端
// ============================================================================
static WebSocketsServer g_ws(81);
static bool g_wsReady = false;
class MultiStream : public Print {
public:
  size_t write(uint8_t c) override {
    Serial.write(&c, 1);
    if (g_wsReady) g_ws.broadcastBIN(&c, 1);
    return 1;
  }
  size_t write(const uint8_t *buf, size_t n) override {
    Serial.write(buf, n);
    if (g_wsReady) g_ws.broadcastBIN(buf, n);
    return n;
  }
  // Print::print(const char*) 会调 write(const uint8_t*, size_t)，但 const char*
  // 不能隐式转 const uint8_t*（-fpermissive 下也只是警告）。加一个 char 版本重载。
  size_t write(const char *buf, size_t n) {
    return write((const uint8_t *)buf, n);
  }
};
static MultiStream g_atOut;
// Wi-Fi 状态（声明在此处供 cmdWifi 等前向引用；定义见下方 wifiSetup 区）
static String g_wifiSsid, g_wifiPass, g_ipStr, g_mdnsUrl;
static bool   g_wifiSta = false;

// ============================================================================
//  基本配置
// ============================================================================
// AT 输出走 MultiStream：同时 fanout 到 USB-CDC + 所有 WebSocket 客户端。
// 输入方向仍直接读 Serial（USB-CDC），WS 收到的字节在 wsEvent 里喂 feedAtByte。
// 这样 USB 插电脑、Wi-Fi 局域网访问，两条通道并存且响应互通。
#define AT_PORT   g_atOut
#define LOG_PORT  Serial0    // UART0 GPIO20/21 115200 —— 只跑日志

#define FW_NAME       "esp32c3-ble-scan"
#define FW_VERSION    "1.0.0"

#define LINE_MAX      640
#define ARG_STR_MAX   128
#define MAX_ARGS      6
#define LOGRING_SZ    2048
#define ADV_HEX_MAX   480      // 单条广播数据上限（adv+scanrsp 拼起来）
#define MAX_RESULTS   32       // 本地缓存的去重结果数（AT+BLESCANRSL 倒出）
#define DEF_DURATION  10       // 默认扫描秒数
#define MAX_DURATION  3600     // 单次扫描最长 1 小时

// ============================================================================
//  配置
// ============================================================================
struct AtCfg {
  bool     echo;          // AT 回显
  bool     syslog;        // 是否往 UART0 打日志
  bool     bleInited;     // BLE 协议栈是否已 init
  bool     scanning;      // 是否正在扫描
  bool     activeScan;    // 主动扫描（带 scan request）
  uint16_t interval;      // 扫描间隔（单位 0.625ms，BLE 栈约定）
  uint16_t window;        // 扫描窗口（同上）
};

// ── 注意：以下 struct 必须位于【第一个函数定义之前】─────────────────────
//    Arduino 的 .ino 预处理会自动为所有函数生成原型并插到文件头部；若 struct
//    定义在第一个函数之后，生成的原型引用不到类型，编译会报
//    'Args' has not been declared。详见 esp32c3-at-cdc 第 9 条平台事实。
struct Args {
  int  n;
  bool isStr[MAX_ARGS];
  char s[MAX_ARGS][ARG_STR_MAX];
};

// 本地缓存的扫描结果（按 MAC 去重，存最后一次见到时的字段）
struct ScanResult {
  bool          used;
  uint8_t       addrType;
  char          addr[18];      // "AA:BB:CC:DD:EE:FF"
  int           rssi;
  char          name[64];
  char          advHex[ADV_HEX_MAX + 1];
};

static AtCfg      g_cfg;
static Preferences g_prefs;
static const char *NVS_NS = "blecfg";

// ============================================================================
//  日志（RAM 环 + 可选 UART0）
// ============================================================================
static char   g_logRing[LOGRING_SZ];
static size_t g_logHead = 0;
static size_t g_logLen  = 0;

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

// ============================================================================
//  参数解析：把 `"a","b",12` 拆成 token；引号内的逗号不切分
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
        if (c == '\\' && *p) c = *p++;
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
//  扫描结果缓存（按 MAC 去重，存最后一次见到的字段）
// ============================================================================
static ScanResult g_results[MAX_RESULTS];

static void resultsClear() {
  for (int i = 0; i < MAX_RESULTS; i++) g_results[i].used = false;
}

// 按 addr 找一个槽位；没有就找一个空槽。返回 nullptr 表示已满。
static ScanResult *resultsFindOrAlloc(const char *addr) {
  for (int i = 0; i < MAX_RESULTS; i++) {
    if (g_results[i].used && strcmp(g_results[i].addr, addr) == 0) return &g_results[i];
  }
  for (int i = 0; i < MAX_RESULTS; i++) {
    if (!g_results[i].used) {
      memset(&g_results[i], 0, sizeof(ScanResult));
      g_results[i].used = true;
      return &g_results[i];
    }
  }
  return nullptr;
}

// ============================================================================
//  BLE 扫描回调
// ============================================================================
//  每收到一条广播就：
//    1) 格式化 +BLESCAN:<addr_type>,<addr>,<rssi>,<name>,<adv_hex> URC
//    2) 把/更新这条数据塞进 g_results（供 AT+BLESCANRSL 倒出）
//
//  设计上不做去重过滤：BLE 广播本来就是反复发的，每次见到都上报，
//  上位机侧自行决定保留最新还是去重（网页默认按 MAC 更新 RSSI）。
//  g_results 槽位则按 MAC 去重，是兜底的「最后一次见到时的快照」。
class BleScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  // arduino-esp32 3.x 签名是传值（BLEAdvertisedDevice 有拷贝构造）
  void onResult(BLEAdvertisedDevice d) override {
    BLEAddress bleAddr = d.getAddress();
    String addrStr = bleAddr.toString();     // "aa:bb:cc:dd:ee:ff"（小写）
    int rssi = d.getRSSI();
    uint8_t addrType = bleAddr.getType();

    // 大写，跟 nRF Connect 等工具的展示习惯一致
    char addrUp[18] = {0};
    addrStr.toUpperCase();
    strncpy(addrUp, addrStr.c_str(), sizeof(addrUp) - 1);
    addrUp[sizeof(addrUp) - 1] = 0;

    // 广播 payload（库已合并 scan response），转十六进制
    char advHex[ADV_HEX_MAX + 1] = {0};
    size_t advLen = d.getPayloadLength();
    if (advLen > ADV_HEX_MAX / 2) advLen = ADV_HEX_MAX / 2;
    const uint8_t *pb = d.getPayload();
    for (size_t i = 0; i < advLen; i++) {
      sprintf(advHex + i * 2, "%02X", pb[i]);
    }

    // 名字里的双引号替换成单引号，避免破坏 AT 响应的引号配对
    char nameSafe[64] = {0};
    if (d.haveName()) {
      String nm = d.getName();
      size_t n = nm.length();
      if (n >= sizeof(nameSafe)) n = sizeof(nameSafe) - 1;
      memcpy(nameSafe, nm.c_str(), n);
      for (char *p = nameSafe; *p; p++) if (*p == '"') *p = '\'';
    }

    AT_PORT.printf("\r\n+BLESCAN:%u,%s,%d,\"%s\",\"%s\"\r\n",
                   (unsigned)addrType, addrUp, rssi, nameSafe, advHex);

    // 同步进 g_results（兜底缓存）
    ScanResult *r = resultsFindOrAlloc(addrUp);
    if (r) {
      r->addrType = addrType;
      strncpy(r->addr, addrUp, sizeof(r->addr) - 1);
      r->addr[sizeof(r->addr) - 1] = 0;
      r->rssi = rssi;
      strncpy(r->name, nameSafe, sizeof(r->name) - 1);
      r->name[sizeof(r->name) - 1] = 0;
      strncpy(r->advHex, advHex, sizeof(r->advHex) - 1);
      r->advHex[sizeof(r->advHex) - 1] = 0;
    }
  }
};

static BLEScan *g_scan = nullptr;
static uint32_t g_scanEndMs = 0;     // 0 表示没设时长（持续到 AT+BLESCAN=0）
static uint32_t g_scanStartMs = 0;

// ---- GATT Client（单连接）----
// ESP32-C3 默认 CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1，足够做 nRF Connect 式单连调试。
// 多连需要改 sdkconfig，这里不做。
static BLEClient *g_client = nullptr;
static bool g_connConnected = false;
static char  g_connAddr[18] = {0};        // 当前连接的对端 MAC（大写）
static uint8_t g_connAddrType = 0;
static bool g_discRunning = false;        // 发现过程进行中（异步）

// ============================================================================
//  AT 命令处理
// ============================================================================
static void cmdRst(const char *, bool, bool);
static void cmdGmr(const char *, bool, bool);
static void cmdRestore(const char *, bool, bool);
static void cmdHelp(const char *, bool, bool);
static void cmdSyslog(const char *, bool, bool);
static void cmdLogDump(const char *, bool, bool);
static void cmdBleInit(const char *, bool, bool);
static void cmdBleScan(const char *, bool, bool);
static void cmdBleScanRsl(const char *, bool, bool);
static void cmdBleScanParm(const char *, bool, bool);
static void cmdBleConn(const char *, bool, bool);
static void cmdBleGattcDisc(const char *, bool, bool);
static void cmdBleGattcRead(const char *, bool, bool);
static void cmdBleGattcWrite(const char *, bool, bool);
static void cmdBleGattcWriteNr(const char *, bool, bool);
static void cmdBleGattcSub(const char *, bool, bool);
static void cmdWifi(const char *, bool, bool);

struct AtCmd {
  const char *name;
  void (*fn)(const char *, bool, bool);
};

// 名字必须大写；比较时不区分大小写
static const AtCmd AT_CMDS[] = {
  { "RST",            cmdRst },
  { "GMR",            cmdGmr },
  { "RESTORE",        cmdRestore },
  { "HELP",           cmdHelp },
  { "SYSLOG",         cmdSyslog },
  { "LOG",            cmdLogDump },

  { "BLEINIT",        cmdBleInit },
  { "BLESCAN",        cmdBleScan },
  { "BLESCANRSL",     cmdBleScanRsl },
  { "BLESCANPARM",    cmdBleScanParm },

  { "BLECONN",        cmdBleConn },
  { "BLEGATTC_DISC",  cmdBleGattcDisc },
  { "BLEGATTC_READ",  cmdBleGattcRead },
  { "BLEGATTC_WRITE", cmdBleGattcWrite },
  { "BLEGATTC_WRITENR", cmdBleGattcWriteNr },
  { "BLEGATTC_SUB",   cmdBleGattcSub },

  { "WIFI",           cmdWifi },
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

static void atEchoByte(uint8_t c) {
  if (!g_cfg.echo) return;
  if (c == '\r') AT_PORT.write("\r\n", 2);
  else if (c == '\n') { /* \r 已经换过行了 */ }
  else if (c == 0x08 || c == 0x7F) AT_PORT.write("\b \b", 3);
  else AT_PORT.write(c);
}

static char    g_line[LINE_MAX];
static size_t  g_lineLen = 0;
static bool    g_lineOver = false;

static void feedAtByte(uint8_t c) {
  atEchoByte(c);

  if (c == '\r' || c == '\n') {
    if (g_lineLen == 0) {
      if (g_lineOver) { g_lineOver = false; atError(); }
      return;
    }
    g_line[g_lineLen] = 0;
    g_lineLen = 0;
    handleLine(g_line);
    return;
  }
  if (c == 0x08 || c == 0x7F) {
    if (g_lineLen) g_lineLen--;
    return;
  }
  if (g_lineLen >= LINE_MAX - 1) { g_lineOver = true; return; }
  g_line[g_lineLen++] = (char)c;
}

// 逐字节读，保证绝不阻塞：
// HWCDC::read() 内部是 xQueueReceive(rx_queue, &c, 0)，超时参数为 0。
static void pumpAtPort() {
  static uint8_t rb[256];
  size_t n = 0;
  while (n < sizeof(rb)) {
    int c = Serial.read();
    if (c < 0) break;
    rb[n++] = (uint8_t)c;
  }
  for (size_t i = 0; i < n; i++) feedAtByte(rb[i]);
}

// ============================================================================
//  持久化
// ============================================================================
static void cfgDefaults() {
  memset(&g_cfg, 0, sizeof(g_cfg));
  g_cfg.echo      = true;
  g_cfg.syslog    = true;
  g_cfg.bleInited = false;
  g_cfg.scanning  = false;
  g_cfg.activeScan = true;
  g_cfg.interval  = 160;      // 100ms（160 * 0.625ms）
  g_cfg.window    = 160;      // 100ms 全开窗
}

static void cfgSave() {
  if (!g_prefs.begin(NVS_NS, false)) return;
  g_prefs.putBool ("echo",     g_cfg.echo);
  g_prefs.putBool ("syslog",   g_cfg.syslog);
  g_prefs.putBool ("actscan",  g_cfg.activeScan);
  g_prefs.putUShort("intv",    g_cfg.interval);
  g_prefs.putUShort("wind",    g_cfg.window);
  g_prefs.end();
}

static void cfgLoad() {
  if (!g_prefs.begin(NVS_NS, true)) return;
  if (g_prefs.isKey("echo")) {
    g_cfg.echo      = g_prefs.getBool ("echo",    g_cfg.echo);
    g_cfg.syslog    = g_prefs.getBool ("syslog",  g_cfg.syslog);
    g_cfg.activeScan = g_prefs.getBool("actscan", g_cfg.activeScan);
    g_cfg.interval  = g_prefs.getUShort("intv",   g_cfg.interval);
    g_cfg.window    = g_prefs.getUShort("wind",   g_cfg.window);
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
static void cmdRst(const char *, bool, bool) {
  atLog("[sys] restart\r\n");
  delay(50);
  atRaw("\r\nOK\r\n");
  delay(80);
  ESP.restart();
}

static void cmdGmr(const char *, bool, bool) {
  AT_PORT.printf("\r\nAT version:%s(%s)\r\n", FW_VERSION, FW_NAME);
  AT_PORT.printf("SDK version:%s\r\n", ESP.getSdkVersion());
  AT_PORT.printf("compile time:%s %s\r\n", __DATE__, __TIME__);
  AT_PORT.printf("chip:ESP32-C3  BLE:5.0\r\n");
  atRaw("Bin version:" FW_VERSION "\r\n\r\nOK\r\n");
}

static void cmdRestore(const char *, bool, bool) {
  atLog("[cfg] factory restore\r\n");
  if (g_cfg.scanning && g_scan) g_scan->stop();
  if (g_client && g_connConnected) { g_client->disconnect(); g_connConnected = false; }
  g_client = nullptr;
  if (g_cfg.bleInited) BLEDevice::deinit();
  g_cfg.scanning  = false;
  g_cfg.bleInited = false;
  g_discRunning = false;
  resultsClear();
  cfgWipe();
  delay(50);
  atRaw("\r\nOK\r\n");
  delay(80);
  ESP.restart();
}

static void cmdHelp(const char *, bool, bool) {
  atRaw("\r\n");
  atRaw("AT                       测试\n");
  atRaw("AT+RST                   重启\n");
  atRaw("AT+GMR                   版本信息\n");
  atRaw("ATE0 / ATE1              关/开回显\n");
  atRaw("AT+RESTORE               恢复出厂并重启\n");
  atRaw("AT+SYSLOG=<0|1>          UART0 日志开关\n");
  atRaw("AT+LOG?                  导出 RAM 日志环\n");
  atRaw("AT+WIFI?                 查询 Wi-Fi 状态（sta/ip/ssid/mdns）\n");
  atRaw("AT+WIFI=<ssid>,<pass>    配网（保存重启连 STA）\n");
  atRaw("AT+WIFI=0                清除凭证，回 AP 热点\n");
  atRaw("AT+BLEINIT?              查询 BLE 初始化状态\n");
  atRaw("AT+BLEINIT=<0|1>         0=deinit 1=init\n");
  atRaw("AT+BLESCAN?              查询扫描状态\n");
  atRaw("AT+BLESCAN=1[,<秒>]      开始扫描（默认 10s，0=不限）\n");
  atRaw("AT+BLESCAN=0             停止扫描\n");
  atRaw("AT+BLESCANRSL            导出本次扫描缓存结果\n");
  atRaw("AT+BLESCANPARM?          查询扫描参数\n");
  atRaw("AT+BLESCANPARM=<a>,<i>,<w>  设 active/interval/window\n");
  atRaw("AT+BLECONN?              查询 GATT 连接状态\n");
  atRaw("AT+BLECONN=1,<mac>[,<t>] 连接（t: 0=public 1=random）\n");
  atRaw("AT+BLECONN=0             断开\n");
  atRaw("AT+BLEGATTC_DISC=<s|c|d|all>  发现 service/characteristic/descriptor/全部\n");
  atRaw("AT+BLEGATTC_READ=<h>     读 value handle\n");
  atRaw("AT+BLEGATTC_WRITE=<h>,<hex>   写（带响应）\n");
  atRaw("AT+BLEGATTC_WRITENR=<h>,<hex> 写（无响应）\n");
  atRaw("AT+BLEGATTC_SUB=<h>,<1|0>    订阅/取消订阅 notify\n");
  atRaw("\n  扫描期间每条广播会主动上报：\n");
  atRaw("    +BLESCAN:<addr_type>,<addr>,<rssi>,<name>,<adv_hex>\n");
  atRaw("  设时长扫描结束会发：\n");
  atRaw("    +BLESCAN:done\n");
  atRaw("  GATT 连接状态变化会发：\n");
  atRaw("    +BLECONN:connected|disconnected,<addr>\n");
  atRaw("  Notify 上报：\n");
  atRaw("    +BLEGATTC_NOTIF:<handle>,<hex>\n");
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

// AT+WIFI?              查询 Wi-Fi 状态（sta/ip/ssid/mdns）
// AT+WIFI=<ssid>,<pass> 配网（保存后重启连 STA）
// AT+WIFI=0             清除凭证，重启回 AP 热点
static void cmdWifi(const char *arg, bool q, bool s) {
  if (q) {
    AT_PORT.printf("\r\n+WIFI:%u,%s,%s,%s\r\n",
                   (unsigned)(g_wifiSta ? 1 : 0),
                   g_ipStr.c_str(),
                   g_wifiSta ? WiFi.SSID().c_str() : "",
                   g_mdnsUrl.c_str());
    atOk();
    return;
  }
  if (!s) { atError(); return; }
  Args a; parseArgs(arg, a);
  if (a.n == 1 && atoiSafe(a.s[0]) == 0) {
    wifiClear();
    atOk();
    delay(400);
    ESP.restart();
    return;
  }
  if (a.n < 2) { atError(); return; }
  wifiSave(a.s[0], a.s[1]);
  atOk();
  delay(400);
  ESP.restart();
}

// ============================================================================
//  BLE 命令
// ============================================================================
// 注意：BLEDevice::init() 是阻塞的，第一次调用会拉起协议栈，约 200~500ms。
// 后续 getScan / start / stop 都是非阻塞的，可以放心放进 loop 周期里。

static void cmdBleInit(const char *arg, bool q, bool s) {
  if (q) {
    AT_PORT.printf("\r\n+BLEINIT:%u\r\n", (unsigned)(g_cfg.bleInited ? 1 : 0));
    atOk();
    return;
  }
  if (!s) { atError(); return; }
  Args a; parseArgs(arg, a);
  if (a.n < 1) { atError(); return; }
  int v = atoiSafe(a.s[0]);

  if (v == 0) {
    if (g_cfg.scanning && g_scan) {
      g_scan->stop();
      g_cfg.scanning = false;
      g_scanEndMs = 0;
    }
    if (g_cfg.bleInited) {
      BLEDevice::deinit();
      g_cfg.bleInited = false;
      g_scan = nullptr;
      atLog("[ble] deinit\r\n");
    }
    atOk();
    return;
  }

  if (v == 1) {
    if (g_cfg.bleInited) { atOk(); return; }
    atLog("[ble] init start\r\n");
    BLEDevice::init(FW_NAME);
    g_scan = BLEDevice::getScan();
    g_scan->setAdvertisedDeviceCallbacks(new BleScanCallbacks(), true);  // wantDuplicates=true
    g_scan->setActiveScan(g_cfg.activeScan);
    g_scan->setInterval(g_cfg.interval);
    g_scan->setWindow(g_cfg.window);
    g_cfg.bleInited = true;
    atLog("[ble] init done\r\n");
    atOk();
    return;
  }
  atError();
}

static void cmdBleScan(const char *arg, bool q, bool s) {
  if (q) {
    AT_PORT.printf("\r\n+BLESCAN:%u,%u\r\n",
                   (unsigned)(g_cfg.scanning ? 1 : 0),
                   (unsigned)(g_cfg.bleInited ? 1 : 0));
    atOk();
    return;
  }
  if (!s) { atError(); return; }
  if (!g_cfg.bleInited || !g_scan) {
    atRaw("\r\n+BLEINIT:0\r\n");
    atError();
    return;
  }

  Args a; parseArgs(arg, a);
  if (a.n < 1) { atError(); return; }
  int on = atoiSafe(a.s[0]);

  if (on == 0) {
    if (!g_cfg.scanning) { atRaw("\r\n+BLESCAN:idle\r\n"); atError(); return; }
    g_scan->stop();
    g_cfg.scanning = false;
    g_scanEndMs = 0;
    atLog("[ble] scan stopped by host\r\n");
    atRaw("\r\n+BLESCAN:stopped\r\n");
    atOk();
    return;
  }

  if (on != 1) { atError(); return; }

  // 开始扫描
  int dur = DEF_DURATION;
  if (a.n >= 2) dur = atoiSafe(a.s[1]);
  if (dur < 0 || dur > MAX_DURATION) { atError(); return; }

  // 已在扫的话先停一下，让栈内部状态干净
  if (g_cfg.scanning) {
    g_scan->stop();
    g_cfg.scanning = false;
  }
  resultsClear();

  // 参数有可能被 BLESCANPARM 改过，重新刷一遍
  g_scan->setActiveScan(g_cfg.activeScan);
  g_scan->setInterval(g_cfg.interval);
  g_scan->setWindow(g_cfg.window);

  // start(duration, continue_scan)：
  //   continue_scan=false → 栈在 duration 秒后自动 stop 并触发 COMPLETE 事件。
  //   duration=0 在 Bluedroid 里等同于「一直扫到手动 stop」。
  bool ok = g_scan->start((uint32_t)dur, false);
  if (!ok) {
    atLog("[ble] scan start failed\r\n");
    atError();
    return;
  }
  g_cfg.scanning = true;
  g_scanStartMs = millis();
  // dur=0 表示不限时；用 0 标记「不主动报 done」
  g_scanEndMs = (dur == 0) ? 0 : (millis() + (uint32_t)dur * 1000);

  atLog("[ble] scan start dur=%d intv=%u wind=%u active=%d\r\n",
        dur, (unsigned)g_cfg.interval, (unsigned)g_cfg.window, (int)g_cfg.activeScan);
  atOk();
}

static void cmdBleScanRsl(const char *, bool, bool) {
  atRaw("\r\n");
  int count = 0;
  for (int i = 0; i < MAX_RESULTS; i++) {
    ScanResult &r = g_results[i];
    if (!r.used) continue;
    AT_PORT.printf("+BLESCANRSL:%u,%s,%d,\"%s\",\"%s\"\r\n",
                   (unsigned)r.addrType, r.addr, r.rssi, r.name, r.advHex);
    count++;
  }
  atLog("[ble] dump results n=%d\r\n", count);
  atOk();
}

static void cmdBleScanParm(const char *arg, bool q, bool s) {
  if (q) {
    AT_PORT.printf("\r\n+BLESCANPARM:%d,%u,%u\r\n",
                   (int)g_cfg.activeScan, (unsigned)g_cfg.interval, (unsigned)g_cfg.window);
    atOk();
    return;
  }
  if (!s) { atError(); return; }
  Args a; parseArgs(arg, a);
  if (a.n < 3) { atError(); return; }
  int act = atoiSafe(a.s[0]);
  int itv = atoiSafe(a.s[1]);
  int win = atoiSafe(a.s[2]);
  if (act != 0 && act != 1) { atError(); return; }
  // BLE 栈约束：4 ≤ window ≤ interval ≤ 16384（单位 0.625ms）
  if (itv < 4 || itv > 16384 || win < 4 || win > itv) { atError(); return; }
  if (g_cfg.scanning) {
    atRaw("\r\n+BLESCAN:busy\r\n");
    atError();
    return;
  }
  g_cfg.activeScan = (act == 1);
  g_cfg.interval   = (uint16_t)itv;
  g_cfg.window     = (uint16_t)win;
  cfgSave();
  if (g_scan) {
    g_scan->setActiveScan(g_cfg.activeScan);
    g_scan->setInterval(g_cfg.interval);
    g_scan->setWindow(g_cfg.window);
  }
  atLog("[ble] parm active=%d intv=%u wind=%u\r\n",
        (int)g_cfg.activeScan, (unsigned)g_cfg.interval, (unsigned)g_cfg.window);
  atOk();
}

// ============================================================================
//  GATT Client 命令
// ============================================================================
//  单连接设计：g_client 全局唯一。ESP32-C3 默认 max_conn=1，做 nRF Connect 式
//  单连调试够用；要多连得改 CONFIG_BT_NIMBLE_MAX_CONNECTIONS 再编。
//
//  注意：Bluedroid 的 client->connect() 默认 30s 超时；getServices() / readValue()
//  会阻塞等 GATT 事务，单条几十毫秒；本工程 AT 命令是单线程处理的，所以一条
//  AT+BLEGATTC_DISC=all 期间，下一条命令会阻塞在 HWCDC rx 队列里（1024 字节够）。
//
//  arduino-esp32 core 默认 -fno-exceptions，所以这里不用 try/catch；
//  BLE 远端对象在远端断开时调用其方法可能 throw std::string → abort 重启。
//  实测 connect 失败会返回 false 而非 throw；getServices/readValue 在已连状态下
//  正常返回。极少数 throw 场景就让它重启，符合「AT 猫」的容错预期。

// ---- 工具：hex 字符串 → 字节向量 ----
static size_t hexToBytes(const char *hex, uint8_t *out, size_t outMax) {
  size_t n = 0;
  const char *p = hex;
  while (*p && n < outMax) {
    while (*p == ' ' || *p == ',') p++;
    if (!*p) break;
    int hi = -1, lo = -1;
    char c1 = *p++;
    if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
    else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
    else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
    if (hi < 0) break;
    char c2 = *p;
    if (c2 >= '0' && c2 <= '9') { lo = c2 - '0'; p++; }
    else if (c2 >= 'a' && c2 <= 'f') { lo = c2 - 'a' + 10; p++; }
    else if (c2 >= 'A' && c2 <= 'F') { lo = c2 - 'A' + 10; p++; }
    else lo = 0;   // 单字符补 0（容错）
    out[n++] = (uint8_t)((hi << 4) | lo);
  }
  return n;
}

static void bytesToHex(const uint8_t *in, size_t n, char *out) {
  static const char *H = "0123456789ABCDEF";
  for (size_t i = 0; i < n; i++) {
    out[i * 2]     = H[in[i] >> 4];
    out[i * 2 + 1] = H[in[i] & 0x0F];
  }
  out[n * 2] = 0;
}

// 把 BLEUUID 转成可打印字符串（16/32/128-bit 都覆盖）
static void uuidToStr(const BLEUUID &u, char *out, size_t outMax) {
  // arduino-esp32 3.x BLEUUID::toString() 返回 String（小写、128-bit 带 -）
  String s = u.toString();
  strncpy(out, s.c_str(), outMax - 1);
  out[outMax - 1] = 0;
  // 转大写，跟扫描 MAC 的展示习惯一致
  for (char *p = out; *p; p++) *p = up(*p);
}

// 连接状态变化回调（client 端）
// arduino-esp32 3.x BLEClientCallbacks::onDisconnect 只有一参数（无 reason）
static void onClientDisconnect(BLEClient *c) {
  (void)c;
  g_connConnected = false;
  g_discRunning = false;
  atLog("[gattc] peer disconnected\r\n");
  AT_PORT.printf("\r\n+BLECONN:disconnected,%s\r\n", g_connAddr);
}

// Notify/Indicate 回调：按 handle 上报
// arduino-esp32 3.x 的回调签名：void(BLERemoteCharacteristic*, uint8_t*, size_t, bool)
static void onNotify(BLERemoteCharacteristic *chr, uint8_t *data, size_t n, bool isNotify) {
  if (!chr) return;
  char hex[600];
  if (n > sizeof(hex) / 2 - 1) n = sizeof(hex) / 2 - 1;
  bytesToHex(data, n, hex);
  uint16_t h = chr->getHandle();
  AT_PORT.printf("\r\n+%s:%u,%s\r\n", isNotify ? "BLEGATTC_NOTIF" : "BLEGATTC_IND",
                 (unsigned)h, hex);
}

// 发现一条 characteristic 的辅助：上报 + 注册 notify 回调（如果支持）
static void reportCharacteristic(BLERemoteCharacteristic *chr) {
  if (!chr) return;
  char uuidStr[40];
  uuidToStr(chr->getUUID(), uuidStr, sizeof(uuidStr));
  // arduino-esp32 3.x：getHandle() 就是 value handle；没有 getProperties()，
  // 用 canRead/canWrite/canNotify 等布尔拼出 props 位掩码
  uint16_t valueH = chr->getHandle();
  uint8_t props = 0;
  if (chr->canRead())            props |= 0x02;  // READ
  if (chr->canWriteNoResponse()) props |= 0x04;  // WRITE_NR
  if (chr->canWrite())           props |= 0x08;  // WRITE
  if (chr->canNotify())          props |= 0x10;  // NOTIFY
  if (chr->canIndicate())        props |= 0x20;  // INDICATE
  AT_PORT.printf("+BLEGATTC_CHR:%u,%u,%s\r\n",
                 (unsigned)valueH, (unsigned)props, uuidStr);
  // 给支持 notify/indicate 的特征挂回调（实际订阅由 AT+BLEGATTC_SUB 触发）
  if (props & (0x10 | 0x20)) {
    chr->registerForNotify(onNotify, true);
  }
}

// ---- AT+BLECONN ----
static void cmdBleConn(const char *arg, bool q, bool s) {
  if (q) {
    AT_PORT.printf("\r\n+BLECONN:%u,\"%s\",%u\r\n",
                   (unsigned)(g_connConnected ? 1 : 0),
                   g_connAddr, (unsigned)g_connAddrType);
    atOk();
    return;
  }
  if (!s) { atError(); return; }
  if (!g_cfg.bleInited) {
    atRaw("\r\n+BLEINIT:0\r\n");
    atError();
    return;
  }
  Args a; parseArgs(arg, a);
  if (a.n < 1) { atError(); return; }
  int op = atoiSafe(a.s[0]);

  if (op == 0) {
    // 断开
    if (!g_client || !g_connConnected) {
      atRaw("\r\n+BLECONN:idle\r\n");
      atError();
      return;
    }
    g_client->disconnect();
    g_connConnected = false;
    g_discRunning = false;
    atLog("[gattc] disconnect by host\r\n");
    AT_PORT.printf("\r\n+BLECONN:disconnected,%s\r\n", g_connAddr);
    atOk();
    return;
  }

  if (op != 1) { atError(); return; }
  if (a.n < 2 || !a.s[1][0]) {
    atRaw("\r\n+BLECONN:need mac\r\n");
    atError();
    return;
  }
  // 已经连着的话先断
  if (g_client && g_connConnected) {
    g_client->disconnect();
    g_connConnected = false;
  }

  // mac 形如 "AA:BB:CC:DD:EE:FF"，BLEAddress 接受这种格式
  uint8_t addrType = 0;
  if (a.n >= 3) addrType = (uint8_t)atoiSafe(a.s[2]);
  if (addrType > 1) addrType = 0;   // 0=public 1=random，C3 客户端只支持这两种

  // 把 mac 转大写存档
  strncpy(g_connAddr, a.s[1], sizeof(g_connAddr) - 1);
  g_connAddr[sizeof(g_connAddr) - 1] = 0;
  for (char *p = g_connAddr; *p; p++) *p = up(*p);
  g_connAddrType = addrType;

  if (!g_client) {
    g_client = BLEDevice::createClient();
    class ClientCb : public BLEClientCallbacks {
    public:
      void onConnect(BLEClient *) override {}
      void onDisconnect(BLEClient *c) override { onClientDisconnect(c); }
    };
    g_client->setClientCallbacks(new ClientCb());
    atLog("[gattc] createClient done\r\n");
  }

  atLog("[gattc] connecting %s type=%u\r\n", g_connAddr, (unsigned)addrType);
  // BLEAddress 第二参数带 addr_type；connect(BLEAddress, type=0xFF, timeout=30s)，
  // type=0xFF 让栈自动选连接类型，NimBLE/Bluedroid 都兼容。
  bool ok = g_client->connect(BLEAddress(String(g_connAddr), addrType));
  if (!ok) {
    atLog("[gattc] connect failed\r\n");
    g_connConnected = false;
    atRaw("\r\n+BLECONN:failed\r\n");
    atError();
    return;
  }
  g_connConnected = true;
  atLog("[gattc] connected\r\n");
  AT_PORT.printf("\r\n+BLECONN:connected,%s\r\n", g_connAddr);
  atOk();
}

// ---- AT+BLEGATTC_DISC ----
static void cmdBleGattcDisc(const char *arg, bool q, bool s) {
  if (q) {
    AT_PORT.printf("\r\n+BLEGATTC_DISC:%u\r\n", (unsigned)(g_discRunning ? 1 : 0));
    atOk();
    return;
  }
  if (!s) { atError(); return; }
  if (!g_client || !g_connConnected) {
    atRaw("\r\n+BLECONN:0\r\n");
    atError();
    return;
  }
  Args a; parseArgs(arg, a);
  if (a.n < 1 || !a.s[0][0]) { atError(); return; }
  const char *what = a.s[0];
  // s / c / d / all
  bool doS = what[0] == 's' || what[0] == 'S' || what[0] == 'a' || what[0] == 'A';
  bool doC = what[0] == 'c' || what[0] == 'C' || what[0] == 'a' || what[0] == 'A';
  bool doD = what[0] == 'd' || what[0] == 'D' || what[0] == 'a' || what[0] == 'A';
  if (!doS && !doC && !doD) { atError(); return; }

  g_discRunning = true;
  atLog("[gattc] disc %s start\r\n", what);

  // 取 services 映射指针（arduino-esp32 3.x getServices 返回 map 指针，
  // key 是 UUID 字符串；不要 delete，库自管）
  std::map<std::string, BLERemoteService *> *svcs = g_client->getServices();
  if (!svcs) {
    g_discRunning = false;
    AT_PORT.printf("\r\n+BLEGATTC_DISC:done\r\n");
    atOk();
    return;
  }

  for (auto &kv : *svcs) {
    BLERemoteService *svc = kv.second;
    if (!svc) continue;
    if (doS) {
      char uuidStr[40];
      uuidToStr(svc->getUUID(), uuidStr, sizeof(uuidStr));
      // start handle（end handle 在库里是 private，不暴露）
      uint16_t sh = svc->getHandle();
      AT_PORT.printf("+BLEGATTC_SVC:%u,%s\r\n", (unsigned)sh, uuidStr);
    }
    if (doC || doD) {
      std::map<std::string, BLERemoteCharacteristic *> *chrs = svc->getCharacteristics();
      if (chrs) {
        for (auto &ckv : *chrs) {
          BLERemoteCharacteristic *chr = ckv.second;
          if (!chr) continue;
          if (doC) reportCharacteristic(chr);
          if (doD) {
            std::map<std::string, BLERemoteDescriptor *> *descs = chr->getDescriptors();
            if (descs) {
              for (auto &dkv : *descs) {
                BLERemoteDescriptor *dsc = dkv.second;
                if (!dsc) continue;
                char uuidStr[40];
                uuidToStr(dsc->getUUID(), uuidStr, sizeof(uuidStr));
                AT_PORT.printf("+BLEGATTC_DESC:%u,%s\r\n",
                               (unsigned)dsc->getHandle(), uuidStr);
              }
            }
          }
        }
      }
    }
  }

  g_discRunning = false;
  atLog("[gattc] disc done\r\n");
  AT_PORT.printf("\r\n+BLEGATTC_DISC:done\r\n");
  atOk();
}

// 按 value handle 找 characteristic（遍历所有 service）
static BLERemoteCharacteristic *findChrByValueHandle(uint16_t handle) {
  if (!g_client || !g_connConnected) return nullptr;
  std::map<std::string, BLERemoteService *> *svcs = g_client->getServices();
  if (!svcs) return nullptr;
  for (auto &kv : *svcs) {
    BLERemoteService *svc = kv.second;
    if (!svc) continue;
    std::map<std::string, BLERemoteCharacteristic *> *chrs = svc->getCharacteristics();
    if (!chrs) continue;
    for (auto &ckv : *chrs) {
      BLERemoteCharacteristic *chr = ckv.second;
      if (chr && chr->getHandle() == handle) return chr;
    }
  }
  return nullptr;
}

// ---- AT+BLEGATTC_READ ----
static void cmdBleGattcRead(const char *arg, bool q, bool s) {
  (void)q;
  if (!s) { atError(); return; }
  if (!g_client || !g_connConnected) {
    atRaw("\r\n+BLECONN:0\r\n");
    atError();
    return;
  }
  Args a; parseArgs(arg, a);
  if (a.n < 1) { atError(); return; }
  uint16_t handle = (uint16_t)atoiSafe(a.s[0]);
  BLERemoteCharacteristic *chr = findChrByValueHandle(handle);
  if (!chr) {
    atRaw("\r\n+BLEGATTC:notfound\r\n");
    atError();
    return;
  }
  if (!chr->canRead()) {
    atRaw("\r\n+BLEGATTC:noread\r\n");
    atError();
    return;
  }
  // arduino-esp32 3.x readValue() 返回 String
  String v = chr->readValue();
  char hex[600];
  size_t vn = v.length();
  if (vn > sizeof(hex) / 2 - 1) vn = sizeof(hex) / 2 - 1;
  bytesToHex((const uint8_t *)v.c_str(), vn, hex);
  AT_PORT.printf("\r\n+BLEGATTC_READ:%u,%s\r\n", (unsigned)handle, hex);
  atOk();
}

// ---- AT+BLEGATTC_WRITE / WRITENR ----
static void doGattcWrite(const char *arg, bool response) {
  if (!g_client || !g_connConnected) {
    atRaw("\r\n+BLECONN:0\r\n");
    atError();
    return;
  }
  Args a; parseArgs(arg, a);
  if (a.n < 2) { atError(); return; }
  uint16_t handle = (uint16_t)atoiSafe(a.s[0]);
  uint8_t buf[250];
  size_t n = hexToBytes(a.s[1], buf, sizeof(buf));
  if (n == 0) {
    atRaw("\r\n+BLEGATTC:emptydata\r\n");
    atError();
    return;
  }
  BLERemoteCharacteristic *chr = findChrByValueHandle(handle);
  if (!chr) {
    atRaw("\r\n+BLEGATTC:notfound\r\n");
    atError();
    return;
  }
  chr->writeValue(buf, n, response);
  atOk();
}

static void cmdBleGattcWrite(const char *arg, bool q, bool s) {
  (void)q;
  if (!s) { atError(); return; }
  doGattcWrite(arg, true);
}

static void cmdBleGattcWriteNr(const char *arg, bool q, bool s) {
  (void)q;
  if (!s) { atError(); return; }
  doGattcWrite(arg, false);
}

// ---- AT+BLEGATTC_SUB ----
// AT+BLEGATTC_SUB=<value_handle>,<1|0>
// 1 = 订阅 notify/indicate（按 props 自动选），0 = 取消
static void cmdBleGattcSub(const char *arg, bool q, bool s) {
  (void)q;
  if (!s) { atError(); return; }
  if (!g_client || !g_connConnected) {
    atRaw("\r\n+BLECONN:0\r\n");
    atError();
    return;
  }
  Args a; parseArgs(arg, a);
  if (a.n < 2) { atError(); return; }
  uint16_t handle = (uint16_t)atoiSafe(a.s[0]);
  int on = atoiSafe(a.s[1]);
  BLERemoteCharacteristic *chr = findChrByValueHandle(handle);
  if (!chr) {
    atRaw("\r\n+BLEGATTC:notfound\r\n");
    atError();
    return;
  }
  // arduino-esp32 3.x 没有 getProperties()，用 canNotify/canIndicate
  bool canN = chr->canNotify();
  bool canI = chr->canIndicate();
  if (!canN && !canI) {
    atRaw("\r\n+BLEGATTC:nonotify\r\n");
    atError();
    return;
  }
  BLERemoteDescriptor *cccd = chr->getDescriptor(BLEUUID((uint16_t)0x2902));
  if (!cccd) {
    atRaw("\r\n+BLEGATTC:nocccd\r\n");
    atError();
    return;
  }
  uint8_t v = on ? (canN ? 0x01 : 0x02) : 0x00;
  cccd->writeValue(&v, 1, true);
  atLog("[gattc] sub h=%u on=%d\r\n", (unsigned)handle, on);
  atOk();
}

// ============================================================================
//  Wi-Fi 配网 + HTTP/WS server（局域网访问层）
//  与 USB-CDC 并存：USB 永远在，Wi-Fi 起来后 WS 也开，两条通道收发互通。
// ============================================================================
#define AP_SSID_PREFIX   "BLE-Scan-"   // 配网热点前缀，后接 MAC 后两字节
#define AP_PASSWORD      "12345678"
#define MDNS_HOST_PREFIX "blescan-"    // mDNS 主机名前缀，访问 http://blescan-XXXX.local
#define WIFI_STA_TIMEOUT_MS  20000
#define WIFI_RETRY_INTERVAL  30000

static WebServer g_http(80);

static const char* macSuffix() {
  static char s[5] = {0};
  if (s[0] == 0) {
    uint64_t e = ESP.getEfuseMac();
    snprintf(s, sizeof(s), "%02X%02X", (uint8_t)(e >> 8), (uint8_t)e);
  }
  return s;
}

static String wifiHostname() {
  String h = String(MDNS_HOST_PREFIX) + macSuffix();
  h.toLowerCase();
  return h;
}

static String jsonEsc(const String &in) {
  String o; o.reserve(in.length() + 4);
  for (unsigned i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

static void wifiLoad() {
  Preferences p;
  p.begin("wifi", true);
  g_wifiSsid = p.getString("ssid", "");
  g_wifiPass = p.getString("pass", "");
  p.end();
}

static void wifiSave(const String &ssid, const String &pass) {
  Preferences p;
  p.begin("wifi", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}

static void wifiClear() {
  Preferences p;
  p.begin("wifi", false);
  p.remove("ssid");
  p.remove("pass");
  p.end();
  g_wifiSsid = g_wifiPass = "";
}

static void startMDNS() {
  String h = wifiHostname();
  if (MDNS.begin(h.c_str())) {
    MDNS.addService("http", "tcp", 80);
    g_mdnsUrl = "http://" + h + ".local";
    atLog("[net] mDNS %s\r\n", g_mdnsUrl.c_str());
  }
}

// 启动网络：有凭证连 STA，失败/无凭证回退 AP+STA 热点
static void wifiSetup() {
  wifiLoad();
  if (g_wifiSsid.length() > 0) {
    atLog("[net] 连 STA [%s]\r\n", g_wifiSsid.c_str());
    WiFi.setHostname(wifiHostname().c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_STA_TIMEOUT_MS) delay(300);
    if (WiFi.status() == WL_CONNECTED) {
      g_wifiSta = true;
      g_ipStr = WiFi.localIP().toString();
      atLog("[net] STA up ip=%s\r\n", g_ipStr.c_str());
      startMDNS();
      return;
    }
    atLog("[net] STA fail status=%d, fallback AP\r\n", (int)WiFi.status());
  } else {
    atLog("[net] no creds, AP mode\r\n");
  }
  // 热点模式（AP+STA 共存，便于后台补连）
  g_wifiSta = false;
  String apSsid = String(AP_SSID_PREFIX) + macSuffix();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apSsid.c_str(), AP_PASSWORD, 1, 0, 4);
  g_ipStr = WiFi.softAPIP().toString();
  startMDNS();
  atLog("[net] AP up ssid=%s ip=%s\r\n", apSsid.c_str(), g_ipStr.c_str());
}

// 保活：STA 掉线重连 / AP 模式后台补连 STA
static void wifiEnsure() {
  static uint32_t last = 0;
  if (g_wifiSsid.length() == 0) return;
  if (millis() - last < WIFI_RETRY_INTERVAL) return;
  last = millis();
  if (g_wifiSta) {
    if (WiFi.status() != WL_CONNECTED) {
      atLog("[net] STA down, reconnect\r\n");
      WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());
    }
    return;
  }
  // AP 模式后台试连
  WiFi.begin(g_wifiSsid.c_str(), g_wifiPass.c_str());
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t) < 8000) delay(300);
  if (WiFi.status() == WL_CONNECTED) {
    g_wifiSta = true;
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    g_ipStr = WiFi.localIP().toString();
    startMDNS();
    atLog("[net] auto STA up ip=%s\r\n", g_ipStr.c_str());
  }
}

// ---- 配网页（AP 模式时 / 返回这个；STA 模式 / 返回业务页 index.html）----
static const char PROV_HTML[] PROGMEM = R"rawliteral(
<!doctype html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BLE-Scan 配网</title><style>
*{box-sizing:border-box}body{font-family:-apple-system,"PingFang SC",sans-serif;
margin:0;padding:16px;background:#f2f3f5;color:#222;font-size:15px}
.w{max-width:460px;margin:0 auto}h1{font-size:19px;margin:6px 0 16px}
.c{background:#fff;border-radius:12px;padding:16px;margin-bottom:14px;box-shadow:0 1px 3px rgba(0,0,0,.07)}
label{display:block;font-size:12px;color:#888;margin:10px 0 4px}
input{width:100%;padding:10px;border:1px solid #dde;border-radius:8px;font-size:15px}
button{padding:10px 16px;border:0;border-radius:8px;background:#2b6cff;color:#fff;cursor:pointer}
.r{display:flex;gap:8px;margin-top:14px}.s{font-size:14px;line-height:2}.m{color:#888;font-size:12px}
</style></head><body><div class="w"><h1>BLE-Scan 设备配网</h1>
<div class="c"><div class="s">模式: <b id="m">读取中</b><br>IP: <b id="i">-</b><br>地址: <b id="u">-</b></div></div>
<div class="c"><h2>连接 Wi-Fi</h2>
<label>Wi-Fi 名称</label><input id="s" autocomplete="off" spellcheck="false">
<label>密码</label><input id="p" type="password" autocomplete="off">
<div class="r"><button onclick="sv()">保存并连接</button></div>
<div class="m" id="h">仅支持 2.4GHz。保存后设备重启并连接，连上后本页自动跳业务页。</div>
</div></div><script>
function rf(){fetch('/api/status').then(r=>r.json()).then(s=>{
$('m').innerText=s.sta?'已连接局域网':'配网热点';$('i').innerText=s.ip;
$('u').innerText=s.mdns||'-';if(s.mdns)_m=s.mdns;}).catch(()=>{});}
var _m='';function $(id){return document.getElementById(id);}
function sv(){var ss=$('s').value.trim(),pp=$('p').value;if(!ss){alert('请填 SSID');return;}
$('h').innerText='保存中...';
fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'ssid='+encodeURIComponent(ss)+'&password='+encodeURIComponent(pp)}).then(r=>{
if(!r.ok)throw 0;$('h').innerText='已保存，设备重启中。切回 '+ss+' 后访问 '+(_m||'设备地址');
window.addEventListener('online',()=>{if(_m)location.href=_m;});
}).catch(()=>{$('h').innerText='保存失败';});}
rf();setInterval(rf,3000);
</script></body></html>
)rawliteral";

// ---- HTTP handlers ----
// 业务页始终在上位机本地（index.html），dongle 只做 WS 数据通道 + 配网页。
// 已联网时 / 返回状态 JSON，让上位机发现设备 IP/mDNS。
static void httpRoot() {
  if (g_wifiSta) {
    String j = "{\"sta\":true,\"ip\":\"" + g_ipStr + "\",\"ssid\":\"" +
               jsonEsc(WiFi.SSID()) + "\",\"mdns\":\"" + g_mdnsUrl + "\"}";
    g_http.send(200, "application/json", j);
  } else {
    g_http.send_P(200, "text/html", PROV_HTML);
  }
}

static void httpStatus() {
  String j = "{";
  j += "\"sta\":"    + String(g_wifiSta ? "true" : "false") + ",";
  j += "\"ip\":\""   + g_ipStr + "\",";
  j += "\"ssid\":\"" + jsonEsc(g_wifiSta ? WiFi.SSID() : String("")) + "\",";
  j += "\"mdns\":\"" + g_mdnsUrl + "\",";
  j += "\"fw\":\""   + String(FW_NAME) + " " + FW_VERSION + "\"";
  j += "}";
  g_http.send(200, "application/json", j);
}

static void httpWifi() {
  String ssid = g_http.arg("ssid");
  String pass = g_http.arg("password");
  ssid.trim();
  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64) {
    g_http.send(400, "text/plain", "bad ssid/pass");
    return;
  }
  atLog("[net] save wifi [%s]\r\n", ssid.c_str());
  wifiSave(ssid, pass);
  g_http.send(200, "application/json", "{\"ok\":true}");
  delay(600);
  ESP.restart();
}

static void httpForget() {
  atLog("[net] forget wifi\r\n");
  wifiClear();
  g_http.send(200, "application/json", "{\"ok\":true}");
  delay(600);
  ESP.restart();
}

static void httpSetup() {
  g_http.on("/",              HTTP_GET,  httpRoot);
  g_http.on("/api/status",    HTTP_GET,  httpStatus);
  g_http.on("/api/wifi",      HTTP_POST, httpWifi);
  g_http.on("/api/forgetwifi",HTTP_POST, httpForget);
  g_http.onNotFound([]() { g_http.send(404, "text/plain", "not found"); });
  g_http.begin();
  atLog("[net] HTTP 80 up\r\n");
}

// ---- WebSocket：收消息喂 feedAtByte ----
static void wsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t len) {
  (void)num;
  switch (type) {
    case WStype_CONNECTED:
      atLog("[ws] client #%u connected\r\n", (unsigned)num);
      break;
    case WStype_DISCONNECTED:
      atLog("[ws] client #%u gone\r\n", (unsigned)num);
      break;
    case WStype_TEXT:
    case WStype_BIN:
      // 把收到的字节当 AT 输入流喂进去（和 USB-CDC 同一条解析路径）
      for (size_t i = 0; i < len; i++) feedAtByte(payload[i]);
      break;
    default: break;
  }
}

static void wsSetup() {
  g_ws.begin();
  g_ws.onEvent(wsEvent);
  g_wsReady = true;
  atLog("[net] WS 81 up\r\n");
}

// ============================================================================
//  setup / loop
// ============================================================================
void setup() {
  // 1) AT 口：RX 环加大到 1024，BLESCAN 期间上位机发的命令得先在环里等
  Serial.setRxBufferSize(1024);
  Serial.begin(115200);

  // 2) 日志口 UART0
  LOG_PORT.begin(115200);

  // 3) 丢掉上电瞬间的残字节
  delay(30);
  while (Serial.available()) Serial.read();

  // 4) 配置
  cfgDefaults();
  cfgLoad();

  atLog("\r\n==== %s v%s ====\r\n", FW_NAME, FW_VERSION);
  atLog("[cfg] echo=%d syslog=%d active=%d intv=%u wind=%u\r\n",
        (int)g_cfg.echo, (int)g_cfg.syslog, (int)g_cfg.activeScan,
        (unsigned)g_cfg.interval, (unsigned)g_cfg.window);

  // 5) 网络（配网或连 STA）+ HTTP/WS
  wifiSetup();
  httpSetup();
  wsSetup();

  atRaw("\r\nready\r\n");
}

void loop() {
  pumpAtPort();     // 收 AT 命令（USB-CDC）
  g_http.handleClient();
  g_ws.loop();
  wifiEnsure();

  // 检查定时扫描是否到期。BLE 栈本身会在 duration 后 stop，这里只是
  // 同步本地状态 + 给上位机发一个 +BLESCAN:done URC。
  if (g_cfg.scanning && g_scanEndMs != 0) {
    uint32_t now = millis();
    // 用 int32_t 减法避免 uint 回绕：now - end 在回绕场景下会是负数
    if ((int32_t)(now - g_scanEndMs) >= 0) {
      // 栈已自动 stop；显式 stop() 一下兜底（多次调用是安全的）
      g_scan->stop();
      g_cfg.scanning = false;
      g_scanEndMs = 0;
      atLog("[ble] scan done after %u ms\r\n", (unsigned)(millis() - g_scanStartMs));
      atRaw("\r\n+BLESCAN:done\r\n");
    }
  }

  delay(1);
}
