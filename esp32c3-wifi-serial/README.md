# esp32c3-wifi-serial —— Wi-Fi 串口桥（把一个 COM 口变成局域网服务）

**Arduino 工程**（单 `.ino` + 两个 HTML 头文件），跑在 ESP32-C3 USB dongle 上。
核心思想：**电脑上的一个串口（USB-CDC），被桥接成局域网上的 TCP / Telnet /
网页终端 / MQTT 服务**——任何能连到局域网的设备，都等价于直接插着这根串口线。

> 历史注记：本工程原是 9 模块的 ESP-IDF C 工程（app_cfg / bridge / net_srv /
> web_server 等），已**就地重构为 Arduino 单文件**。行为对齐：通道分配、TCP 多客户端、
> Telnet IAC 过滤、WS 终端、MQTT、Web/OTA、WOL、mDNS、NVS 配置全部保留。

```
电脑 COM 口 (USB-CDC) ◄────► bridge 桥接中枢 ◄────► TCP:2333
                                          ├──► Telnet:23
                                          ├──► 网页终端 (WebSocket /ws)
                                          └──► MQTT <prefix>/cmd|tx
```

> 与同目录 `esp32c3-at-cdc` 是**同一块板子的两个互斥形态**：本工程是数据透传（网络侧
> 主动连进来当串口用），at-cdc 是 AT 命令控制（上位机发文本命令）。两者固件不能同时烧。

## 通道分配（本工程最关键的设计）

| 通道 | 载体 | 用途 |
| --- | --- | --- |
| **数据** | USB Serial/JTAG（USB-CDC），即插电脑出的 COM/ttyACM 口 | 纯透传，日志绝不出现在这里 |
| **日志** | UART0（GPIO21/20，115200） | `wlog()` 输出 + 启动信息 |

Arduino 侧的做法：数据固定走 `Serial`（HWCDC，需 `CDCOnBoot=cdc`），日志走 `Serial0`（UART0）。
全文件不用 `log_i()/Serial.printf()` 打数据流方向，只用自写 `wlog()`（同时写 UART0 与
RAM 日志环，`/api/log` 可倒出）。板子没引出 UART0 也能排查问题。
若 dongle 未引出 UART0，看 `/api/log`（RAM 日志环）即可。

## 默认行为（开箱即用）

| 项 | 默认值 |
| --- | --- |
| 串口侧 | USB-CDC，无外设即用 |
| 网络模式 | TCP Server 多客户端（最多 4），端口 **2333** |
| 首次启动（未配 Wi-Fi） | 回退配网热点 `ESP32C3-Serial-XXXX`，密码 **12345678**，IP 192.168.4.1 |
| 管理地址 | `http://serial.local`（mDNS）或 `http://<设备IP>/` |
| 数据通道/网页密码 | 空 = 不校验（建议配置后设置） |

## 快速开始

```bash
# 前置：安装 Arduino 库（库管理器或 git clone 到 ~/Documents/Arduino/libraries/）
#   WebSockets (scottmang/Links2004, 2.7.x)   —— 网页 WS 终端
#   PubSubClient 2.8                           —— MQTT

# 编译（需 arduino-cli，FQBN 决定 USB-CDC 走数据）
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc .
# 或直接用同仓库 esp-build-tool 网页工具（读 build.json 自动配置）

# 烧录（端口即 USB-CDC 枚举出的 COM/ttyACM 口）
arduino-cli upload -p /dev/cu.usbmodemXXX --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc .

# 首次上电：手机/电脑连热点 ESP32C3-Serial-XXXX（密码 12345678）
#          浏览器打开 http://192.168.4.1 ，在配置页填家里 Wi-Fi 并保存

# 设备重启连上 STA 后，用以下任一方式访问串口（见下节）
```

## 四种数据通道（并存，数据互通）

任一通道写入的数据进串口，串口数据广播给所有通道（TCP 槽 + TCP 客户端 + WS 终端 + MQTT）。

| 方式 | 命令 / 入口 | 说明 |
| --- | --- | --- |
| **TCP** | `nc <设备IP> 2333` | 最通用；socat/上位机软件直连 |
| **Telnet** | `telnet <设备IP> 23` | 带 IAC 协商字节过滤，终端友好 |
| **网页终端** | `http://serial.local` → 终端页 | WebSocket(`/ws`)，浏览器直接打字 |
| **MQTT** | 配置 MQTT 后自动接入 | 订阅 `<prefix>/cmd` 写串口；串口数据发 `<prefix>/tx`（默认前缀 `esp32c3-serial`） |

`net_mode` 可切换：`TCP_SERVER`(默认) / `TCP_CLIENT`(主动连远端 host:port，3s 重试) / `TELNET` / `OFF`(只留网页终端)。

## 用电脑刷固件？—— USB-CDC 本来就能刷（常见疑问）

**能，直接刷。** ESP32-C3 的 USB Serial/JTAG 是 ROM 内置的固定功能外设，**烧录和 CDC 透传
走的就是同一个 USB 口**：

- esptool 通过 USB 控制请求让芯片复位进 ROM bootloader，bootloader 接管 USB 后即可烧录；
  应用固件是否正在占用 CDC 驱动**不影响**。
- 唯一要求：**烧录前关闭占用该 COM 口的程序**（串口监视器 / 终端 / 网页终端连接）。
  主机侧端口被独占打开时，esptool 打不开端口会失败。
- 极端情况（固件把 USB 搞挂）可按住 **BOOT(GPIO9) + 复位** 手动进下载模式再刷。

## Web 管理（REST API）

| 接口 | 方法 | 说明 |
| --- | --- | --- |
| `/` | GET | 管理首页（状态 + 终端 + 配置） |
| `/api/status` | GET | Wi-Fi/网络/桥接统计 |
| `/api/config` | GET/POST | 读 / 写整块配置（写后按需重启服务） |
| `/api/wol` | POST | 发 Wake-on-LAN 魔术包（最多 4 个目标） |
| `/api/reboot` | POST | 重启设备 |
| `/api/log` | GET | RAM 日志环（没接 UART0 也能看日志） |
| `/api/log/clear` | POST | 清空日志环 |
| `/ota` `/ota/upload` | GET/POST | 网页 OTA：上传 .bin 直接升级 |

配置了 `password` 后，数据通道与 API 需鉴权（首页本身不鉴权，便于配网）。

## 主要配置项（网页配置页改，存 NVS/Preferences）

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `wifi_ssid/pass` | 空 | STA 凭证（空 → 配网热点模式） |
| `hostname` | `serial` | mDNS / DHCP 主机名（`serial.local`） |
| `password` | 空 | 数据通道 + 网页管理密码 |
| `net_mode` / `net_port` | TCP_SERVER / 2333 | 见上表 |
| `remote_host:port` | — | TCP 客户端模式的对端 |
| `nl_xlate` | 0 | 1 = 串口→网络时把 LF 补成 CRLF |
| `mqtt_uri` / `mqtt_user/pass` / `mqtt_prefix` | 关 | `host[:port]`（端口缺省 1883）/ 凭证 / 主题前缀 |
| `wol[]` | — | WOL 目标 MAC，最多 4 个 |

## 模块结构（单文件内的分节）

| 分节 | 职责 |
| --- | --- |
| 常量 + `AppCfg`/`TcpSlot` struct | 配置结构 + TCP 槽（struct 全部前置——.ino 原型坑） |
| `wlog` | UART0 + RAM 日志环 |
| `cfgDefaults/cfgLoad/cfgSave` | NVS(Preferences, NS `wser`) 存取 |
| `fanoutToNet` / `bridgeNetRx` / `applyNl` / `telnetFilter` | 桥接中枢 |
| `netStart/netStop/netCliPump` + TCP 槽管理 | TCP Server/Client/Telnet |
| `wsEvent` | WebSocket 终端（broadcastBIN 二进制安全） |
| `wolSend/wolSendAll` | Wake-on-LAN |
| `mqttCb/mqttApply/mqttLoopIfUp` | MQTT（setBufferSize(1024) 防 256B 静默截断） |
| Web handlers + `webSetup` | HTTP + REST + OTA |
| `setup`/`loop` | 启动与主循环（串口→网络→TCP→WS→Web/MQTT） |

## 已知限制

- ESP32-C3 无 USB-OTG：USB 口只能是 CDC 串口，**做不了 USB 网卡 / HID 键鼠 / U 盘**
  （需要这些请换 ESP32-S3/S2/P4）。
- USB 透传口 = 烧录口：烧录前需关闭占用 COM 口的工具（见上文）。
- 桥接通道写满会丢字节（`stats.dropped` 可查），上层协议需自行容错。
- 时序敏感协议不建议走 Wi-Fi 桥（抖动比 IDF 版任务模型更粗）。
- Arduino 版固件比 IDF 版大：默认 default 分区表 app 槽 1.25MB，双 OTA 分区；估计 ~1.2MB、
  占比 ~90%+，若超请改用 `PartitionScheme=min_spiffs`（app 槽 1.875MB）。

## 编译环境

- arduino-esp32 core **3.3.x**（基于 ESP-IDF 5.3）+ `arduino-cli`
- FQBN：`esp32:esp32:esp32c3:CDCOnBoot=cdc`（`build.json` 已写好，esp-build-tool 自动读取）
- 库：`WebSockets`（scottmang/Links2004）、`PubSubClient`
