# esp-build-tool —— 网页版 ESP 固件编译 / 刷写工具

一个零依赖的本地网页工具：自动扫描**同级目录**下的所有 ESP 工程，在浏览器里一键切换、编译、烧录、串口监视。

参考了 [Cmdmac/ESP-Switch](https://github.com/Cmdmac/ESP-Switch) 的 `arduino-cli-web`，但核心差异是：

> **不需要为每个工程改路径或环境变量。** 工具启动时扫描自己上一级目录，所有工程自动出现在下拉框里。

支持两类工程，自动识别：

| 类型 | 判定条件 | 调用工具链 |
| --- | --- | --- |
| Arduino | 目录下有 `.ino` | `arduino-cli` |
| ESP-IDF | 有 `CMakeLists.txt` + `main/`（或 `sdkconfig.defaults`） | `idf.py` |

---

## 目录结构

```
USB-Dongle/                     ← 工作区（自动扫描这一层）
├── esp-build-tool/             ← 本工具
│   ├── server.js               Node 后端（零依赖，仅用内置模块）
│   ├── public/index.html       单页前端
│   ├── net-term.bat            网络串口终端的 Windows 入口（双击/命令行，转发给 tcp-term.py）
│   ├── tools/serial-monitor.py 串口监视（pyserial）
│   ├── tools/tcp-term.py       裸 TCP 终端（零依赖，给 wifi-serial 的 :2333 用）
│   ├── .venv/                  串口监视用的 Python 环境（缺 pyserial 时自动创建，已 gitignore）
│   ├── start.sh                macOS / Linux 启动脚本
│   ├── start.bat               Windows 启动脚本
│   └── README.md
├── esp-wifi-provision/         Arduino 工程 → 自动识别
│   └── esp-wifi-provision.ino
├── esp32c3-at-cdc/             Arduino 工程 → 自动识别（AT 固件，走 USB-CDC）
│   ├── esp32c3-at-cdc.ino
│   ├── build.json              FQBN 写了 CDCOnBoot=cdc
│   └── README.md
└── esp32c3-wifi-serial/        ESP-IDF 工程 → 自动识别
    ├── CMakeLists.txt
    ├── main/
    └── sdkconfig.defaults
```

只要把新工程目录放到工作区同级，点一下网页上的「重新扫描」即可，无需重启。

---

## 快速开始

```bash
cd esp-build-tool

./start.sh        # macOS / Linux
start.bat         # Windows（双击亦可）
```

然后浏览器打开 **http://127.0.0.1:8787**

启动日志会打印探测到的环境与工程：

```
========================================
 esp-build-tool
   地址      http://127.0.0.1:8787
   工程根    /path/to/USB-Dongle
   arduino-cli  /usr/local/bin/arduino-cli  arduino-cli Version: 0.35.3 ...
   ESP-IDF      ~/esp/esp-idf  v5.3.2
   发现工程  3 个: esp-wifi-provision(arduino), esp32c3-at-cdc(arduino), esp32c3-wifi-serial(idf)
========================================
```

---

## 前置依赖

按需安装，工具会自动探测，**不需要配置环境变量**（探测不到时才需要手动指定）。

| 用途 | 依赖 | 说明 |
| --- | --- | --- |
| Arduino 工程 | `arduino-cli` + `esp32:esp32` 核 | 未装 esp32 核时先 `arduino-cli core install esp32:esp32` |
| Arduino 第三方库 | 按工程而定 | **arduino-cli 不会自动装**。本仓库只有 `esp32c3-wifi-serial` 需要：`arduino-cli lib install "WebSockets" "PubSubClient"`。缺了会在编译时被识别并提示（见 FAQ） |
| ESP-IDF 工程 | ESP-IDF（`IDF_PATH` 或 `~/esp/esp-idf`） | 需先 source 过 `export.sh` / `export.bat`，否则 `idf.py` 不在 PATH |
| 串口监视 | Python 3 + `pyserial` | 一般不用管：工具会自动挑一个装了 pyserial 的解释器，实在没有会在 `esp-build-tool/.venv` 里自动装一份（见下节） |
| 擦除 Flash（Arduino 工程） | `esptool` | `pip install esptool`，或已在 esp32 核内 |

Node.js 只需 LTS 版本（≥ 14 即可，用到了内置 `http/fs/path/child_process`）。

---

## 页面功能

| 控件 | 作用 |
| --- | --- |
| 工程下拉 | 切换目标工程，显示类型徽标（arduino / idf）与细节（FQBN / target） |
| 重新扫描 | 重新读取工作区，新加的工程立刻出现 |
| 打开目录 | 在系统文件管理器里打开该工程目录 |
| 串口下拉 | 枚举 `/dev/cu.usbserial*`、`/dev/ttyUSB*`、`/dev/ttyACM*`（Windows 走 arduino-cli） |
| FQBN 输入 | 仅 Arduino 工程显示，可覆盖（如 `esp32:esp32:esp32c6`），按工程记忆 |
| **编译** | 只编译，产物输出到 `<工程>/.build`（Arduino）或 `<工程>/build`（IDF） |
| **编译并烧录** | 编译 + 上传一步到位 |
| **仅烧录已有固件** | 复用上次编译产物直接烧录，省一次编译 |
| **擦除 Flash** | 调 `esptool erase_flash` 清空整片 Flash |
| **串口监视** | 走 pyserial 直读串口，Ctrl+C / 停止按钮退出；同时把该端口登记成可写 |
| **发送**（日志框下方） | 往已打开的串口写数据，回车即发。只在串口监视运行时可用，见「给设备发命令」一节 |
| **停止** | 杀掉整棵进程树（shell → python → 工具链），避免串口被孙子进程占住 |
| **详细日志**（默认勾选） | 编译时打印每一条完整编译命令，见下节 |

编译成功后会列出所有 `.bin` 产物（主 app 优先，bootloader / partition-table 排后），点击即可下载。

页面用 `localStorage` 记住上次选的工程 / 串口 / FQBN，刷新不丢。

---

## 详细日志（默认开启）

编译时想看到完整输出（每条 gcc 命令行、每个编译单元、链接细节），勾选「详细日志」即可。**默认就是勾上的**。

它的实际作用：

| 工程类型 | 传入参数 | 效果 |
| --- | --- | --- |
| Arduino | `arduino-cli compile --verbose` | 打印每条 `xtensa-esp32s3-elf-g++ ...` 完整命令行；默认只打印进度摘要 |
| ESP-IDF | `idf.py -v build` | `idf.py` 会把 `-v` 透传给 ninja（源码 `tools/idf_py_actions/tools.py` 的 `run_target`），打印每条编译命令；默认 ninja 只打 `[n/N]` 进度行 |

实测传参（用桩程序抓的真实 argv）：

```
IDF  build  verbose=1  →  idf.py -v build
IDF  build  verbose=0  →  idf.py build
IDF  flash  verbose=1  →  idf.py -v -p /dev/ttyUSB0 -b 921600 flash
IDF  upload verbose=0  →  idf.py -p /dev/ttyUSB0 -b 921600 app-flash
arduino compile verbose=1 → arduino-cli compile --fqbn ... --verbose <sketch>
```

注意：`-v` 是 `idf.py` 的**全局**选项，必须放在子命令之前，这也是上面 `-v -p ... flash` 的顺序由来。

**擦除 Flash 不跟随此开关**——esptool 的 `-v` 会把整个 stub 的十六进制 dump 出来，噪声太大，得不偿失。

**某个工程想默认关掉**：在该工程 `build.json` 里写 `"verbose": false`。用户手动拨过开关后以页面上的选择为准（记在 `localStorage`）。

---

## 每个工程的可选配置：`build.json`

自动探测不准时，在该工程根目录放一个 `build.json` 覆盖。**没有这个文件也完全可用。**

Arduino 工程：

```json
{
  "type": "arduino",
  "fqbn": "esp32:esp32:esp32c3",
  "buildProps": ["build.defines=-DBOARD_ESP32C3"],
  "baud": 921600,
  "verbose": true
}
```

ESP-IDF 工程：

```json
{
  "type": "idf",
  "target": "esp32c3",
  "flashBaud": 921600,
  "extraArgs": [],
  "verbose": true
}
```

> **编译宏的坑**：ESP32 核走 `build.defines`，ESP8266 核走 `build.extra_flags`。
> 本工具会按 FQBN 前缀自动选对那个 key，不用手工区分。

> **FQBN 的第 4 段是板级选项**，必须允许 `=` 和 `,`，例如
> `esp32:esp32:esp32c3:CDCOnBoot=cdc`。`esp32c3-at-cdc` 就靠它把 Arduino 的
> `Serial` 切到 USB-CDC。校验正则已按这个格式放开（仍排除空格/引号/`$`/反引号/`;`/`&`/`|`/`/`/`\`）。

---

## HTTP 接口

后端全部接口都在这几行里，方便脚本化调用：

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/env` | 工作区路径、工具链探测结果与版本 |
| GET | `/api/projects` | 扫描到的工程列表 |
| GET | `/api/ports` | 串口列表 |
| GET | `/api/firmware?project=X` | 已编译的 `.bin` 产物 |
| GET | `/api/download?project=X&file=Y` | 下载指定产物 |
| GET | `/api/action?project=X&op=build|flash|upload|erase&port=...&fqbn=...&verbose=0` | **SSE** 流式执行（`verbose` 省略即开启） |
| GET | `/api/monitor?port=...&baud=...` | **SSE** 流式串口输出，同时把该端口登记成「可写」 |
| GET | `/api/serial-send?data=...&nl=1` | 往**正在运行的**串口监视写数据（`nl` 省略即补 `\r\n`） |
| GET | `/api/open?project=X` | 在文件管理器打开工程目录 |

`/api/action` 的 SSE 事件：

- `start` —— `{ label, cmd, args }` 真正执行的命令行
- `sys` —— 运行目录等提示
- `out` —— 一行输出
- `firmware` —— 编译成功后的产物列表
- `err` / `done` —— `done` 带着退出码

安全约束：`project` 只按名字在扫描结果里查（杜绝路径穿越），`port` 与 `fqbn` 都走白名单正则校验后再落进命令行。

---

## 环境变量（全部可选）

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `PORT` | `8787` | 监听端口 |
| `HOST` | `127.0.0.1` | 监听地址，只对本机开放 |
| `ESP_WORKSPACE` | 本工具的上一级目录 | 工程扫描根 |
| `ARDUINO_CLI_PATH` | 自动探测 | 指定 arduino-cli 可执行文件 |
| `IDF_PATH` | 自动探测 | 指定 ESP-IDF 目录 |
| `ESPMON_PYTHON` | 自动探测 | 指定跑 `tools/serial-monitor.py` 的解释器，**优先级最高**（需自带 pyserial） |
| `MONITOR_NO_AUTOINSTALL` | 未设置 | 设为 `1` 时，缺 pyserial 只报错、不自动建 `.venv` |

例：临时换个工作区并开在 9000 端口

```bash
ESP_WORKSPACE=/path/to/projects PORT=9000 ./start.sh
```

---

## 串口监视用的是哪个 Python

监视走 `tools/serial-monitor.py`（pyserial 直读，不依赖 TTY），必须由一个**装了 pyserial** 的解释器来跑。

教训：不要用「`PATH` 里第一个 `python3`」了事。一台机器上常有好几个解释器（托管版 / 微软商店版 / conda / 各种 venv），排最前面的那个往往最"素"、没装 pyserial，结果就是一点「串口监视」只回一句 `No module named 'serial'`。

所以 `/api/monitor` 按下面的顺序**逐个真跑一次 `import serial` 验证**，只认能跑通的：

1. `ESPMON_PYTHON` 环境变量（显式指定，最高优先级）
2. `esp-build-tool/.venv`（本工具自带环境，见下）
3. ESP-IDF 自带 `python_env/<x.y_z>_env` —— 只要装过 IDF 就白捡一个可用解释器（`idf_monitor` 依赖 pyserial）
4. `PATH` 里的 `python3` / `python`

探测时会跳过两类坑：`.cmd` / `.bat` 壳脚本（Node 不能直接执行，真正的 `.exe` 就在旁边）、`WindowsApps` 下的「执行别名」存根（跑它只会弹微软商店）。

**一个都找不到时**，工具会在 `esp-build-tool/.venv` 建一个虚拟环境并把 pyserial 装进去，然后接着开始监视——点一次「串口监视」就够，中间步骤的输出在页面上都看得见。只发生一次，之后走上面的快路径。启动横幅会打印最终选中的解释器：

```
   串口监视     D:\work\USB-Dongle\esp-build-tool\.venv\Scripts\python.exe  (pyserial 3.5)
```

`.venv` 已在 `.gitignore` 里，删掉它不影响什么，下次点监视会重建。

---

## 给设备发命令（日志框下面的「发送」框）

用途：AT 固件、或者任何「你发它才回」的设备。点「串口监视」把端口打开后，输入框解锁，回车即发（`↑`/`↓` 翻历史）。不打开监视是发不了的——写通道就是那个监视进程本身。

**为什么必须复用监视进程、不能另开一个连接去写串口**：

1. **一开就复位。** ESP32-C3 的 USB Serial/JTAG（HWCDC）打开瞬间就会复位芯片，因为 DTR/RTS 在这块外设上就是它的复位 / BOOT 线。为「发送」另开连接 = 每发一条命令就把设备重启一次。
2. **Windows 上同一个 COM 口不能被两个句柄同时打开**，直接「拒绝访问」。

所以实现是：`POST` 到 `/api/serial-send` → 服务端把字节写进**监视子进程的 stdin** → `tools/serial-monitor.py` 里一个 `stdin` 线程原样转给串口。读方向不受影响，两个方向共用一个句柄。

**行尾**：`nl=1`（默认）会在末尾补 `\r\n`。这非常重要 —— AT 固件是按**行**解析的，只有收到 `\r` 或 `\n` 才会解析一行；裸发 `AT` 两个字节，它一个字都不会回。`AT+CIPSEND` 提示 `>` 之后要发**裸字节**，那时把「行尾 `\r\n`」取消掉再发。

**超过 4096 字符会被拒**，`data` 需 URL 编码（网页端自动做）。

---

## 网络串口：`tools/tcp-term.py`

上面那套是「**USB 串口**」，只能用在本机插着的设备。换成 `esp32c3-wifi-serial` 固件时，串口是被
**暴露到局域网**的（TCP :2333），本机不再有那个 COM 口可用，于是需要**网络**方向的终端。

Windows 上这件事意外地难：**默认不装 TelnetClient**（`System32\telnet.exe` 不存在），`nc` / `ncat` /
PuTTY 也往往没有，PowerShell 的 `Test-NetConnection` 只能探端口不能交互。所以带了个零依赖脚本
（只用标准库 `socket`，ESP-IDF 自带的 venv python 直接能跑，**不需要 `pip install` 任何东西**）：

```bash
python tools/tcp-term.py 192.168.0.239 2333                       # 交互终端
python tools/tcp-term.py 192.168.0.239:2333 --send AT --wait 2    # 发完就退（脚本化）
echo AT | python tools/tcp-term.py 192.168.0.239 2333 --wait 2    # 管道
python tools/tcp-term.py 192.168.0.239 2333 --no-crlf             # 裸字节，不补行尾
```

**Windows 上更省事的是 `net-term.bat`**：自动挑一个可用的 python（`.venv` → ESP-IDF `python_env/*` → PATH），
把参数原样转给上面的脚本（保留引号，`--send "xx yy"` 不会被拆开），用完 `pause` 不让窗口闪退。
不传参数会提示输入 IP：

```bat
net-term.bat 192.168.0.239 2333                     :: 交互
net-term.bat 192.168.0.239:2333                     :: 端口写在 IP 里也行
net-term.bat 192.168.0.239 2333 --send AT --wait 2  :: 发完就退
net-term.bat                                        :: 提示输入 IP（默认端口 2333）
```

> ⚠️ **`Device IP:` 提示符下按空回车，不会再「静默连错」。** cmd 不区分「给了空字符串」和「没给」，
> 老版本这里是 `set /p` 拿到空串 → `%HOST%` 展开成空 → 端口 `2333` 顶到参数第一位 →
> 脚本收到 `target="2333"`、端口又默认 2333，报错却是 `[tcp] 连不上 2333:2333 —— getaddrinfo failed`，
> 看着像网络问题，其实是参数错位。现在：**空输入会被判为无效并直接给出用法**，
> 而且 `tcp-term.py` 单独用时也会对「纯数字的 host」打出定位提示。
> 退出码：`0` 正常 / `1` 参数或 python 缺失 / `2` 连不上 / `3` 参数非法。

> ⚠️ 这个 `.bat` **刻意全用 ASCII 写**。cmd.exe 是按 **ANSI 代码页**（简体中文下是 936/GBK）
> 读 .bat 文件的，UTF-8 保存的中文注释会被当 GBK 解码，字节流一错位整行就散架 ——
> 实测报错长这样：`'etwork' 不是内部或外部命令`（`Network...` 那行被吃掉一个字节）。
> 中文提示一律交给 python 打印，Windows 控制台走 PEP 528 的宽字符 API，与代码页无关。

默认发送补 `\r\n`（AT 类设备按行解析，不补就是「发了没反应」——和上面那个坑是同一个）；
`--log` 存原始字节、`--idle-exit N` 静默即退。

**它作为客户端从不主动发 IAC 字节**，所以连 `TCP_SERVER` 模式的 2333 是干净的。
反过来说：**别用真 telnet 客户端去连 2333**——固件只在 `NET_TELNET` 模式下过滤 IAC，TCP 模式
会把 telnet 客户端的 `FF FD 01 ...` 协商序列原样写进下游串口（实测 6 字节全进）。要用 telnet
客户端，先把网页配置页的「网络模式」改成 Telnet，固件会改听 23 端口。

---

## 常见问题

**找不到 arduino-cli / idf.py**
探测顺序是：环境变量 → `PATH` → 常见安装位置（`~/.espressif`、`~/esp`、`/opt`）。
IDF 必须先 source 过 `export.sh`（Windows 用 ESP-IDF Command Prompt 启动本脚本），否则 `idf.py` 不在 PATH。

**点「串口监视」报 `No module named 'serial'`**
正常不该出现了：工具会先挑一个装了 pyserial 的解释器，没有就在 `.venv` 里自动装一份。
要是还报，基本等于**自动安装失败**——最常见原因是 pip 源不通（`pip config list` 看 `index-url`，
有些镜像/代理在特定网络下连不上）。手工修：

```bash
# Windows
esp-build-tool/.venv/Scripts/python.exe -m pip install -i https://pypi.org/simple pyserial
# Linux / macOS
esp-build-tool/.venv/bin/python -m pip install -i https://pypi.org/simple pyserial
```

**编译报 `fatal error: XXX.h: No such file or directory`**

这是**缺第三方 Arduino 库**，跟工具本身无关（arduino-cli 不会自动装库）。
本仓库里 `esp32c3-wifi-serial` 需要 `WebSockets` + `PubSubClient`，另外两个工程零依赖。

工具会在失败时识别出具体是哪个头文件缺了，并直接打出能抄的命令，例如：

```
[缺库] 找不到头文件：WebSocketsServer.h
[缺库] 这些都是第三方库，arduino-cli 不会自动装。执行：arduino-cli lib install "WebSockets"
```

映射表在 `server.js` 的 `HEADER_TO_LIB` 里，只预置了常见几个；遇到表外的头文件会改成
提示你先 `arduino-cli lib search <头文件名>` 查库名。

手工修：

```bash
arduino-cli lib update-index
arduino-cli lib install "WebSockets" "PubSubClient"
arduino-cli lib list          # 确认已装
```

库装在 `<Documents>/Arduino/libraries/`（Linux/macOS 是 `~/Arduino/libraries/`），
**换机器或重装系统后最容易漏这一步**。

或者干脆借 ESP-IDF 的现成环境：`ESPMON_PYTHON=~/esp/esp-idf/../python_env/idf5.5_py3.13_env/bin/python`。

**烧录报 `Failed to connect to ESP32-C3`**
工具会把这类输出（即便退出码为 0）判定为失败。检查：串口选对没、板子是否处于下载模式、是否被别的串口助手占着。

**停止后串口仍被占用**
已经处理：停止走的是杀**整棵进程树**（Windows `taskkill /T /F`），不会留下孙进程。

**编译产物在哪**
Arduino 在 `<工程>/.build`，IDF 在 `<工程>/build`，页面上也能直接下载。

**日志太多刷屏 / 页面变卡**
详细日志默认开启，IDF 全量编译确实能到十万行量级。前端已做处理：输出先攒进缓冲区批量写入，同一批内连续同色的文本合并成一个 span，**不做任何截断**，所以不会卡。
真嫌吵就取消勾选「详细日志」，退回只显示 `[n/N]` 进度摘要。日志框右下角可以拖拽拉高（最高 80vh）。

**看不到完整编译命令**
确认「详细日志」是否被取消了勾选。它记在浏览器 `localStorage` 里，换浏览器/清缓存后会回到默认（开启）。

**工程没出现在下拉框里**
只扫工作区**直接子目录**，且会跳过工具自身与隐藏目录。确认工程目录就在工作区同级，然后点「重新扫描」。
