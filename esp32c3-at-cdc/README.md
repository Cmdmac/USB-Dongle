# esp32c3-at-cdc —— ESP32-C3 上的 AT 固件（AT 走 USB-CDC）

把 ESP32-C3 当「Wi-Fi 猫」用：上位机（PC 或单片机）发 AT 文本命令让它联网、收发 TCP/UDP。  
命令通道走 **USB-CDC**，插上电脑就是一个 COM 口，不需要引出 UART。

```
单文件 Arduino 工程：esp32c3-at-cdc.ino
目标芯片：ESP32-C3
框架：arduino-esp32 core 3.x（已在 3.3.11 上核过 API）
必用 FQBN：esp32:esp32:esp32c3:CDCOnBoot=cdc
```

---

## 通道分配（本工程最关键的设计）

| 用途        | 端口                                         | 说明                       |
| --------- | ------------------------------------------ | ------------------------ |
| **AT 命令** | USB Serial/JTAG（HWCDC）= Arduino 的 `Serial` | 插电脑即用；波特率无意义（USB 不按波特率传） |
| **日志**    | UART0（GPIO20/21, 115200）= `Serial0`        | 两块互不干扰，AT 响应绝不会被日志污染     |

**为什么日志绝不走 AT 口**：AT 是严格的一问一答协议，日志混进去会让上位机解析失败。  
所以本文件里**故意不使用** `log_i()` / `ESP_LOGI()` 等 Arduino 日志宏 ——  
在 `CDCOnBoot=cdc` 下 Arduino 的 `Serial` 就是 AT 口，用那些宏等于把日志打进 AT 流。  
全文件只用自写的 `atLog()`。

**没有 UART0 也能看日志**：这块 dongle 没把 UART0 引出来，光靠 `Serial0` 等于看不见。  
所以 `atLog()` 会同时写进 RAM 里一个 2KB 的环形缓冲，用 `AT+LOG?` 就能通过 AT 通道倒出来。

---

## 编译与烧录

推荐直接用同级的 `esp-build-tool/`（会自动识别本工程并读走 `build.json`）：

```bash
cd ../esp-build-tool && ./start.sh      # 打开 http://127.0.0.1:8787
```

或者手工：

```bash
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc \
                    --output-dir .build .
arduino-cli upload  --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc \
                    -p /dev/cu.usbmodemXXXX --input-dir .build .
```

> ⚠️ **`CDCOnBoot=cdc` 不是可选项。** 少了它，`ARDUINO_USB_CDC_ON_BOOT=0`，  
> Arduino 的 `Serial` 会指向 UART0，AT 命令就跑到那个没引出的口上去了。  
> .ino 里有 `#error` 兜底，编译时会直接报错提示，而不是让你烧进去才发现。

`build.json` 已经写好了这个 FQBN，所以用 build-tool 不用手填。

---

## 上位机怎么发命令（发不出去时看这节）

### 用 esp-build-tool

点「**串口监视**」把端口打开，再用**日志框下面的输入框**发（回车即发，`↑`/`↓` 翻历史）。
输入框只在监视运行时解锁 —— 写通道就是那个监视进程本身。

### 用别的终端

Arduino IDE 串口监视器、SSCOM、PuTTY、`python -m serial.tools.miniterm COM40 115200` 都行。
`COM` 号认 **VID:PID = `303A:1001`**（Espressif USB Serial/JTAG），不是 `2BDF:*` 那类别的设备。

### 三个「发了没反应」的坑

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 屏幕上有回显（你自己打的 `AT` 出现了），但没有 `OK` | **没发换行符**。本固件按**行**解析：`feedAtByte()` 只在收到 `\r` 或 `\n` 时才调 `handleLine()`。裸发 `AT` 两个字节，它一个字都不会回 | 勾上「发送新行」/ 在 Arduino 监视器里选 **New Line** 或 **Both NL & CR** |
| 全程一个字都没有，连 ROM banner 都没有 | 终端把 **DTR/RTS 拉高**，把芯片按在复位 / 下载态（USB Serial/JTAG 的 DTR/RTS 就是复位与 BOOT 线），`loop()` 根本没跑 | 关掉流控、把 DTR/RTS 置低。正常应该先看到 `ESP-ROM:esp32c3-api1-...` 再看到 `ready` |
| 打开就报「拒绝访问 / 端口被占用」 | 同一个 COM 口被另一个程序开着（**Windows 一个口只能一个句柄**） | 关掉别的监视/串口工具。注意：build-tool 的「串口监视」在跑时就会独占该口 |

> 顺便：**波特率无所谓**。AT 走 USB-CDC，USB 不按波特率传，填多少都一样。

---

## 命令清单

全部 22 条。响应统一 `\r\n` 包边，成功 `OK`，失败 `ERROR`。

### 基础

| 命令                                | 作用                           |
| --------------------------------- | ---------------------------- |
| `AT`                              | 测试，回 `OK`                    |
| `AT+RST`                          | 重启                           |
| `AT+GMR`                          | 版本、SDK 版本、编译时间、芯片、MAC        |
| `ATE0` / `ATE1`                   | 关 / 开回显（**默认开启**，和经典调制解调器一致） |
| `AT+RESTORE`                      | 擦掉 NVS 配置并重启                 |
| `AT+HELP`                         | 打印命令表                        |
| `AT+SYSLOG=<0\|1>` / `AT+SYSLOG?` | UART0 那份日志的开关                |
| `AT+LOG?`                         | 把 RAM 日志环从 AT 口倒出来           |

### Wi-Fi

| 命令                                           | 作用                           |
| -------------------------------------------- | ---------------------------- |
| `AT+CWMODE=<1\|2\|3>` / `AT+CWMODE?`         | 1=STA 2=AP 3=AP+STA          |
| `AT+CWJAP="ssid"[,"pwd"]`                    | 连 Wi-Fi（阻塞等待，最多 15s），成功即写入 NVS |
| `AT+CWJAP`                                   | 用上次保存的配置重连一次（不覆盖 NVS）        |
| `AT+CWJAP?`                                  | 查当前 SSID / BSSID / 信道 / RSSI |
| `AT+CWQAP`                                   | 断开（**不清 NVS**，之后还能 `AT+CWJAP` 重连） |
| `AT+CWLAP`                                   | 扫描（⚠️ 扫描期间连接会短暂中断）           |
| `AT+CWSAP="ssid","pwd",ch,ecn` / `AT+CWSAP?` | 配置热点（默认 `USB-Dongle-XXXX`，XXXX = MAC 后两字节） |
| `AT+CIPSTA?`                                 | 查 IP / 网关 / 掩码               |
| `AT+CIPSTA="dhcp"`                           | 用 DHCP                       |
| `AT+CIPSTA="ip","gw","mask"`                 | 静态 IP（会自动断开重连一次）             |
| `AT+CIFSR`                                   | 查 IP / MAC                   |
| `AT+CWHOSTNAME="name"` / `?`                 | 主机名（**必须在连接前设置**才生效）         |
| `AT+CWAUTOCONN=<0\|1>` / `?`                 | 掉线自动重连                       |

`CWJAP` 失败时的 `+CWJAP:<err>`：`1` 超时 / `2` 密码错或认证失败 / `3` 找不到 AP（含"从没保存过 SSID 却执行裸 `AT+CWJAP`"）。

`AT+CWJAP` 有三种形态，别搞混（跟官方 ESP-AT 一致）：

| 写法 | 类型 | 行为 |
| --- | --- | --- |
| `AT+CWJAP="ssid","pwd"` | 设置命令 | 连指定 AP，成功写 NVS |
| `AT+CWJAP?` | 查询命令 | 已连则回 `+CWJAP:"ssid","bssid",ch,rssi`；未连回 `+CWJAP:not connected` + `ERROR` |
| `AT+CWJAP`（裸） | **执行命令** | 用 NVS 里的配置重连一次；没保存过就回 `+CWJAP:3` + `ERROR` |

⚠️ ESP32-C3 的射频**只支持 2.4 GHz**。SSID 是 5 GHz 的会直接落 `+CWJAP:3`（找不到 AP），而 `AT+CWLAP` 里也不会出现它——别以为是密码或固件的问题。

接 WiFi 的标准三步：

```
AT+CWMODE=1                              # 确认是 STA（出厂默认就是 1）
AT+CWLAP                                 # 先看 SSID 在不在、什么加密方式
AT+CWJAP="MyWiFi","mypassword"           # 连；最多阻塞 15s
AT+CIPSTA?                               # 确认拿到 IP（不是 0.0.0.0）
```

连上之后 SSID/密码已进 NVS，下次上电会自动连（`AT+CWAUTOCONN=1` 时掉线也会自动重连）；临时掉线可用 `AT+CWQAP` 断、`AT+CWJAP` 再连。

### TCP / UDP

| 命令                                                      | 作用                         |
| ------------------------------------------------------- | -------------------------- |
| `AT+CIPMUX=<0\|1>` / `?`                                | 单连接 / 多连接（有连接时不许切）         |
| `AT+CIPDINFO=<0\|1>` / `?`                              | `+IPD` 是否附带远端 ip/port      |
| `AT+CIPSTART=[link,]"TCP\|UDP","host",port[,localport]` | 建连接                        |
| `AT+CIPSEND=[link,]<len>`                               | 发数据，回 `>` 后跟 len 个字节       |
| `AT+CIPCLOSE[=link]`                                    | 关连接（不带参数：单连接关那一个，多连接全关）    |
| `AT+CIPSERVER=<0\|1>[,port]`                            | 开/关 TCP 服务器（要求 `CIPMUX=1`） |
| `AT+CIPSTATUS`                                          | 连接状态                       |

---

## 数据上报

收到 socket 数据时主动上报：

```
+IPD,<link>,<len>:<原始字节>
```

`AT+CIPDINFO=1` 时带上远端信息：

```
+IPD,<link>,<len>,"192.168.1.5",54321:<原始字节>
```

单连接模式（`CIPMUX=0`）不输出 `<link>`。

> ⚠️ **没有 USB 主机连接时，发往 AT 口的数据会被丢弃。** 这是 HWCDC 的 FIFO 策略  
> （源码里 `!isCDC_Connected()` 分支直接走 flush），好处是拔了 USB 不会卡住 loop，  
> 代价是这段时间的 `+IPD` 上报会丢。要可靠收数据，保持串口打开。

---

## 一次典型会话

```
AT
OK
AT+CWMODE=1
OK
AT+CWJAP="MyWiFi","mypassword"
WIFI CONNECTED
WIFI GOT IP
OK
AT+CIFSR
+CIFSR:STAIP,"192.168.1.42"
+CIFSR:STAMAC,"AC:67:B2:11:22:33"
OK
AT+CIPSTART="TCP","192.168.1.100",9000
CONNECT
OK
AT+CIPSEND=5
OK
> hello
SEND OK
+IPD,5:world
AT+CIPCLOSE
CLOSED
OK
```

多连接（`CIPMUX=1`）时每条都带 link id：

```
AT+CIPMUX=1
OK
AT+CIPSERVER=1,8080
OK
0,CONNECT                 ← 有客户端连进来
+IPD,0,6:ping
AT+CIPSEND=0,4
OK
> pong
SEND OK
```

---

## 9 条已核实的平台事实（避免下次再踩）

写这份固件时照着 core 3.3.11 的源码逐条核对过，不是猜的：

1. `platform.txt` 里 esp32c3 的额外宏是  
   `-DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT={build.cdc_on_boot}`，  
   所以 `CDCOnBoot=cdc` 一定让 `Serial` 指向 `HWCDC`。
2. core 3.x 里 `WiFiServer` / `WiFiClient` 只是 `NetworkServer` / `NetworkClient` 的 **typedef**，  
   `available()` 已标 `deprecated`，应该用 `accept()`。
3. `NetworkServer::begin()` 会给 listening socket 设 `O_NONBLOCK`，  
   所以 `hasClient()` / `accept()` 都不会阻塞 `loop()`。
4. `NetworkClient` 的 fd 由 `shared_ptr` 持有、析构函数是**空的**，  
   所以 `link.tcp = server.accept()` 这种按值复制是安全的（不会误关 fd）。
5. 没有主机连接时 `HWCDC::write()` 直接走 FIFO 丢弃策略、**不阻塞**，拔了 USB 不会卡住 loop。
6. `HWCDC::read()` 内部是 `xQueueReceive(rx_queue, &c, 0)`，超时为 0 → **不阻塞**。  
   但 `read(buf, size)` 是 `Stream::readBytes` 语义（会等到凑满或超时），所以本工程逐字节读。
7. core 3.x 的 `wl_status_t` 里**没有** `WL_WRONG_PASSWORD`，  
   密码错只能落在 `WL_CONNECT_FAILED`，故 `+CWJAP:2` 是推断出来的。
8. `NetworkServer` 在 core 3.x 里没有 `begin(port)` 之外的独立 accept 超时，  
   连接池满时只能 `stop()` 掉新连接（本工程就是这么做的）。
9. **.ino 结构坑（2026-09-30 实测修复）**：Arduino 的 .ino 预处理会把所有函数的  
   自动原型插到文件头部；**凡在函数签名里出现的自定义类型（如 `struct Args`、  
   `struct Link`）必须定义在【第一个函数定义之前】**，否则报 `'Args' has not
   been declared` / `'Link' does not name a type`。官方 Arduino IDE 的流程同样如此，  
   会强制在文件末尾追加「关键函数在前、类型在前」的搬运建议。

---

## 已知边界与没做的部分

| 项                              | 说明                                                                   |
| ------------------------------ | -------------------------------------------------------------------- |
| **不支持 `AT+PING`**              | Arduino 核没暴露 ICMP socket API，要做得引第三方 `ESP32Ping` 或用 IDF 的 `esp_ping` |
| **不支持 `AT+CIPSENDEX`**         | 只支持定长 `CIPSEND`，不支持 `\0` 结尾的变长模式                                     |
| **不支持透传模式**                    | `AT+CIPMODE=1`（`+++` 退出）没实现，收数据一律走 `+IPD`                            |
| **不支持 HTTPS / SSL**            | 没接 `NetworkClientSecure`                                             |
| **UDP 远端只解析一次**                | `CIPSTART` 时用 `WiFi.hostByName()` 解析域名并固定下来；域名 IP 变了需重连              |
| **单连接模式的 link id**             | `CIPMUX=0` 时忽略你给的 link id，内部只用唯一在用的那个                                |
| **扫描与连接互斥**                    | `CWLAP` 是阻塞扫描，会让当前连接短暂中断（官方 ESP-AT 同样如此）                             |
| **`CWJAP` / `CWLAP` 期间不响应 AT** | 同步阻塞实现。期间上位机发的命令会先攒在 CDC 的 1024 字节 RX 环里                             |

---

## 与同目录其它工程的关系

| 工程                     | 定位                                        |
| ---------------------- | ----------------------------------------- |
| `esp32c3-wifi-serial/` | ESP-IDF：把电脑的 COM 口**透明**地变成局域网服务（无协议）     |
| `esp-wifi-provision/`  | Arduino：Wi-Fi 配网模板（HTTP 表单）               |
| **`esp32c3-at-cdc/`**  | Arduino：把 dongle 变成**受 AT 命令控制**的 Wi-Fi 猫 |
| `esp-build-tool/`      | 上面三者的网页版编译/刷写工具                           |
