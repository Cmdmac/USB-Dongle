# esp32c3-ble-scan —— BLE 扫描 + GATT Client AT 固件（USB-CDC + 局域网双通道，网页上位机）

把 ESP32-C3 当「BLE 调试猫」用：上位机（PC / 单片机 / 浏览器）发 AT 文本命令让它扫描 BLE 广播、
连接指定设备、发现 service/characteristic、读写 value、订阅 notify。
所有事件通过 `+BLESCAN:...` / `+BLECONN:...` / `+BLEGATTC_...` URC 主动推给上位机。
**配套网页上位机**（`index.html`，零依赖，始终在上位机本地），同页支持两种 transport：
插电脑 USB 时用 **Web Serial API** 直连；dongle 独立供电在局域网内时用 **WebSocket** 连设备 81 端口。
默认体验对标 nRF Connect：广播包结构化解析、广播类型识别（iBeacon / Eddystone）、RSSI 折线图、
过滤栏、设备详情面板里的 GATT 树。

```
        上位机本地网页 (index.html)                     ESP32-C3 BLE 协议栈 ◄──► 周围 BLE 设备
        ├─ 连接方式：USB 直连 / 局域网 WebSocket         ↑
        │   · USB：Web Serial API，插电脑                │ Wi-Fi STA / 首次配网开 SoftAP
        │   · WS ：填 dongle 地址 → ws://<host>:81/ws     │ HTTP :80 配网/控制页 + /api/*
        ├─ 发 AT 指令                                    │ WebSocket :81 ↔ AT 解析器
        ├─ 实时表格 + RSSI 折线 + 广播包 AD 解析        │ mDNS: blescan-XXXX.local
        └─ 设备详情面板：GATT Service/Characteristic 树 └─ MultiStream: AT 输出 fanout → USB-CDC + 所有 WS 客户端
```

> **架构要点**：业务页 `index.html` 始终在上位机本地（双击即用），dongle 固件只内嵌**配网页**
> 和**控制页**（联网后改网络用）+ WS 数据通道。改业务页不用重编固件。
>
> 两条通道并存且互通：USB-CDC 和 WebSocket 共享同一个 AT 解析器（输入）和 MultiStream（输出），
> 任何一条通道发的命令、任何一条通道收到的 URC，两边都看得到。

## 通道分配

| 通道 | 载体 | 用途 |
| --- | --- | --- |
| **AT 命令** | USB Serial/JTAG（HWCDC）= Arduino 的 `Serial` | 插电脑即用；波特率无意义 |
| **AT 命令（局域网）** | WebSocket `ws://<dongle>:81/ws` | dongle 独立供电，上位机填 mDNS 名或 IP 直连 |
| **配网页 / 控制页** | HTTP `http://<dongle>:80` | 未联网时是配网页；联网后是控制页（忘记 Wi-Fi / 重启） |
| **日志** | UART0（GPIO20/21, 115200）= `Serial0` | `atLog()` 输出 + RAM 日志环（`AT+LOG?` 倒出） |

`CDCOnBoot=cdc` 下 Arduino 的 `Serial` 就是 AT 口，因此本文件**不使用** `log_i()` / `ESP_LOGI()` 等
Arduino 日志宏，只用自写的 `atLog()`，避免日志混进 AT 流。

## 编译与烧录

推荐直接用同级的 `esp-build-tool/`（会自动识别本工程并读走 `build.json`）：

```bash
cd ../esp-build-tool && ./start.sh      # 打开 http://127.0.0.1:8787
```

或手工：

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app --output-dir .build .
arduino-cli upload  --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=huge_app \
                    -p /dev/cu.usbmodemXXXX --input-dir .build .
```

> ⚠️ `CDCOnBoot=cdc` 不是可选项，少了它 `Serial` 会指向 UART0（没引出），
> AT 命令就跑丢了。.ino 里有 `#error` 兜底，编译时会直接报错。
>
> ⚠️ `PartitionScheme=huge_app` 是必选项：Wi-Fi + HTTP + WS + 配网页后固件约 1.39MB，
> 默认 1.25MB 分区装不下；`huge_app` 给 3MB app 区，实测占比约 44%。
>
> 依赖：
> - `BLEDevice.h` / `WiFi.h` / `WebServer.h` / `ESPmDNS.h` —— arduino-esp32 core 自带
> - **`WebSockets` 库（2.7+）** —— 需额外安装：`arduino-cli lib install WebSockets`

## 上位机用法

双击 [`index.html`](index.html) 在 Chrome / Edge / Opera 桌面版（89+）打开。
页面顶部「连接方式」下拉**手动选 transport**：

- **USB 直连 (Web Serial)** —— 插电脑，点「连接设备」弹端口选择框，认 `303A:1001`
- **局域网 (WebSocket)** —— dongle 独立供电；填设备地址（`blescan-XXXX.local` 或 IP），点「连接设备」连 `ws://<host>:81/ws`

### dongle 联网（仅首次需要）

dongle 首次上电无 Wi-Fi 凭证 → 自动开 SoftAP `BLE-Scan-XXXX`（密码 `12345678`）：

1. 手机/电脑连这个热点
2. 浏览器开 `http://192.168.4.1` → 固件下发的配网页 → 填家里 Wi-Fi 名/密码 → 设备重启连 STA
3. 连上后 mDNS 注册为 `blescan-XXXX.local`（XXXX = MAC 后 4 位），此后 `http://<ip>/` 是**设备控制页**
4. 之后上位机和 dongle 在同一局域网即可，浏览器打开本地 `index.html` 选 WebSocket 填地址即可用

> **连上 Wi-Fi 后热点就关了**（走的是纯 STA）。换路由器、改密码、把设备挪到别处时，用下面任一方式
> 清凭证回热点，否则只能插 USB：
>
> - 上位机（WS 已连接）点连接栏里的 **「回退热点」** —— 走 WS 发 `AT+WIFI=0`，清凭证 + 重启
> - 浏览器打开 `http://<ip>/` → 控制页 → **「忘记 Wi-Fi，回热点」**
> - 命令行：`curl -X POST http://<ip>/api/forgetwifi`（还有 `/api/reboot`）
> - 插 USB：串口发 `AT+WIFI=0`
>
> 也可用 AT 命令配网：`AT+WIFI=<ssid>,<pass>` 保存重启；`AT+WIFI=0` 清凭证回 AP；
> `AT+WIFI?` 查询状态（`+WIFI:<sta>,<ip>,<ssid>,<mdns_url>`）。
>
> AP 模式下也能用上位机：上位机连 `BLE-Scan-XXXX` 热点后，设备地址填 `192.168.4.1` 即可。

页面上的标准流程（两种 transport 完全一致）：

1. **连接设备** —— USB 模式弹端口选择框认 `303A:1001`；WS 模式连 `ws://<host>:81/ws`
2. **初始化 BLE** —— 点 `AT+BLEINIT=1`
3. **开始扫描** —— 填时长（默认 10s，0 表示持续），点按钮
4. 扫描期间每收到一条广播，下方表格追加/更新一行（按 MAC 去重，保留最新 RSSI 与最后一次见到的字段）
5. 定时扫描结束，设备自动发 `+BLESCAN:done`，网页自动识别
6. **点行展开详情**：列表在左、详情在右（窄屏 <1240px 自动改为上下排列；右栏粘在视口内，
   列表很长时不用滚到底看）。详情含广播包 AD Structure 解析、Service UUID 命名、
   厂商数据 Company ID 解析、原始 hex、RSSI 折线图、过滤栏（按名称/MAC/RSSI/仅有名称/含 UUID）
7. **广播类型识别**：列表多一列「广播类型」，自动标出 **iBeacon** 与 **Eddystone**
   （UID / URL / TLM / EID），详情里展开解出的字段 ——
   iBeacon 的 UUID/Major/Minor/校准功率，Eddystone-UID 的 Namespace/Instance、
   URL 的完整链接（含压缩后缀还原）、TLM 的电压/温度/广播计数/运行秒数、EID 的临时 ID
7. **GATT Client**：详情面板底部点「连接此设备」→自动发现 service/characteristic/descriptor
   → 每个 characteristic 行带 Read / Write / WriteNoRsp / Subscribe 按钮，
   Notify 上来时实时显示在底部日志区，并把最新值回填到对应 chr 行

> 「发送」复用 Web Serial 已打开的写句柄；浏览器关页面/拔线时会自动释放。

## 命令清单

全部 19 条。响应统一 `\r\n` 包边，成功 `OK`，失败 `ERROR`。

### 基础

| 命令 | 作用 |
| --- | --- |
| `AT` | 测试，回 `OK` |
| `AT+RST` | 重启 |
| `AT+GMR` | 版本 / SDK / 编译时间 / 芯片 |
| `ATE0` / `ATE1` | 关 / 开回显（**默认开启**） |
| `AT+RESTORE` | 擦 NVS 并重启 |
| `AT+HELP` | 打印命令表 |
| `AT+SYSLOG=<0\|1>` / `AT+SYSLOG?` | UART0 日志开关 |
| `AT+LOG?` | 把 RAM 日志环从 AT 口倒出来 |
| `AT+WIFI?` | 查询 Wi-Fi 状态（`+WIFI:<sta>,<ip>,<ssid>,<mdns_url>`） |
| `AT+WIFI=<ssid>,<pass>` | 配网：保存后重启连 STA（`pass` 可空） |
| `AT+WIFI=0` | 清除凭证，重启回 AP 热点 `BLE-Scan-XXXX` |

### BLE 扫描

| 命令 | 作用 |
| --- | --- |
| `AT+BLEINIT?` | 查询 BLE 是否已 init |
| `AT+BLEINIT=<0\|1>` | `0`=deinit `1`=init（扫描前必须 init） |
| `AT+BLESCAN?` | 查询扫描状态（scanning, inited） |
| `AT+BLESCAN=1[,<秒>]` | 开始扫描。`<秒>` 默认 10，**0 = 持续到手动停止**，最长 3600 |
| `AT+BLESCAN=0` | 立即停止扫描，回 `+BLESCAN:stopped` |
| `AT+BLESCANRSL` | 倒出本次扫描本地缓存的去重结果（最多 32 条） |
| `AT+BLESCANPARM?` | 查询扫描参数（active / interval / window） |
| `AT+BLESCANPARM=<a>,<i>,<w>` | 设置：`a`=0/1 主动扫描，`i`=interval，`w`=window（单位 0.625ms，约束 `4≤w≤i≤16384`） |

### GATT Client（单连接）

| 命令 | 作用 |
| --- | --- |
| `AT+BLECONN?` | 查询 GATT 连接状态，回 `+BLECONN:<1\|0>,"<addr>",<addr_type>` 或 `+BLECONN:idle` |
| `AT+BLECONN=1,<mac>[,<t>]` | 连接 `<mac>`，可选 `t`=0 public / 1 random（默认 0）。回 `+BLECONN:connected,<addr>` 或 `+BLECONN:failed` |
| `AT+BLECONN=0` | 断开当前连接 |
| `AT+BLEGATTC_DISC=<s\|c\|d\|all>` | 发现 service / characteristic / descriptor / 全部。逐条 URC 上报，最后 `+BLEGATTC_DISC:done` |
| `AT+BLEGATTC_READ=<h>` | 读 value handle 为 `<h>` 的 characteristic，回 `+BLEGATTC_READ:<h>,<hex>` |
| `AT+BLEGATTC_WRITE=<h>,<hex>` | 写 `<hex>`（带响应） |
| `AT+BLEGATTC_WRITENR=<h>,<hex>` | 写 `<hex>`（无响应） |
| `AT+BLEGATTC_SUB=<h>,<1\|0>` | 订阅（1）/ 取消订阅（0）notify（自动写 CCCD `0x2902`） |

> **单连接**：ESP32-C3 默认 `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`，固件全局只有一个 `g_client`。
> 要多连得改 sdkconfig 重编，本工程不做。
> **阻塞调用**：`connect()` 默认 30s 超时；`getServices()/readValue()` 会等 GATT 事务（几十 ms），
> 一条 `AT+BLEGATTC_DISC=all` 期间下一条 AT 会阻塞在 HWCDC rx 队列里。

### 数据上报（URC）

扫描期间每收到一条广播就主动上报：

```
+BLESCAN:<addr_type>,<addr>,<rssi>,<name>,<adv_hex>
```

| 字段 | 说明 |
| --- | --- |
| `addr_type` | `0`=public `1`=random `2`=public-id `3`=random-id |
| `addr` | MAC，大写冒号分隔，如 `AA:BB:CC:DD:EE:FF` |
| `rssi` | 整数 dBm，如 `-67` |
| `name` | 设备名（带引号；没有广播名时为空串） |
| `adv_hex` | 广播数据十六进制（带引号；可空） |

设了时长的扫描结束时：

```
+BLESCAN:done
```

`AT+BLESCAN=0` 手动停止时：

```
+BLESCAN:stopped
```

GATT 连接状态变化：

```
+BLECONN:connected,<addr>      连接成功
+BLECONN:disconnected,<addr>   断开（含对端主动断）
+BLECONN:failed                connect 返回 false（超时 / 拒绝）
```

发现结果（按 `AT+BLEGATTC_DISC` 类型逐条上报，最后发 done）：

```
+BLEGATTC_SVC:<start_h>,<uuid>                  service（arduino-esp32 3.x 不暴露 end_h）
+BLEGATTC_CHR:<value_h>,<props>,<uuid>          characteristic（value handle 即可读写/订阅）
+BLEGATTC_DESC:<handle>,<uuid>                  descriptor
+BLEGATTC_DISC:done
```

| `props` 位 | 含义 |
| --- | --- |
| `0x02` | READ |
| `0x04` | WRITE_NO_RESPONSE |
| `0x08` | WRITE |
| `0x10` | NOTIFY |
| `0x20` | INDICATE |

读 / Notify 主动上报：

```
+BLEGATTC_READ:<handle>,<hex>     AT+BLEGATTC_READ 的回包
+BLEGATTC_NOTIF:<handle>,<hex>    notify
+BLEGATTC_IND:<handle>,<hex>      indicate
```

## HTTP / WebSocket 接口（局域网通道）

dongle 联网后（AP 模式 IP `192.168.4.1`；STA 模式用 mDNS `blescan-XXXX.local` 或路由器分配的 IP），
在 80 / 81 两个端口提供服务。所有接口均无鉴权（局域网内直连，不要暴露到公网）。

### HTTP `:80`

| 方法 | 路径 | 作用 | 返回 |
| --- | --- | --- | --- |
| `GET`  | `/` | **根路径**：AP 模式下发配网页 HTML；STA 模式返回精简状态 JSON | `text/html` 或 `application/json` |
| `GET`  | `/api/status` | 完整状态（含固件版本） | `application/json` |
| `POST` | `/api/wifi` | 提交 Wi-Fi 凭证，保存后重启连 STA。表单字段：`ssid`（≤32）、`password`（≤64） | `{"ok":true}` 或 `400 bad ssid/pass` |
| `POST` | `/api/forgetwifi` | 清除已存凭证，重启回 AP 热点 | `{"ok":true}` |
| `POST` | `/api/reboot` | 重启设备 | `{"ok":true,"reboot":true}` |
| *      | *    | 其他路径 | `404 not found` |

> HTTP 只管**设备管理**（配网 / 状态 / 重启），不涉及 BLE 业务。
> BLE 扫描 / GATT 等业务走 **WebSocket :81** 发 AT 命令、收 URC 流（见下节）。

`/api/status` 返回示例（STA 已联网）：

```json
{
  "sta": true,
  "ip": "192.168.1.42",
  "ssid": "MyHome",
  "mdns": "blescan-a1b2.local",
  "fw": "ble-scan 1.0"
}
```

`/api/status` 返回示例（AP 配网中）：

```json
{
  "sta": false,
  "ip": "192.168.4.1",
  "ssid": "",
  "mdns": "",
  "fw": "ble-scan 1.0"
}
```

> `POST /api/wifi` 和 `POST /api/forgetwifi` 都会在返回后约 600ms `ESP.restart()`，
> 客户端拿到响应后不必等连接关闭，直接重连即可。

### WebSocket `:81` —— 业务数据通道

WS 是**双向字节流**，等同于把 USB-CDC 的 AT 通道搬到网络上。客户端发 AT 命令（带 `\r\n`）、
收 AT 输出（`OK` / `ERROR` / `+BLESCAN:...` / `+BLEGATTC_*` 等 URC），和 USB 直连完全等价。

**典型业务流程**（客户端视角）：

```
1. （可选）GET /api/status        → 确认设备在线 + 拿 mDNS/IP
2. WebSocket 连 ws://<host>:81/
3. 发 "AT+BLEINIT=1\r\n"          ← 收 "OK\r\n"
4. 发 "AT+BLESCAN=1,10\r\n"       ← 收 "OK\r\n"
5. 扫描期间持续收：
   "+BLESCAN:0,AA:BB:CC:DD:EE:FF,-67,\"MyDev\",\"020106...\"\r\n"
   "+BLESCAN:1,11:22:33:44:55:66,-78,\"\"\r\n"
   ...
   "+BLESCAN:done\r\n"            ← 10 秒到期
6. 客户端按 \n 切行、解析 +BLESCAN: 前缀，按 MAC 去重展示
```

> 之所以不把 BLE 业务做成 HTTP REST（如 `GET /api/scan`），是因为扫描结果是
> **持续推送的 URC 流**（每几百毫秒一条广播），HTTP 请求-响应模式不适合。
> WS 字节流 + AT 命令体系正好对齐 USB-CDC 的体验，客户端解析逻辑可复用。

| 项 | 说明 |
| --- | --- |
| URL | `ws://<dongle>:81/`（路径任意，库不校验） |
| 子协议 | 无 |
| 帧类型 | **文本帧 / 二进制帧均可**，按字节原样喂 AT 解析器 |
| 方向：C→S | 上位机发的 AT 命令字节（含 `\r\n`），等同于往 USB-CDC 写 |
| 方向：S→C | 固件 `MultiStream` 把所有 AT 输出（`OK` / `ERROR` / `+BLESCAN:...` / `+BLEGATTC_*` 等）`broadcastBIN` 给**所有**已连客户端 |
| 多客户端 | 支持；每个客户端都能看到完整 AT 流（包括别的客户端发的命令产生的响应） |
| 连接/断开 | 仅在 UART0 日志记一行 `[ws] client #N connected/gone`，不影响 AT 流 |

> 多客户端场景：可同时开多个浏览器页面 / 用 `wscat` 抓流，都能看到同一份 AT 输出。
> 命令去重 / 冲突由用户自己把握，固件不限制谁能在哪条通道发命令。

`wscat` 抓流示例：

```bash
npx wscat -c ws://blescan-a1b2.local:81
> AT+BLEINIT=1
< OK
< +BLESCAN:0,AA:BB:CC:DD:EE:FF,-67,"MyBLEDevice","0201060AFF..."
```

## 一次典型会话

```
AT
OK
AT+BLEINIT=1
OK
AT+BLESCAN=1,10
OK

+BLESCAN:0,AA:BB:CC:DD:EE:FF,-67,"MyBLEDevice","0201060AFF..."
+BLESCAN:1,11:22:33:44:55:66,-78,""
+BLESCAN:0,AA:BB:CC:DD:EE:FF,-65,"MyBLEDevice","0201060AFF..."
+BLESCAN:done

AT+BLESCANRSL
+BLESCANRSL:0,AA:BB:CC:DD:EE:FF,-65,"MyBLEDevice","0201060AFF..."
+BLESCANRSL:1,11:22:33:44:55:66,-78,""
OK
```

连接 + GATT 发现 + 读 + 订阅：

```
AT+BLECONN=1,AA:BB:CC:DD:EE:FF,0
OK

+BLECONN:connected,AA:BB:CC:DD:EE:FF
AT+BLEGATTC_DISC=all
OK
+BLEGATTC_SVC:1,0x1800
+BLEGATTC_SVC:8,0x1801
+BLEGATTC_CHR:3,10,0x2a00         ← READ/WRITE，Device Name
+BLEGATTC_CHR:5,2,0x2a04          ← READ，PPCP
+BLEGATTC_DESC:6,0x2902
+BLEGATTC_CHR:8,34,0x2a05         ← READ/INDICATE，Service Changed
+BLEGATTC_DISC:done

AT+BLEGATTC_READ=3
+BLEGATTC_READ:3,4d79-446576696365
OK

AT+BLEGATTC_SUB=8,1
OK
+BLEGATTC_NOTIF:8,00              ← notify 实时上报
+BLEGATTC_NOTIF:8,01

AT+BLECONN=0
+BLECONN:disconnected,AA:BB:CC:DD:EE:FF
OK
```

## 设计要点

- **不去重 URC**：BLE 广播本来就是反复发的，固件每见到一条就上报一次，上位机自行决定怎么展示。
  网页默认按 MAC 去重、保留最新 RSSI 和「最后一次见到的字段」（同 nRF Connect 的展示风格）。
  `g_results[]` 是固件侧兜底的「最后一次见到时的快照」，供 `AT+BLESCANRSL` 倒出。
- **时长到期检测**：BLE 栈在 `start(duration, false)` 后会自动 stop，固件只在 `loop()` 里
  用 `millis()` 兜底判断到期，并给上位机补一个 `+BLESCAN:done` URC。
- **参数化扫描**：`AT+BLESCANPARM` 暴露 `active / interval / window`，单位 0.625ms（BLE 栈约定），
  默认 `interval=window=160`（100ms 全开窗），扫描比较激进。
- **单连接 GATT**：ESP32-C3 默认 `max_conn=1`，固件全局只有一个 `g_client`，配 nRF Connect 式
  单连调试刚好；新连接前若已连会先发 `AT+BLECONN=0`。`onDisconnect` 回调里清空 `g_connConnected`
  并上报 `+BLECONN:disconnected`，对端主动断也能感知。
- **不用 try/catch**：arduino-esp32 core 默认 `-fno-exceptions`，所以 GATT 调用直接走返回值/abort。
  实测 `connect()` 失败返回 false 而非 throw；`getServices/readValue` 在已连状态下正常返回。
  极少数 throw 场景就让它 abort 重启，符合「AT 猫」的容错预期。
- **网页侧分两层**：第一步扫描体验全部在浏览器解析广播包（零固件改动）；
  第二步 GATT 树/读写订阅基于 URC 增量更新 `gattState.services[]`，
  notify 来一条就追加到底部日志区并回填对应 chr 的最新值。
- **回显与日志**：与 esp32c3-at-cdc 完全一致 —— AT 口跑 AT、UART0 跑日志，互不污染；
  板子没引出 UART0 时用 `AT+LOG?` 倒 RAM 日志环。

## 已知边界

- ESP32-C3 BLE 是 **BLE 5.0**，**只支持 BLE**（不支持经典蓝牙）。
- arduino-esp32 core 3.x 默认用 **NimBLE** 协议栈（不是 Bluedroid），`BLEDevice.h` 是统一封装，
  代码层无需关心；默认 `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`，所以 GATT Client 只能单连。
- 主动扫描（`activeScan=1`）会发 `SCAN_REQ` 索取 scan response，被动扫描只收广播。
  想抓 iBeacon / Eddystone 的完整数据建议保持主动。
- 扫描期间 BLE 协议栈占用 CPU 较多，但 AT 命令解析不受影响（HWCDC 仍是逐字节读、不阻塞）。
- GATT 阻塞调用：`connect()` 30s 超时、`getServices()/readValue()` 等几十 ms；
  一条 `AT+BLEGATTC_DISC=all` 期间下一条 AT 会排在 HWCDC rx 队列里等。
- 没有 try/catch：core 默认 `-fno-exceptions`，远端突然断开时调 `getServices()` 等可能 throw
  std::string → abort 重启（少见，重启后会自动恢复扫描就绪状态）。

## 与同目录其它工程的关系

| 工程 | 定位 |
| --- | --- |
| `esp32c3-at-cdc/` | Arduino：把 dongle 变成受 AT 命令控制的 **Wi-Fi 猫**（联网 / TCP / UDP） |
| **`esp32c3-ble-scan/`** | Arduino：把 dongle 变成受 AT 命令控制的 **BLE 调试猫**（扫描 + GATT Client），配套本地网页上位机 |
| `esp32c3-wifi-serial/` | Arduino：无线串口桥（透明） |
| `esp-wifi-provision/`  | Arduino：Wi-Fi 配网模板（HTTP 表单） |
| `esp-build-tool/`      | Node.js：网页版编译/刷写工具（自动扫到本工程） |
