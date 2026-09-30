# esp32c3-wifi-serial —— Wi-Fi 串口桥（把一个 COM 口变成局域网服务）

ESP-IDF 工程，跑在 ESP32-C3 USB dongle 上。核心思想：**电脑上的一个串口（USB-CDC），
被桥接成局域网上的 TCP / Telnet / 网页终端 / MQTT 服务**——任何能连到局域网的设备，
都等价于直接插着这根串口线。

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
| **日志** | UART0（GPIO21/20，115200） | IDF 日志 + 启动信息 |

`sdkconfig.defaults` 已把控制台固定在 UART0、USB Serial/JTAG 整个让给透传。
若板子没引出 UART0 拿不到日志：改为 `CONFIG_ESP_CONSOLE_NONE=y`（sdkconfig.defaults
内有注释行），或直接看网页管理页的 `/api/log`（RAM 日志环）。

## 默认行为（开箱即用）

| 项 | 默认值 |
| --- | --- |
| 串口侧 | USB-CDC（`SERIAL_SIDE_USB_CDC`），无外设即用 |
| 网络模式 | TCP Server 多客户端，端口 **2333** |
| 首次启动（未配 Wi-Fi） | 只开配网热点 `ESP32C3-Serial-XXXX`，密码 **12345678** |
| 管理地址 | `http://serial.local`（mDNS）或 `http://<设备IP>/` |
| 数据通道/网页密码 | 空 = 不校验（建议配置后设置） |

## 快速开始

```bash
# 1. 编译烧录（在本目录）
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbmodemXXX -b 921600 flash      # 端口即 USB-CDC 枚举出的口

# 2. 首次上电：手机/电脑连热点 ESP32C3-Serial-XXXX（密码 12345678）
#    浏览器打开 http://192.168.4.1 ，在配置页填家里 Wi-Fi 并保存

# 3. 设备重启连上 STA 后，用以下任一方式访问串口（见下节）
```

## 四种数据通道（并存，数据互通）

bridge 允许最多 8 个通道同时在线，任一通道写入的数据进串口，串口数据广播给所有通道。

| 方式 | 命令 / 入口 | 说明 |
| --- | --- | --- |
| **TCP** | `nc <设备IP> 2333` | 最通用；socat/上位机软件直连 |
| **Telnet** | `telnet <设备IP> 23` | 带 IAC 协商字节过滤，终端友好 |
| **网页终端** | `http://serial.local` → 终端页 | WebSocket(`/ws`)，浏览器直接打字 |
| **MQTT** | 配置 MQTT 后自动接入 | 订阅 `<prefix>/cmd` 写串口；串口数据发 `<prefix>/tx`；状态发 `<prefix>/state`（默认前缀 `esp32c3-serial`） |

`net_mode` 可切换：`TCP_SERVER`(默认) / `TCP_CLIENT`(主动连远端 host:port) / `TELNET` / `NONE`(只留网页终端)。

## 用电脑刷固件？—— USB-CDC 本来就能刷（常见疑问）

**能，直接刷。** ESP32-C3 的 USB Serial/JTAG 是 ROM 内置的固定功能外设，**烧录和 CDC 透传
走的就是同一个 USB 口**：

- esptool 通过 USB 控制请求让芯片复位进 ROM bootloader，bootloader 接管 USB 后即可烧录；
  应用固件是否正在占用 CDC 驱动**不影响**。
- 唯一要求：**烧录前关闭占用该 COM 口的程序**（串口监视器 / 终端 / 网页终端连接）。
  主机侧端口被独占打开时，esptool 打不开端口会失败。固件启动日志也会提示这一点。
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

## 主要配置项（网页配置页改，存 NVS）

| 字段 | 默认 | 说明 |
| --- | --- | --- |
| `serial_side` | USB-CDC | USB-CDC / UART0(GPIO21,20) / UART1(自定义引脚) |
| `uart_baud` | 115200 | 仅 UART 模式有效（USB-CDC 波特率无意义） |
| `net_mode` / `net_port` | TCP_SERVER / 2333 | 见上表 |
| `remote_host:port` | — | TCP 客户端模式的对端 |
| `wifi_ssid/pass` | 空 | STA 凭证（空 → 配网热点模式） |
| `ap_en` / `ap_pass` | 关 / 12345678 | 配网热点是否常开 |
| `hostname` | `serial` | mDNS / DHCP 主机名（`serial.local`） |
| `password` | 空 | 数据通道 + 网页管理密码 |
| `nl_xlate` | 0 | 1 = 串口→网络时把 LF 补成 CRLF |
| `mqtt_*` | 关 | URI / 用户名密码 / 主题前缀 |
| `wol[]` | — | WOL 目标（MAC + 可选 IP），最多 4 个 |

## 模块架构

| 模块 | 职责 |
| --- | --- |
| `app_cfg` | 配置结构 + NVS 存取 + 默认值 |
| `log_ring` | 早期日志环形缓存（`/api/log` 可倒出） |
| `serial_port` | 串口侧统一抽象（USB-CDC / UART0 / UART1） |
| `wifi_mgr` | STA 连接/重连 + 可选 APSTA 配网热点 |
| `bridge` | 桥接中枢：独立任务轮询串口，多通道分发/广播 |
| `net_srv` | TCP Server / TCP Client / Telnet 服务 |
| `mqtt_bridge` | 可选 MQTT 通道 |
| `web_server` | HTTP + WebSocket 终端 + OTA + REST API |
| `wol` | Wake-on-LAN |

## 已知限制

- ESP32-C3 无 USB-OTG：USB 口只能是 CDC 串口，**做不了 USB 网卡 / HID 键鼠 / U 盘**
  （需要这些请换 ESP32-S3/S2/P4）。
- USB 透传口 = 烧录口：烧录前需关闭占用 COM 口的工具（见上文）。
- 桥接通道写满会丢字节（`bridge_stats_t.dropped` 可查），上层协议需自行容错。
- 时序敏感协议不建议走 Wi-Fi 桥（抖动不可控）。

## 编译环境

- ESP-IDF v5.x（在 `esp32c3-wifi-serial/` 目录执行 `idf.py`）
- 也可用同仓库 `esp-build-tool` 网页工具编译/烧录（自动识别本工程为 IDF 类型）
