# USB-Dongle

一个 **ESP32-C3 USB Dongle**（U 盘形，插电脑）的固件与配套工具集合。

核心思路：ESP32-C3 自带一个 USB Serial/JTAG 控制器，插上电脑就是一个 COM 口。
这个仓库里的几个工程，分别把这个 COM 口做成不同形态的东西。

---

## 工程一览

| 目录 | 框架 | 这是什么 |
| --- | --- | --- |
| [`esp32c3-wifi-serial/`](esp32c3-wifi-serial/) | Arduino | **无线串口桥**：把电脑的一个 COM 口透明地变成局域网服务（TCP / Telnet / WebSocket / MQTT 都能接进来），另带网页终端、WOL 唤醒、双分区 OTA（原 ESP-IDF 版已就地重构为 Arduino 单文件） |
| [`esp32c3-at-cdc/`](esp32c3-at-cdc/) | Arduino | **AT 固件**：把 dongle 变成受 AT 命令控制的「Wi-Fi 猫」，PC 或单片机发文本命令就能联网、收发 TCP/UDP |
| [`esp-wifi-provision/`](esp-wifi-provision/) | Arduino | **Wi-Fi 配网模板**：手机连上热点、填表单换 Wi-Fi，兼容 ESP32 系列与 ESP8266/ESP8285 |
| [`esp-build-tool/`](esp-build-tool/) | Node.js（零依赖） | **网页版编译/刷写工具**：自动扫描同级目录下的工程，一键切换、编译、烧录、串口监视 |

四个工程放在同一层，`esp-build-tool` 会自己把它们都扫出来。

---

## 快速开始

```bash
cd esp-build-tool
./start.sh          # macOS / Linux
start.bat           # Windows
```

浏览器打开 <http://127.0.0.1:8787>，下拉框里选工程，点「编译」或「编译并烧录」。

工具会自己探测工具链（`arduino-cli`、ESP-IDF），也会自动读每个工程的 `build.json` 拿 FQBN / target。

---

## 原理图

[`hardware/USB-Dongle-C3-schematic-V1.0.pdf`](hardware/USB-Dongle-C3-schematic-V1.0.pdf)

嘉立创 EDA 导出，A4 单页，**V1.0**（更新于 2026-10-06）。

与固件对得上的几处：主控 **ESP32-C3FN4**、40 MHz 晶振、USB `D+`/`D−` **直连主控内置的 USB Serial/JTAG**（板上没有外部 USB-PHY，所以也解释了为什么做不成 RNDIS 网卡）、5 V 经 LDO 转 3.3 V、`EN`/`BOOT` 各带 10 kΩ 上拉。

---

## 硬件能力边界（选这块芯片前先看这里）

ESP32-C3 的 USB 是 **USB Serial/JTAG 控制器（固定功能的 CDC-ACM + JTAG）**，**不是 USB OTG**：

- 端点与描述符硬件固定 → **不能**枚举成 RNDIS / CDC-ECM 网卡、HID、MSC
- 所以 **C3 做不了真·USB 无线网卡**、模拟键鼠、U 盘，只能是「虚拟串口」
- 想要真·USB 网卡（`tusb_rndis` + `esp_netif_napt_enable()`），得换 **ESP32-S3 / S2 / P4**

另一个实际约束：这块 dongle 只引出 USB，**没有引出 UART0**。
所以各工程都把 UART0 让给日志、USB 让给业务 —— 而且为了在没引出 UART0 的情况下也能排查问题，
`esp32c3-at-cdc` 额外做了 `AT+LOG?`，可以把 RAM 日志环从 USB 口倒出来。

---

## 三个工程怎么选

| 你的场景 | 用哪个 |
| --- | --- |
| 老上位机软件只会开 COM 口，但设备在局域网另一头 | `esp32c3-wifi-serial`（透明桥，软件侧零改动） |
| 上位机（PC 或单片机）想**主动**控制联网，每次连不同目标 | `esp32c3-at-cdc`（AT 命令，对标官方 ESP-AT） |
| 设备第一次上电、要现场填 Wi-Fi 密码 | `esp-wifi-provision`（网页配网） |

`esp32c3-wifi-serial` 与 `esp32c3-at-cdc` 是**两种互斥的产品形态**：前者无协议（透明），后者有协议（AT）。
不要同时烧，它们是两种模式，不是能叠加的功能。

---

## License

[Apache License 2.0](LICENSE)
