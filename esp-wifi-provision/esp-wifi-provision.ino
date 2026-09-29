/*
  ESP Wi-Fi 配网工程 (Wi-Fi Provisioning Template)
  ==================================================
  一个可直接拖进任意 ESP32 / ESP8266 Arduino 项目的配网模板。
  只做配网，不做业务——业务写在自己的 sketch 里，通过 onNetworkReady() 钩子接入。

  参考实现：Cmdmac/ESP-Switch（ESP32_Light_Switch）的配网部分，并做了增强。
  已吸收的踩坑经验（改动前请先读完这几点）：
    1. 启动先试 STA（有凭证时），失败才回退 AP。回退时必须用 WIFI_AP_STA，
       不能用纯 WIFI_AP——纯 AP 模式下 esp_wifi_connect() 会被底层拒绝，自动重连永远无效。
    2. 设备唯一编号必须用 ESP.getEfuseMac()（出厂固化），不能用 WiFi.macAddress()：
       AP/STA 模式返回的 MAC 不同（AP = base+1），会导致热点名后缀与连上后 mDNS 后缀不一致。
    3. WiFi.setHostname() 必须在 WiFi.mode() 之前调用才生效。
    4. WiFi.setSleep(false) 关掉调制解调器休眠，否则局域网 ping 会有 ~100ms 抖动和偶发丢包。
    5. 配网表单用 application/x-www-form-urlencoded 的 POST body 传参。
       部分 ESP32 WebServer 版本对「POST + URL 查询串」不解析，会导致密码变空。
    6. ESP8266 没有 Preferences 库，用 EEPROM 模拟；且结构体必须定义在「最后一个 #include 之前」，
       否则 Arduino 自动生成的前向原型看不到该类型，会报 "was not declared in this scope"。

  芯片支持：ESP32-C3 / ESP32-C2 / ESP32 / ESP8266(ESP8285)，同一份代码靠 #ifdef 适配。

  ------------------------------------------------------------------
  使用方法
  ------------------------------------------------------------------
  1. 把本文件重命名为你的项目名（Arduino 要求 .ino 文件名与所在目录同名）。
  2. 按需修改上方「用户配置」区。
  3. 在 onNetworkReady() 里写你的业务初始化代码（网络就绪后调用一次）。
  4. 编译上传。首次上电没有凭证，设备会开热点，用手机连上后打开 http://192.168.4.1 配网。

  ------------------------------------------------------------------
  配网流程
  ------------------------------------------------------------------
  [上电] → 有凭证? --是--> 连 STA(20s) --成功--> STA 模式 + mDNS，跑业务
                                  |
                                  失败
                                  ↓
              无凭证 / 连接失败 --> AP+STA 热点模式
                                     |  手机连热点 → 192.168.4.1 → 填 SSID/密码
                                     |  保存 → 重启 → 回到开头
                                     ↓
                        后台每 30s 自动重试已保存的 Wi-Fi，
                        连上后自动切成纯 STA（热点关闭），无需手动干预
*/

// ==================== 用户配置 ====================
#define FW_NAME              "esp-wifi-provision"  // 页面标题用
#define FW_VERSION           "1.0.0"

#define AP_SSID_PREFIX       "ESP-Config-"  // 热点前缀，后接 MAC 后两字节（如 ESP-Config-1A2B）
#define AP_PASSWORD          "12345678"     // 至少 8 位；留空字符串则开开放热点
#define AP_CHANNEL           1
#define AP_MAX_CLIENTS       4

#define MDNS_HOST_PREFIX     "esp-"         // mDNS 主机名前缀，后接 MAC 后两字节
#define MDNS_SERVICE_NAME    "Wi-Fi 配网"     // mDNS 实例名

#define WIFI_CONNECT_TIMEOUT_MS  20000      // 启动时 STA 连接超时
#define WIFI_RETRY_TIMEOUT_MS    8000       // AP 模式下后台重试单次超时
#define WIFI_RETRY_INTERVAL_MS   30000      // 后台重试间隔

#define ENABLE_FACTORY_RESET_BUTTON 1       // 长按 BOOT 键恢复配网（1=启用）
#define FACTORY_RESET_PIN           9       // ESP32-C3/C2=9(板载BOOT)，ESP32=0，ESP8266=0
#define FACTORY_RESET_HOLD_MS       3000    // 长按多久触发

#define ENABLE_WIFI_SCAN            1       // 页面提供「扫描周边网络」（注意 AP 信道会短暂跳变）

// ==================== 依赖 ====================
#ifdef ESP8266
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  #include <ESP8266mDNS.h>
  #include <EEPROM.h>

  // ---- ESP8266 专用：必须放在「最后一个 #include」之前（见文件头说明 6）----
  #define EEPROM_SIZE 128
  typedef struct {
    uint32_t magic;                 // 用于判断是否已初始化
    char     ssid[33];
    char     pass[65];
  } ProvStore;
  #define PROV_MAGIC 0x50524F56u   // "PROV"
#else
  #include <WiFi.h>
  #include <WebServer.h>
  #include <ESPmDNS.h>
  #include <Preferences.h>
#endif

// 注：需要 NTP 时间的话，ESP8266 加 #include <TZ.h> + configTime()，
//     ESP32 直接 configTime(gmtOffset, daylightOffset, "ntp.aliyun.com")，两边头文件不同。

// ==================== 全局对象 ====================
#ifdef ESP8266
  ESP8266WebServer   server(80);
#else
  WebServer          server(80);
  Preferences        prefs;
#endif

// 配网状态
String wifiSsid  = "";
String wifiPass  = "";
bool   staMode   = false;    // true = 已连上局域网；false = 热点模式
String currentIP = "";

// 前端用的 mDNS 地址（配网成功后提示用户跳转）
String mdnsUrl = "";

// ==================== 持久化（ESP32=Preferences / ESP8266=EEPROM）====================
#ifdef ESP8266
// 注意：EEPROM.begin() 只在 setup() 里调用一次（见 setup），此处不再重复 begin/end
static void eepromRead(ProvStore& s) {
  EEPROM.get(0, s);
  if (s.magic != PROV_MAGIC) {          // 首次上电，初始化
    memset(&s, 0, sizeof(s));
    s.magic = PROV_MAGIC;
  }
}
static void eepromWrite(const ProvStore& s) {
  EEPROM.put(0, s);
  EEPROM.commit();
}
#endif

void loadWiFi() {
#ifdef ESP8266
  ProvStore s; eepromRead(s);
  wifiSsid = String(s.ssid);
  wifiPass = String(s.pass);
#else
  prefs.begin("prov", true);            // 只读打开
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  prefs.end();
#endif
}

void saveWiFi(const String& ssid, const String& pass) {
#ifdef ESP8266
  ProvStore s; eepromRead(s);
  memset(s.ssid, 0, sizeof(s.ssid));
  memset(s.pass, 0, sizeof(s.pass));
  strncpy(s.ssid, ssid.c_str(), sizeof(s.ssid) - 1);
  strncpy(s.pass, pass.c_str(), sizeof(s.pass) - 1);
  eepromWrite(s);
#else
  prefs.begin("prov", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
#endif
}

void clearWiFi() {
#ifdef ESP8266
  ProvStore s; eepromRead(s);
  memset(s.ssid, 0, sizeof(s.ssid));
  memset(s.pass, 0, sizeof(s.pass));
  eepromWrite(s);
#else
  prefs.begin("prov", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
#endif
  wifiSsid = "";
  wifiPass = "";
}

// ==================== 设备唯一编号 ====================
// 用出厂固化的 ID，保证 AP 热点名后缀与 mDNS 域名后缀完全一致（见文件头说明 2）
const char* getMacSuffix() {
  static char suffix[5] = {0};
  if (suffix[0] == 0) {
#ifdef ESP8266
    uint32_t id = ESP.getChipId();
    snprintf(suffix, sizeof(suffix), "%02X%02X", (uint8_t)(id >> 8), (uint8_t)id);
#else
    uint64_t efuse = ESP.getEfuseMac();
    snprintf(suffix, sizeof(suffix), "%02X%02X", (uint8_t)(efuse >> 8), (uint8_t)efuse);
#endif
  }
  return suffix;
}

String getHostname() {
  String h = String(MDNS_HOST_PREFIX) + getMacSuffix();
  h.toLowerCase();                     // mDNS 主机名规范要求小写
  return h;
}

// JSON 字符串转义：SSID 里可能含引号或反斜杠，不转义会破坏前端的 JSON.parse
String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 4);
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

// ==================== 业务钩子 ====================
// STA 连上网络后调用一次（首次上电即连上，或之后后台自动补连上时）。
// 把你的业务初始化写在这里：注册更多路由、启动传感器任务、连 MQTT 等。
// 不需要业务就保留空实现。
void onNetworkReady() {
  // 默认空实现。示例：
  // Serial.println("[APP] 网络就绪");
}

// ==================== 网络 ====================
void startMDNS() {
  String host = getHostname();
  if (MDNS.begin(host.c_str())) {
    MDNS.setInstanceName(MDNS_SERVICE_NAME);   // 老版本 core 若无此方法，删掉本行
    MDNS.addService("http", "tcp", 80);
    mdnsUrl = "http://" + host + ".local";
    Serial.printf("[mDNS] %s\n", mdnsUrl.c_str());
  } else {
    Serial.println("[mDNS] 启动失败");
  }
}

// 启动网络：有凭证先试 STA，失败回退 AP+STA 热点
void setupNetwork() {
  loadWiFi();

  if (wifiSsid.length() > 0) {
    Serial.printf("[WiFi] 尝试连接: SSID=[%s]\n", wifiSsid.c_str());

    WiFi.setHostname(getHostname().c_str());   // 必须在 mode() 之前（见文件头说明 3）
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);                      // 见文件头说明 4
#ifdef ESP8266
    WiFi.persistent(true);
    WiFi.setAutoReconnect(true);
#endif
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
      delay(300);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      staMode   = true;
      currentIP = WiFi.localIP().toString();
      Serial.printf("[WiFi] 已连上局域网, IP=%s\n", currentIP.c_str());
      startMDNS();
      return;
    }
    Serial.printf("[WiFi] 连接失败(status=%d), 回退热点模式\n", WiFi.status());
  } else {
    Serial.println("[WiFi] 无保存的凭证, 直接进入配网热点");
  }

  // ---- 热点模式 ----
  staMode = false;
  String apSsid = String(AP_SSID_PREFIX) + getMacSuffix();

  // 必须 AP+STA 共存：纯 WIFI_AP 下 esp_wifi_connect() 会被底层拒绝（见文件头说明 1）
  WiFi.mode(WIFI_AP_STA);
  if (strlen(AP_PASSWORD) >= 8) {
    WiFi.softAP(apSsid.c_str(), AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CLIENTS);
  } else {
    WiFi.softAP(apSsid.c_str(), NULL, AP_CHANNEL, 0, AP_MAX_CLIENTS);   // 开放热点
  }
  currentIP = WiFi.softAPIP().toString();
  startMDNS();   // 热点模式也开 mDNS：连上热点即可用 http://esp-XXXX.local

  Serial.println("============ 配网热点已启动 ============");
  Serial.printf("  SSID    : %s\n", apSsid.c_str());
  Serial.printf("  密码    : %s\n", strlen(AP_PASSWORD) >= 8 ? AP_PASSWORD : "(开放)");
  Serial.printf("  配置页  : http://%s 或 %s\n", currentIP.c_str(), mdnsUrl.c_str());
  Serial.println("========================================");
}

// 保活 / 自动联网（每 30s 一次）
//  - STA 模式：掉线则用保存的凭证重连
//  - AP 模式：尝试连已保存的 Wi-Fi，成功则切纯 STA（关热点）
//    这样开机时因信号弱 / DHCP 慢导致 20s 超时的场景，之后能自动补连上，无需手动重配
void ensureWiFi() {
  static unsigned long lastCheck = 0;

  if (wifiSsid.length() == 0) return;                 // 没配过，不重试
  if (millis() - lastCheck < WIFI_RETRY_INTERVAL_MS) return;
  lastCheck = millis();

  if (staMode) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] STA 掉线, 用已保存凭证重连...");
      WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    }
    return;
  }

  // AP 模式：尝试自动连上已保存的 Wi-Fi
  Serial.printf("[WiFi] 后台重试连接 SSID=[%s]\n", wifiSsid.c_str());
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t) < WIFI_RETRY_TIMEOUT_MS) {
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    staMode = true;
    WiFi.mode(WIFI_STA);        // 切纯 STA，关掉热点
    WiFi.setSleep(false);
    currentIP = WiFi.localIP().toString();
    startMDNS();
    Serial.printf("[WiFi] 自动连接成功, 已切换为 STA, IP=%s\n", currentIP.c_str());
    onNetworkReady();
  } else {
    // status 参考(WL_*)：1=无SSID 2=扫描中 3=连接中 4=连接失败 5=连接丢失 255=未知
    // 常见 4 = 密码错或信号太弱；找不到网络多为路由器只开 5G（ESP 只支持 2.4G）
    Serial.printf("[WiFi] 自动连接失败(status=%d), 保持热点\n", WiFi.status());
  }
}

// ==================== 配网页面 ====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html><html lang="zh"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wi-Fi 配网</title>
<style>
  *{box-sizing:border-box}
  body{font-family:-apple-system,"PingFang SC",sans-serif;margin:0;padding:16px;
       background:#f2f3f5;color:#222;font-size:15px}
  .wrap{max-width:460px;margin:0 auto}
  h1{font-size:19px;margin:6px 0 16px}
  .card{background:#fff;border-radius:12px;padding:16px;margin-bottom:14px;
        box-shadow:0 1px 3px rgba(0,0,0,.07)}
  h2{font-size:15px;margin:0 0 12px;color:#333}
  label{display:block;font-size:12px;color:#888;margin:10px 0 4px}
  input{width:100%;padding:10px;border:1px solid #dde;border-radius:8px;font-size:15px}
  button{padding:10px 16px;border:0;border-radius:8px;background:#2b6cff;color:#fff;
         font-size:15px;cursor:pointer}
  button.sec{background:#e8ebf0;color:#333}
  button:disabled{opacity:.5}
  .row{display:flex;gap:8px;margin-top:14px;flex-wrap:wrap}
  .st{font-size:14px;line-height:2}
  .st b{font-weight:600}
  .muted{color:#888;font-size:12px}
  ul{list-style:none;margin:12px 0 0;padding:0;max-height:240px;overflow:auto}
  li{padding:10px;border-bottom:1px solid #f0f0f3;font-size:14px;cursor:pointer;
     display:flex;justify-content:space-between}
  li:active{background:#f4f6fa}
  li .rssi{color:#999;font-size:12px}
  .ok{color:#17a34a}
  .err{color:#d33}
  .hint{font-size:12px;color:#999;margin-top:10px;line-height:1.6}
  .done{display:none;background:#eef7ee;border:1px solid #cfe8cf;border-radius:12px;
        padding:16px;margin-bottom:14px;font-size:14px;line-height:1.8}
  .tagp{display:inline-block;background:#eef;color:#335;border-radius:10px;
        padding:2px 8px;font-size:12px;margin-left:6px}
</style></head><body><div class="wrap">

<h1>设备 Wi-Fi 配网</h1>

<div class="card">
  <h2>当前状态</h2>
  <div class="st">
    模式: <b id="mode">读取中...</b><br>
    IP: <b id="ip">-</b><br>
    已连网络: <b id="ssid">-</b><span id="rssi"></span><br>
    管理地址: <b id="mdns">-</b>
  </div>
</div>

<div class="done" id="done">
  <b class="ok">配置已保存，设备正在重启并连接 Wi-Fi。</b><br>
  请把手机切回「<b id="doneSsid"></b>」，网络恢复后本页会自动打开设备页面。<br>
  <span class="muted">若没自动跳转，手动访问 <b id="doneUrl"></b></span>
</div>

<div class="card">
  <h2>连接到 Wi-Fi</h2>
  <label>Wi-Fi 名称 (SSID)</label>
  <input id="ssid" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="选择下方列表或手动输入">
  <label>密码</label>
  <input id="pwd" type="password" autocomplete="off" placeholder="开放网络留空">
  <div class="row">
    <button id="btnSave" onclick="save()">保存并连接</button>
    <button class="sec" id="btnScan" onclick="scan()">扫描周边网络</button>
  </div>
  <ul id="list"></ul>
  <div class="hint" id="hint">
    仅支持 <b>2.4GHz</b> 网络。扫描时热点会短暂中断属正常现象。
  </div>
</div>

<div class="card">
  <a href="#" onclick="forget();return false" style="color:#c33;font-size:14px">
    忘记 Wi-Fi，恢复热点模式</a>
  <div class="hint">清空已保存的凭证，设备重启后重新进入配网热点。</div>
</div>

<div class="card">
  <div class="hint">
    固件 <span id="fw"></span><span class="tagp" id="mver"></span><br>
    本机唯一编号: <b id="suffix"></b>
  </div>
</div>

</div><script>
var $ = function(id){ return document.getElementById(id); };
var _mdns = '';

function refresh(){
  fetch('/api/status').then(function(r){ return r.json(); }).then(function(s){
    $('mode').innerText = s.sta ? '已连接局域网 (STA)' : '配网热点 (AP)';
    $('ip').innerText   = s.ip || '-';
    $('ssid').innerText = s.ssid || '未连接';
    $('rssi').innerText = (s.sta && s.rssi) ? ('  ' + s.rssi + ' dBm') : '';
    $('mdns').innerText = s.mdns || '-';
    $('fw').innerText   = s.fw;
    $('mver').innerText = s.chip;
    $('suffix').innerText = s.mac;
    if (s.mdns) _mdns = s.mdns;
  }).catch(function(){});
}

function scan(){
  $('btnScan').disabled = true; $('hint').innerText = '扫描中...';
  fetch('/api/scan').then(function(r){ return r.json(); }).then(function(list){
    var ul = $('list'); ul.innerHTML = '';
    if (!list.length) { $('hint').innerText = '没扫到网络，靠近路由器再试。'; }
    list.forEach(function(ap){
      var li = document.createElement('li');
      li.innerHTML = '<span>' + ap.ssid + '</span><span class="rssi">' +
                     ap.rssi + ' dBm' + (ap.lock ? ' [加密]' : '') + '</span>';
      li.onclick = function(){ $('ssid').value = ap.ssid; $('pwd').focus(); };
      ul.appendChild(li);
    });
    $('hint').innerText = list.length ? ('扫到 ' + list.length + ' 个网络，点击即可填入。') :
                                       '没扫到网络。';
  }).catch(function(){ $('hint').innerText = '扫描失败。'; })
   .then(function(){ $('btnScan').disabled = false; });
}

function save(){
  var ssid = $('ssid').value.trim();
  var pwd  = $('pwd').value;
  if (!ssid) { alert('请先填写或选择 Wi-Fi 名称'); return; }

  // 用 POST body(form-urlencoded) 传参，避免部分 WebServer 对 POST+查询串不解析
  // 导致密码变成空字符串（见文件头说明 5）
  var body = 'ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(pwd);
  $('btnSave').disabled = true;

  fetch('/api/wifi', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: body
  }).then(function(r){
    if (!r.ok) throw new Error('bad');
    $('doneSsid').innerText = ssid;
    $('doneUrl').innerText  = _mdns || '设备 mDNS 地址';
    $('done').style.display = 'block';
    $('hint').innerText = '已保存，设备重启中...';
    window._jumped = true;
    // 网络恢复(切回家里的 Wi-Fi)后自动打开设备页面
    window.addEventListener('online', function(){
      if (window._once) return; window._once = true;
      if (_mdns) location.href = _mdns;
    });
  }).catch(function(){
    $('btnSave').disabled = false;
    alert('保存失败，请重试');
  });
}

function forget(){
  if (!confirm('确定恢复热点模式？设备将断开当前局域网。')) return;
  fetch('/api/forgetwifi', {method:'POST'}).then(function(){
    alert('已清除凭证，设备重启后回到配网热点。');
  });
}

refresh();
setInterval(refresh, 3000);
</script></body></html>
)rawliteral";

// ==================== Web 处理 ====================
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  String json = "{";
  json += "\"sta\":"     + String(staMode ? "true" : "false") + ",";
  json += "\"ip\":\""    + currentIP + "\",";
  json += "\"ssid\":\""  + jsonEscape(staMode ? WiFi.SSID() : String("")) + "\",";
  json += "\"rssi\":"    + String(staMode ? WiFi.RSSI() : 0) + ",";
  json += "\"mdns\":\""  + mdnsUrl + "\",";
  json += "\"fw\":\""    + String(FW_NAME) + " " + FW_VERSION + "\",";
  json += "\"mac\":\""   + String(getMacSuffix()) + "\",";
#ifdef ESP8266
  json += "\"chip\":\"ESP8266\"";
#else
  json += "\"chip\":\"" + String(ESP.getChipModel()) + "\"";
#endif
  json += "}";
  server.send(200, "application/json", json);
}

// 保存凭证：写入后重启，由启动逻辑自动连 STA
void handleWiFiConfig() {
  String ssid = server.arg("ssid");
  String pass = server.arg("password");
  ssid.trim();

  if (ssid.length() == 0) {
    server.send(400, "text/plain", "SSID 不能为空");
    return;
  }
  if (ssid.length() > 32 || pass.length() > 64) {
    server.send(400, "text/plain", "SSID 或密码过长");
    return;
  }

  Serial.printf("[配网] 保存凭证 SSID=[%s]\n", ssid.c_str());
  saveWiFi(ssid, pass);

  server.send(200, "application/json", "{\"ok\":true,\"reboot\":true}");
  delay(600);          // 等响应发完
  ESP.restart();
}

void handleForgetWiFi() {
  Serial.println("[配网] 清除凭证, 恢复热点模式");
  clearWiFi();
  server.send(200, "application/json", "{\"ok\":true}");
  delay(600);
  ESP.restart();
}

#if ENABLE_WIFI_SCAN
// 扫描周边网络。注意：扫描会让 AP 信道跳变，正在连着热点的客户端可能短暂掉线，
// 所以只在用户主动点击时执行，不做开机扫描。
void handleScan() {
  int n = WiFi.scanNetworks(false /*同步*/, false /*不显示隐藏*/);
  String json = "[";
  int emitted = 0;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;

    // 加密判断：两套 core 的常量名不同
#ifdef ESP8266
    bool lock = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
#else
    bool lock = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
#endif

    if (emitted > 0) json += ",";
    json += "{\"ssid\":\"" + jsonEscape(s) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
            ",\"lock\":" + String(lock ? "true" : "false") + "}";
    emitted++;
    if (emitted >= 24) break;      // 限制返回条数，避免响应过大
  }
  json += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}
#endif

void setupWebServer() {
  server.on("/",            HTTP_GET,  handleRoot);
  server.on("/api/status",  HTTP_GET,  handleStatus);
  server.on("/api/wifi",    HTTP_POST, handleWiFiConfig);
  server.on("/api/forgetwifi", HTTP_POST, handleForgetWiFi);
#if ENABLE_WIFI_SCAN
  server.on("/api/scan",    HTTP_GET,  handleScan);
#endif
  server.onNotFound([]() {
    server.send(404, "text/plain", "not found");
  });
  server.begin();
  Serial.println("[HTTP] 配置页已启动 (80)");
}

// ==================== 长按恢复配网 ====================
#if ENABLE_FACTORY_RESET_BUTTON
static void handleFactoryReset() {
  static bool     lastState    = true;   // 上拉，未按下为 HIGH
  static bool     stable       = true;
  static unsigned long lastChg = 0;
  static unsigned long pressed = 0;
  static bool     fired        = false;

  bool raw = digitalRead(FACTORY_RESET_PIN);
  if (raw != lastState) { lastChg = millis(); lastState = raw; }
  if (millis() - lastChg < 50) return;   // 50ms 去抖
  if (raw != stable) {
    stable = raw;
    if (stable == LOW) { pressed = millis(); fired = false; }   // 按下
  }
  if (stable == LOW && !fired && (millis() - pressed) >= FACTORY_RESET_HOLD_MS) {
    fired = true;
    Serial.println("[按键] 长按触发: 清除凭证并重启");
    clearWiFi();
    delay(200);
    ESP.restart();
  }
}
#endif

// ==================== setup / loop ====================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("========================================");
  Serial.printf("  %s v%s 启动\n", FW_NAME, FW_VERSION);
  Serial.printf("  芯片: ");
#ifdef ESP8266
  Serial.printf("ESP8266 (chipId=%08X)\n", ESP.getChipId());
  EEPROM.begin(EEPROM_SIZE);        // ESP8266 只需在这里 begin 一次
#else
  Serial.printf("%s rev%d, %d 核\n", ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getChipCores());
#endif
  Serial.printf("  唯一编号: %s\n", getMacSuffix());
  Serial.println("========================================");

#if ENABLE_FACTORY_RESET_BUTTON
  pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);
#endif

  setupNetwork();      // 连 STA，或起 AP 热点
  setupWebServer();

  if (staMode) onNetworkReady();   // 已连上：立即跑业务
}

void loop() {
  server.handleClient();
  ensureWiFi();          // 保活：AP 模式重试连 STA / STA 掉线重连

#if ENABLE_FACTORY_RESET_BUTTON
  handleFactoryReset();
#endif

  // 业务主循环：自己的逻辑写在这里
}
