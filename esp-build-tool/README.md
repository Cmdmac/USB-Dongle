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
│   ├── tools/serial-monitor.py 串口监视（pyserial）
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
| ESP-IDF 工程 | ESP-IDF（`IDF_PATH` 或 `~/esp/esp-idf`） | 需先 source 过 `export.sh` / `export.bat`，否则 `idf.py` 不在 PATH |
| 串口监视 | Python 3 + `pyserial` | `pip install pyserial` |
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
| **串口监视** | 走 pyserial 直读串口，Ctrl+C / 停止按钮退出 |
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
| GET | `/api/monitor?port=...&baud=...` | **SSE** 流式串口输出 |
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

例：临时换个工作区并开在 9000 端口

```bash
ESP_WORKSPACE=/path/to/projects PORT=9000 ./start.sh
```

---

## 常见问题

**找不到 arduino-cli / idf.py**
探测顺序是：环境变量 → `PATH` → 常见安装位置（`~/.espressif`、`~/esp`、`/opt`）。
IDF 必须先 source 过 `export.sh`（Windows 用 ESP-IDF Command Prompt 启动本脚本），否则 `idf.py` 不在 PATH。

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
