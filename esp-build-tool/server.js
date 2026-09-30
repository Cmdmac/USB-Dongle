#!/usr/bin/env node
'use strict';

/**
 * esp-build-tool —— 本地网页版 ESP 固件编译/刷写工具
 *
 * 参考 Cmdmac/ESP-Switch 的 arduino-cli-web，但核心差异是：
 *   本工具会自动扫描「同级目录」下的所有工程，网页上可随时切换，
 *   不需要为每个工程改路径或环境变量。
 *
 * 零第三方依赖：只用 Node 内置模块（http / fs / path / child_process）。
 * 实时输出用 SSE 推送到浏览器。
 *
 * 支持两类工程（自动识别）：
 *   - Arduino 工程：目录下有 .ino          → 走 arduino-cli
 *   - ESP-IDF 工程：有 CMakeLists.txt + main/ → 走 idf.py
 *
 * 启动：
 *   node server.js            然后浏览器打开 http://127.0.0.1:8787
 * 可选环境变量：
 *   PORT              监听端口，默认 8787
 *   ESP_WORKSPACE     工程根目录，默认取本工具目录的上一级
 *   ARDUINO_CLI_PATH  指定 arduino-cli 可执行文件
 *   IDF_PATH          指定 ESP-IDF 安装目录
 *   ESPMON_PYTHON     指定跑 tools/serial-monitor.py 的解释器（需已装 pyserial）
 *   MONITOR_NO_AUTOINSTALL=1  串口监视缺 pyserial 时不要自动创建 .venv 安装
 *
 * 每个工程可选放一个 build.json 覆盖自动探测结果：
 *   Arduino: { "type":"arduino", "fqbn":"esp32:esp32:esp32c3",
 *              "buildProps":["build.defines=-DBOARD_XXX"], "baud":921600,
 *              "verbose": true }
 *   IDF    : { "type":"idf", "target":"esp32c3", "flashBaud":921600,
 *              "verbose": true }
 *
 * 详细日志（verbose）默认开启：编译时打印完整 gcc/ninja 命令行。
 * 想给某个工程关掉，在其 build.json 里写 "verbose": false。
 */

const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn, execFileSync } = require('child_process');

const PORT = Number(process.env.PORT || 8787);
const HOST = process.env.HOST || '127.0.0.1';
const SELF_DIR = __dirname;
const WORKSPACE = process.env.ESP_WORKSPACE
  ? path.resolve(process.env.ESP_WORKSPACE)
  : path.resolve(SELF_DIR, '..');
const PUBLIC_DIR = path.join(SELF_DIR, 'public');
const MONITOR_SCRIPT = path.join(SELF_DIR, 'tools', 'serial-monitor.py');
// 工具自带的 Python 环境：串口监视缺 pyserial / 擦除缺 esptool 时按需创建一次
const MONITOR_VENV = path.join(SELF_DIR, '.venv');
const MONITOR_AUTO_INSTALL = process.env.MONITOR_NO_AUTOINSTALL !== '1';
const isWin = () => process.platform === 'win32';

// ==================================================================
// 工具链探测
// ==================================================================
function which(cmd) {
  try {
    const out = execFileSync(isWin() ? 'where' : 'which', [cmd], {
      encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'],
    });
    return out.split(/\r?\n/).map(s => s.trim()).filter(Boolean)[0] || null;
  } catch (_) { return null; }
}

function findArduinoCli() {
  if (process.env.ARDUINO_CLI_PATH && fs.existsSync(process.env.ARDUINO_CLI_PATH)) {
    return process.env.ARDUINO_CLI_PATH;
  }
  const p = which('arduino-cli');
  if (p) return p;
  // 常见安装位置兜底
  const cands = isWin()
    ? [path.join(os.homedir(), 'AppData', 'Local', 'Arduino15', 'arduino-cli.exe')]
    : ['/usr/local/bin/arduino-cli', '/opt/homebrew/bin/arduino-cli',
       path.join(os.homedir(), '.local', 'bin', 'arduino-cli')];
  for (const c of cands) { try { if (fs.existsSync(c)) return c; } catch (_) {} }
  return null;
}

// 扫描 <root>/<任意>/esp-idf 这类布局（官方安装器 / eim / 手动 git clone）
function scanIdfFrameworks() {
  const out = [];
  const roots = [
    process.env.IDF_TOOLS_PATH && path.join(process.env.IDF_TOOLS_PATH, 'frameworks'),
    path.join(os.homedir(), '.espressif'),
    path.join(os.homedir(), 'esp'),
    '/opt',
  ].filter(Boolean);
  const marker = isWin() ? 'export.bat' : 'export.sh';
  for (const root of roots) {
    let names;
    try { names = fs.readdirSync(root); } catch (_) { continue; }
    for (const n of names) {
      for (const c of [path.join(root, n), path.join(root, n, 'esp-idf')]) {
        try { if (fs.existsSync(path.join(c, marker))) out.push(c); } catch (_) {}
      }
    }
  }
  return out;
}

function findIdf() {
  const marker = isWin() ? 'export.bat' : 'export.sh';
  const cands = [
    process.env.IDF_PATH,
    path.join(os.homedir(), 'esp', 'esp-idf'),
    path.join(os.homedir(), 'esp-idf'),
    '/opt/esp-idf',
    ...scanIdfFrameworks(),
  ].filter(Boolean);
  for (const c of cands) {
    try { if (fs.existsSync(path.join(c, marker))) return c; } catch (_) {}
  }
  return null;
}

const ARDUINO_CLI = findArduinoCli();
const IDF_DIR = findIdf();

function envInfo() {
  let cliVersion = null;
  if (ARDUINO_CLI) {
    try {
      cliVersion = execFileSync(ARDUINO_CLI, ['version'], { encoding: 'utf8', timeout: 8000 })
        .trim().replace(/\s+/g, ' ');
    } catch (_) {}
  }
  let idfVersion = null;
  if (IDF_DIR) {
    try {
      const v = fs.readFileSync(path.join(IDF_DIR, 'version.txt'), 'utf8').trim();
      idfVersion = v;
    } catch (_) {
      try {
        const cm = fs.readFileSync(path.join(IDF_DIR, 'tools', 'cmake', 'version.cmake'), 'utf8');
        const m = cm.match(/IDF_VERSION_MAJOR\s+(\d+)[\s\S]*?IDF_VERSION_MINOR\s+(\d+)[\s\S]*?IDF_VERSION_PATCH\s+(\d+)/);
        if (m) idfVersion = `v${m[1]}.${m[2]}.${m[3]}`;
      } catch (_) {}
    }
  }
  const mon = findMonitorPython();     // 会真的跑一次 import serial 验证
  return {
    workspace: WORKSPACE,
    arduinoCli: ARDUINO_CLI,
    arduinoCliVersion: cliVersion,
    idfDir: IDF_DIR,
    idfVersion,
    monitorPython: mon ? mon.path : null,
    pyserial: mon ? mon.ver : null,
    monitorVenv: MONITOR_VENV,
    platform: process.platform,
  };
}

// ==================================================================
// 串口监视用的 Python 解释器
// ==================================================================
// 为什么不直接用 PATH 里第一个 python3：
//   一台机器上常有好几个解释器（托管版 / 商店版 / conda / 各种 venv），
//   排最前面的那个往往最"素"、没装 pyserial —— 拿它跑 serial-monitor.py
//   只会得到 "No module named 'serial'"。所以这里按优先级逐个人肉验证：
//   真跑一次 `import serial`，只认能跑通的。
//
// 优先级：ESPMON_PYTHON 环境变量 > 本工具 .venv > ESP-IDF 自带 python_env > PATH
function venvPythonPath() {
  return isWin() ? path.join(MONITOR_VENV, 'Scripts', 'python.exe')
                 : path.join(MONITOR_VENV, 'bin', 'python');
}

// where/which 的全部命中（`which()` 只取第一条，会漏掉后面真正可用的那个）
function whichAll(cmd) {
  const bin = isWin() ? 'where' : 'which';
  const args = isWin() ? [cmd] : ['-a', cmd];
  try {
    return execFileSync(bin, args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] })
      .split(/\r?\n/).map(s => s.trim()).filter(Boolean);
  } catch (_) { return []; }
}

// PATH 里的 python，剔掉两类坑：
//   - .cmd/.bat 壳：Node 的 spawn/execFile 不能直接执行，且真正的 .exe 就在旁边
//   - WindowsApps 下的"执行别名"存根：跑它不是报错就是弹微软商店
function pathPythons() {
  const out = [];
  for (const name of ['python3', 'python']) {
    for (const p of whichAll(name)) {
      if (isWin() && (!/\.exe$/i.test(p) || /[\\/]WindowsApps[\\/]/i.test(p))) continue;
      out.push(p);
    }
  }
  return out;
}

// ESP-IDF 的 python_env/<x.y_z>_env 里必定带 pyserial（idf_monitor 的依赖），
// 所以只要用户装过 IDF，就白捡一个可用的解释器。
function idfVenvPythons() {
  const out = [];
  const roots = [process.env.IDF_TOOLS_PATH, path.join(os.homedir(), '.espressif')].filter(Boolean);
  for (const r of roots) {
    let names;
    try { names = fs.readdirSync(path.join(r, 'python_env')); } catch (_) { continue; }
    for (const n of names) {
      const p = isWin() ? path.join(r, 'python_env', n, 'Scripts', 'python.exe')
                        : path.join(r, 'python_env', n, 'bin', 'python');
      try { if (fs.existsSync(p)) out.push(p); } catch (_) {}
    }
  }
  return out;
}

const PY_PROBE_OPTS = { encoding: 'utf8', timeout: 10000, stdio: ['ignore', 'pipe', 'ignore'] };

// 能 import mod 就返回版本号（没有 __version__ 就返回 'ok'），否则 null
function probeModule(py, mod) {
  try {
    const code = `import ${mod} as m;print(getattr(m,'__version__',''))`;
    return execFileSync(py, ['-c', code], PY_PROBE_OPTS).trim() || 'ok';
  } catch (_) { return null; }
}

// 能当解释器跑起来就返回版本号，否则 null（不看装了哪些包）
function probePython(py) {
  try {
    return execFileSync(py, ['-c', 'import sys;print(sys.version.split()[0])'], PY_PROBE_OPTS).trim() || '?';
  } catch (_) { return null; }
}

/** 解释器候选（按优先级去重）；extra 用来插队 */
function pythonCandidates(extra = []) {
  const cands = [
    process.env.ESPMON_PYTHON,          // 显式指定，最高优先级
    ...extra,
    venvPythonPath(),                   // 本工具 .venv
    ...idfVenvPythons(),                // ESP-IDF 自带（必有 pyserial）
    ...pathPythons(),                   // PATH
  ];
  return [...new Set(cands.filter(Boolean))];
}

/** 找一个「真的 import 得动 mod」的解释器 @returns {{path:string,ver:string}|null} */
function findPythonWith(mod, extra = []) {
  for (const py of pythonCandidates(extra)) {
    if (!fs.existsSync(py) && !/[\\/]/.test(py)) continue;   // 纯命令名交给 PATH 解析
    const ver = probeModule(py, mod);
    if (ver) return { path: py, ver };
  }
  return null;
}

function findMonitorPython() { return findPythonWith('serial'); }

/** 拿一个「能跑起来」的解释器去建 venv（不要求它装了什么包） */
function basePythonForVenv() {
  for (const py of pythonCandidates()) {
    if (py.startsWith(MONITOR_VENV)) continue;              // 别拿 .venv 自己重建 .venv
    if (!fs.existsSync(py) && !/[\\/]/.test(py)) continue;
    const ver = probePython(py);
    if (ver) return { path: py, ver };
  }
  return null;
}

/**
 * 「建 .venv + 装包」的步骤，给 runSteps 用。
 * 串口监视缺 pyserial、擦除缺 esptool 都走这里，共用一个环境。
 * @returns 步骤数组；连一个能建 venv 的解释器都没有时返回 null（调用方负责报错）
 */
function venvInstallSteps(pkgs, why) {
  const base = basePythonForVenv();
  if (!base) return null;
  const vpy = venvPythonPath();
  return [
    { note: `${why}，改用工具自带环境（只装一次）：${MONITOR_VENV}` },
    { note: `已尝试过：${pythonCandidates().join('  |  ') || '(PATH 里没有 python)'}` },
    { note: `建 venv 用的解释器：${base.path}  (python ${base.ver})` },
    { cmd: base.path, args: ['-m', 'venv', MONITOR_VENV], cwd: SELF_DIR, label: '创建 .venv' },
    { note: '安装依赖（若卡在下载：多半是 pip 源不通，见 pip config list / 可加 -i https://pypi.org/simple）' },
    { cmd: vpy, args: ['-m', 'pip', 'install', '--disable-pip-version-check', ...pkgs],
      cwd: SELF_DIR, label: `安装 ${pkgs.join(' ')}` },
  ];
}

/** 报错时给人看的「手工修复」命令 */
function basePythonHint(pkgs = ['pyserial']) {
  const q = s => (/\s/.test(s) ? `"${s}"` : s);
  return `${q('<python>')} -m venv ${q(MONITOR_VENV)} && ${q(venvPythonPath())} -m pip install ${pkgs.join(' ')}`;
}

// ==================================================================
// 工程扫描（核心：同级目录）
// ==================================================================
function readBuildJson(dir) {
  try {
    const p = path.join(dir, 'build.json');
    if (!fs.existsSync(p)) return null;
    return JSON.parse(fs.readFileSync(p, 'utf8'));
  } catch (_) { return null; }
}

function detectIdfTarget(dir) {
  for (const f of ['sdkconfig.defaults', 'sdkconfig']) {
    try {
      const t = fs.readFileSync(path.join(dir, f), 'utf8');
      const m = t.match(/CONFIG_IDF_TARGET="([\w-]+)"/);
      if (m) return m[1];
    } catch (_) {}
  }
  return null;
}

function detectProject(dir, name) {
  const over = readBuildJson(dir) || {};

  // Arduino：目录下有 .ino
  let ino = null;
  try {
    ino = fs.readdirSync(dir).find(f => f.toLowerCase().endsWith('.ino'));
  } catch (_) {}

  const hasCmake = fs.existsSync(path.join(dir, 'CMakeLists.txt'));
  const hasMain = fs.existsSync(path.join(dir, 'main'));
  const looksIdf = hasCmake && (hasMain || fs.existsSync(path.join(dir, 'sdkconfig.defaults')));

  if (over.type === 'arduino' || (ino && !looksIdf)) {
    if (!ino && over.type !== 'arduino') return null;
    return {
      name, dir, type: 'arduino',
      sketch: ino,
      fqbn: over.fqbn || (over.fqbnEsp32 || 'esp32:esp32:esp32c3'),
      buildProps: Array.isArray(over.buildProps) ? over.buildProps : [],
      baud: over.baud || 921600,
      verbose: over.verbose,          // undefined = 跟随全局默认（开启）
    };
  }
  if (over.type === 'idf' || looksIdf) {
    return {
      name, dir, type: 'idf',
      target: over.target || detectIdfTarget(dir) || 'esp32c3',
      flashBaud: over.flashBaud || 921600,
      extraArgs: Array.isArray(over.extraArgs) ? over.extraArgs : [],
      verbose: over.verbose,          // undefined = 跟随全局默认（开启）
    };
  }
  return null;
}

function scanProjects() {
  const out = [];
  let names;
  try { names = fs.readdirSync(WORKSPACE); } catch (_) { return out; }
  for (const n of names) {
    if (n.startsWith('.')) continue;
    const dir = path.join(WORKSPACE, n);
    let st;
    try { st = fs.statSync(dir); } catch (_) { continue; }
    if (!st.isDirectory()) continue;
    if (path.resolve(dir) === path.resolve(SELF_DIR)) continue;   // 跳过工具自己
    const info = detectProject(dir, n);
    if (info) out.push(info);
  }
  out.sort((a, b) => a.name.localeCompare(b.name));
  return out;
}

// 按名字取工程（只在扫描结果里找，杜绝路径穿越）
function getProject(name) {
  return scanProjects().find(p => p.name === name) || null;
}

// Arduino 工程的输出目录：放工程内 .build（增量快、产物好找）
function buildDirOf(proj) {
  return proj.type === 'arduino' ? path.join(proj.dir, '.build') : path.join(proj.dir, 'build');
}

// 收集 .bin 产物（递归），主 app bin 优先
function collectFirmware(dir) {
  const out = [];
  if (!dir || !fs.existsSync(dir)) return out;
  (function walk(d, depth) {
    if (depth > 4) return;
    let names;
    try { names = fs.readdirSync(d); } catch (_) { return; }
    for (const n of names) {
      const full = path.join(d, n);
      let st;
      try { st = fs.statSync(full); } catch (_) { continue; }
      if (st.isDirectory()) walk(full, depth + 1);
      else if (n.toLowerCase().endsWith('.bin')) {
        out.push({
          name: path.relative(dir, full).split(path.sep).join('/'),
          size: st.size,
          mtime: st.mtimeMs,
        });
      }
    }
  })(dir, 0);
  const secondary = n => /(bootloader|partition[-_]?table|merged)/i.test(n) ? 1 : 0;
  out.sort((a, b) => (secondary(a.name) - secondary(b.name)) || (b.size - a.size));
  return out;
}

// ==================================================================
// 串口枚举
// ==================================================================
function listPorts() {
  if (isWin()) {
    // Windows 没有可靠的原生枚举，交给 arduino-cli（若可用）
    if (!ARDUINO_CLI) return [];
    try {
      const out = execFileSync(ARDUINO_CLI, ['board', 'list', '--format', 'json'],
        { encoding: 'utf8', timeout: 20000 });
      const j = JSON.parse(out.replace(/^\uFEFF/, ''));
      return (j.detected_ports || [])
        .filter(p => p.port && p.port.address)
        .map(p => p.port.address);
    } catch (_) { return []; }
  }
  const res = [];
  let files;
  try { files = fs.readdirSync('/dev'); } catch (_) { return res; }
  // macOS 优先 cu.*（tty.* 打开会等待 DCD，容易卡住）；Linux 用 ttyUSB/ttyACM
  const re = /^(cu\.usbserial[\w.-]*|cu\.usbmodem[\w.-]*|cu\.SLAB[\w.-]*|ttyUSB\d+|ttyACM\d+|ttyAMA\d+)$/;
  for (const f of files) if (re.test(f)) res.push('/dev/' + f);
  return res.sort();
}

// ==================================================================
// SSE 工具
// ==================================================================
function sseHeaders(res) {
  res.writeHead(200, {
    'Content-Type': 'text/event-stream; charset=utf-8',
    'Cache-Control': 'no-cache, no-transform',
    'Connection': 'keep-alive',
    'X-Accel-Buffering': 'no',
  });
  try { res.write('retry: 3000\n\n'); } catch (_) {}
}
function sseSend(res, event, data) {
  try {
    res.write('event: ' + event + '\n');
    res.write('data: ' + JSON.stringify(data) + '\n\n');
  } catch (_) { /* 客户端已断开 */ }
}

// 杀整棵进程树：命令链是 shell -> python(idf.py/esptool) -> 工具，
// 只杀 shell 会留下孙进程继续占用串口（表现为"停止后串口仍被占用"）。
function killTree(child) {
  if (!child || !child.pid) return;
  try {
    if (isWin()) {
      spawn('taskkill', ['/pid', String(child.pid), '/T', '/F'], { stdio: 'ignore' });
    } else {
      child.kill('SIGKILL');
    }
  } catch (_) {
    try { child.kill('SIGKILL'); } catch (__) {}
  }
}

// 串口连接失败时 esptool 可能退出码为 0 但仍算失败，用输出关键字兜底判定
const ESP_FAIL_RE = /Failed to connect to (ESP8266|ESP32|ESP32-S[23]|ESP32-C[236]|ESP32-H2|ESP32-P4)|fatal esptool\.py error|Timed out waiting for packet header|A fatal error occurred/i;

/**
 * SSE 流式执行一条命令
 * @param res        SSE 响应对象
 * @param opts       { cmd, args, cwd, env, label, shell, buildDir, tail }
 */
function runStream(res, opts) {
  sseHeaders(res);
  sseSend(res, 'start', { label: opts.label, cmd: opts.cmd, args: opts.args || [] });
  if (opts.cwd) sseSend(res, 'sys', `[目录] ${opts.cwd}`);

  let child;
  try {
    child = spawn(opts.cmd, opts.args || [], {
      cwd: opts.cwd || WORKSPACE,
      env: opts.env || process.env,
      windowsHide: true,
    });
  } catch (e) {
    sseSend(res, 'err', 'spawn 失败: ' + e.message);
    sseSend(res, 'done', { code: -1 });
    try { res.end(); } catch (_) {}
    return;
  }

  // 客户端断开（点"停止"或关页面）→ 杀掉整棵进程树
  res.on('close', () => { if (!child.killed) killTree(child); });

  // 串口监视：登记成「当前可写串口」，供 /api/serial-send 复用同一个句柄
  if (opts.serialPort) bindSerialChild(res, child, opts.serialPort);

  let buf = '';
  child.stdout.on('data', d => { const s = d.toString(); buf += s; sseSend(res, 'out', s); });
  child.stderr.on('data', d => { const s = d.toString(); buf += s; sseSend(res, 'err', s); });
  child.on('error', e => {
    sseSend(res, 'err', 'spawn 失败: ' + e.message);
    sseSend(res, 'done', { code: -1 });
    try { res.end(); } catch (_) {}
  });
  child.on('close', code => {
    code = code == null ? 0 : code;
    if (g_monitor && g_monitor.child === child) {
      g_monitor = null;
      sseSend(res, 'sys', '[发送] 串口已关闭');
    }
    // 兜底：连接/下载失败但退出码为 0 的情况
    if (code === 0 && ESP_FAIL_RE.test(buf)) {
      code = 1;
      sseSend(res, 'err', '[判定] 输出中发现 esptool 失败关键字，按失败处理');
    }
    if (opts.after) { try { opts.after(res, code, buf); } catch (_) {} }
    // 缺第三方库时给一条能直接抄的命令。arduino-cli 的原始报错只说
    // "fatal error: xxx.h: No such file or directory"，看不出该装哪个库。
    if (code !== 0) emitMissingLibHint(res, buf);
    sseSend(res, 'done', { code });
    try { res.end(); } catch (_) {}
  });
}

// ---- 缺库提示 ------------------------------------------------------
// 头文件 → 库名的映射只覆盖本仓库真正用到的。没有映射的也能兜住：
// 用 `arduino-cli lib search` 找 header 对应的库，实在不行给出通用命令。
const HEADER_TO_LIB = {
  'WebSocketsServer.h': 'WebSockets',      // Links2004/arduinoWebSockets
  'WebSocketsClient.h': 'WebSockets',
  'PubSubClient.h':     'PubSubClient',    // Nick O'Leary
  'ArduinoJson.h':      'ArduinoJson',
  'ESPAsyncWebServer.h': 'ESPAsyncWebServer',
  'AsyncTCP.h':         'AsyncTCP',
};

function emitMissingLibHint(res, buf) {
  // 注意必须带 g 标志：matchAll 对非全局正则会抛 TypeError，
  // 而这里在 close 回调里，抛出去会把后面的 sseSend('done') 和 res.end() 一起吞掉
  const missing = [...new Set(
    [...String(buf).matchAll(/fatal error:\s*([A-Za-z0-9_./+-]+\.h(?:pp)?):\s*No such file/gi)]
      .map(m => m[1].split('/').pop())
  )];
  if (!missing.length) return;

  const libs = [...new Set(missing.map(h => HEADER_TO_LIB[h] || null))];
  sseSend(res, 'err', '[缺库] 找不到头文件：' + missing.join('  '));
  if (libs.every(Boolean)) {
    sseSend(res, 'err', '[缺库] 这些都是第三方库，arduino-cli 不会自动装。执行：'
                      + `arduino-cli lib install ${libs.map(l => `"${l}"`).join(' ')}`);
  } else {
    sseSend(res, 'err', '[缺库] 先查库名：arduino-cli lib search <头文件名>'
                      + '，再 arduino-cli lib install "<库名>"');
  }
  sseSend(res, 'err', '[缺库] 装完 arduino-cli lib list 确认，然后重跑编译。'
                    + '（库装在 <Documents>/Arduino/libraries/，换机器/重装系统后最容易漏这一步）');
}

/**
 * 顺序执行多条命令，输出共用一条 SSE 流；某条非 0 退出就停下。
 * 用于「先建 venv、再装 pyserial、然后直接开始串口监视」这种流水线——
 * 前端点一次「串口监视」，中间步骤的输出也看得见，最后一步退出才算 done。
 * @param steps  [{ cmd, args, cwd, label } | { note: 'sse sys 行' }]
 */
function runSteps(res, steps) {
  sseHeaders(res);
  let i = 0;
  const finish = code => { sseSend(res, 'done', { code }); try { res.end(); } catch (_) {} };
  const next = () => {
    if (i >= steps.length) return finish(0);
    const st = steps[i++];
    if (st.note) { sseSend(res, 'sys', st.note); return next(); }

    sseSend(res, 'start', { label: st.label, cmd: st.cmd, args: st.args || [] });
    let child;
    try {
      child = spawn(st.cmd, st.args || [], {
        cwd: st.cwd || WORKSPACE, env: process.env, windowsHide: true,
      });
    } catch (e) {
      sseSend(res, 'err', 'spawn 失败: ' + e.message);
      return finish(-1);
    }
    res.on('close', () => { if (!child.killed) killTree(child); });
    if (st.serialPort) bindSerialChild(res, child, st.serialPort);
    child.stdout.on('data', d => sseSend(res, 'out', d.toString()));
    child.stderr.on('data', d => sseSend(res, 'err', d.toString()));
    child.on('error', e => { sseSend(res, 'err', 'spawn 失败: ' + e.message); finish(-1); });
    child.on('close', code => {
      if (g_monitor && g_monitor.child === child) {
        g_monitor = null;
        sseSend(res, 'sys', '[发送] 串口已关闭');
      }
      if (code !== 0) { sseSend(res, 'err', `[失败] 退出码 ${code}`); return finish(code == null ? -1 : code); }
      next();
    });
  };
  next();
}

// ==================================================================
// 串口写方向：网页「发送」→ 监视进程的 stdin → 串口
// ==================================================================
// 为什么不能为「发送」单独开一个连接去写串口：
//   1) ESP32-C3 的 USB Serial/JTAG（HWCDC）一被打开就会复位芯片 ——
//      DTR/RTS 就是它的复位/BOOT 线。每次发命令重启一遍设备，没法用。
//   2) Windows 上同一个 COM 口不能被两个句柄同时打开，直接「拒绝访问」。
// 所以复用监视进程已经打开的那个句柄：把字节写进它的 stdin，
// 由 tools/serial-monitor.py 里的 stdin 线程原样转给串口。
let g_monitor = null;          // { child, port } —— 当前活着的串口监视进程

function serialSend(data, newline) {
  const m = g_monitor;
  if (!m || !m.child || !m.child.stdin || m.child.stdin.destroyed) {
    return { ok: false, reason: '没有正在运行的串口监视。先点「串口监视」把端口打开，再发命令。' };
  }
  // 行尾由这里决定：AT 固件是「行」解析，只有收到 \r 或 \n 才解析一行；
  // 但 CIPSEND 阶段要发不带行尾的裸字节，所以做成可关。
  const payload = Buffer.from(data + (newline ? '\r\n' : ''), 'utf8');
  try {
    m.child.stdin.write(payload);
  } catch (e) {
    return { ok: false, reason: '写入失败: ' + e.message };
  }
  return { ok: true, port: m.port, bytes: payload.length };
}

/** 登记/注销「当前可写串口」；child 退出时自动注销 */
function bindSerialChild(res, child, port) {
  g_monitor = { child, port };
  sseSend(res, 'sys', `[发送] 已就绪：下方输入框可直接发命令（行尾自动 \\r\\n，可关）`);
}

// ==================================================================
// 命令构造
// ==================================================================
function shq(s) { return "'" + String(s).replace(/'/g, "'\\''") + "'"; }

// esp32 核的 build.extra_flags 是合并字符串，直接覆盖会丢掉 -DESP32=ESP32 等；
// 自定义宏要写进 build.defines。esp8266 核相反。
function extraFlagKey(fqbn) {
  return String(fqbn).startsWith('esp32:esp32:') ? 'build.defines' : 'build.extra_flags';
}

function arduinoCompileArgs(proj, port, upload, verbose) {
  const args = ['compile', '--fqbn', proj.fqbn, '--output-dir', buildDirOf(proj)];
  // --verbose 让 arduino-cli 打印完整 gcc/avr-g++ 命令行（默认只打印进度摘要）
  if (verbose) args.push('--verbose');
  if (upload) args.push('--upload', '-p', port);
  for (const bp of proj.buildProps || []) args.push('--build-property', bp);
  args.push(proj.dir);
  return args;
}

// 在工程目录里执行 idf.py（需要先 source export.sh 才有环境）
function idfShellCmd(inner, proj) {
  const parts = [];
  if (IDF_DIR) {
    const ex = path.join(IDF_DIR, isWin() ? 'export.bat' : 'export.sh');
    parts.push(isWin() ? `. ${shq(ex)}` : `source ${shq(ex)}`);
  }
  parts.push(`idf.py ${inner}`);
  return parts.join(isWin() ? '; ' : ' && ');
}

function idfExtraEnv(proj) {
  return Object.assign({}, process.env, { ESPBAUD: String(proj.flashBaud || 921600) });
}

// 执行 idf.py 命令（Windows 用 PowerShell，其余用 bash）
function runIdf(res, proj, inner, label, after) {
  const shell = idfShellCmd(inner, proj);
  if (isWin()) {
    runStream(res, {
      cmd: 'powershell.exe',
      args: ['-NoProfile', '-NonInteractive', '-Command', shell],
      cwd: proj.dir, env: idfExtraEnv(proj), label, after,
    });
  } else {
    runStream(res, {
      cmd: 'bash', args: ['-c', shell],
      cwd: proj.dir, env: idfExtraEnv(proj), label, after,
    });
  }
}

// op: build | flash(编译+烧录) | upload(只烧 app) | erase
// 目标芯片由工程自带的 sdkconfig / sdkconfig.defaults 决定，无需 -DIDF_TARGET
// verbose：加全局 -v。idf.py 会把 -v 透传给 ninja/make（tools.py: run_target），
//          从而打印每条完整编译命令；不加时 ninja 只打 [n/N] 进度行。
//          注意 -v 是 idf.py 的全局选项，必须放在子命令之前。
function idfCmdFor(proj, op, port, verbose) {
  const extra = (proj.extraArgs || []).join(' ');
  const baud = proj.flashBaud || 921600;
  const v = verbose ? '-v ' : '';
  switch (op) {
    case 'build':  return `${v}build ${extra}`.trim();
    case 'flash':  return `${v}-p ${port} -b ${baud} flash ${extra}`.trim();
    case 'upload': return `${v}-p ${port} -b ${baud} app-flash ${extra}`.trim();
    // 擦除不跟随 verbose：esptool 的 -v 会 dump 整个 stub 十六进制，噪声过大
    case 'erase':  return `-p ${port} erase-flash`;
    default:       return `${v}build`.trim();
  }
}

// ==================================================================
// HTTP 服务
// ==================================================================
function sendJSON(res, obj, code) {
  const body = JSON.stringify(obj);
  res.writeHead(code || 200, {
    'Content-Type': 'application/json; charset=utf-8',
    'Cache-Control': 'no-store',
  });
  res.end(body);
}
function sendText(res, text, code) {
  res.writeHead(code || 200, { 'Content-Type': 'text/plain; charset=utf-8' });
  res.end(text);
}

// FQBN 形如 VENDOR:ARCH:BOARD[:OPT=VAL[,OPT=VAL]...]
// 第 4 段是板级选项，必须允许 "=" 和 ","（如 esp32:esp32:esp32c3:CDCOnBoot=cdc）。
// 仍然排除空格、引号、$、反引号、分号、&、|、/、\ 等可注入字符。
const FQBN_RE = /^(?:[\w.-]+:){2}[\w.-]+(?::[\w.-]+(?:=[\w.,-]+)?(?:,[\w.-]+(?:=[\w.,-]+)?)*)?$/;
const PORT_RE = /^(\/[\w./\-]+|COM\d+)$/i;

function serveIndex(res) {
  const p = path.join(PUBLIC_DIR, 'index.html');
  fs.readFile(p, (err, data) => {
    if (err) return sendText(res, 'index.html 缺失: ' + p, 500);
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(data);
  });
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://' + (req.headers.host || 'localhost'));
  const p = url.pathname;
  const q = url.searchParams;

  try {
    if (p === '/' || p === '/index.html') return serveIndex(res);

    // ---- 工程列表 ----
    if (p === '/api/projects') {
      const list = scanProjects().map(pr => ({
        name: pr.name,
        type: pr.type,
        detail: pr.type === 'arduino' ? pr.fqbn : ('target=' + pr.target),
        sketch: pr.sketch || null,
        fqbn: pr.fqbn || null,
        target: pr.target || null,
        baud: pr.type === 'arduino' ? pr.baud : pr.flashBaud,
        verbose: pr.verbose !== false,
      }));
      return sendJSON(res, { workspace: WORKSPACE, projects: list });
    }

    // ---- 工具链状态 ----
    if (p === '/api/env') return sendJSON(res, envInfo());

    // ---- 串口列表 ----
    if (p === '/api/ports') return sendJSON(res, { ports: listPorts() });

    // ---- 固件产物 ----
    if (p === '/api/firmware') {
      const proj = getProject(q.get('project'));
      if (!proj) return sendJSON(res, { error: '工程不存在' }, 400);
      return sendJSON(res, { files: collectFirmware(buildDirOf(proj)) });
    }

    // ---- 下载产物 ----
    if (p === '/api/download') {
      const proj = getProject(q.get('project'));
      const file = q.get('file') || '';
      if (!proj) return sendText(res, '工程不存在', 400);
      const root = path.resolve(buildDirOf(proj));
      const fp = path.resolve(root, file);
      // 只允许下载 build 目录内的文件
      if (fp !== root && !fp.startsWith(root + path.sep)) return sendText(res, 'forbidden', 403);
      if (!fs.existsSync(fp) || !fs.statSync(fp).isFile()) return sendText(res, 'not found', 404);
      res.writeHead(200, {
        'Content-Type': 'application/octet-stream',
        'Content-Disposition': `attachment; filename="${path.basename(fp)}"`,
      });
      fs.createReadStream(fp).pipe(res);
      return;
    }

    // ---- 编译 / 烧录 / 擦除（SSE） ----
    if (p === '/api/action') {
      const proj = getProject(q.get('project'));
      if (!proj) { sseHeaders(res); sseSend(res, 'err', '工程不存在'); sseSend(res, 'done', { code: -1 }); return res.end(); }

      const op = q.get('op') || 'build';
      const port = q.get('port') || '';
      const fqbnOverride = q.get('fqbn') || '';

      // 详细日志（打印完整编译命令）默认开启：
      //   显式传 verbose=0/false/no/off 才关闭，不传即为开
      //   工程 build.json 里写 "verbose": false 可把该工程默认改成关闭
      const vq = q.get('verbose');
      const verbose = vq == null || vq === ''
        ? proj.verbose !== false
        : !/^(0|false|no|off)$/i.test(vq);

      if (proj.type === 'arduino') {
        if (!ARDUINO_CLI) {
          sseHeaders(res);
          sseSend(res, 'err', '未找到 arduino-cli。请安装，或设置环境变量 ARDUINO_CLI_PATH 指向它。');
          sseSend(res, 'done', { code: -1 });
          return res.end();
        }
        const fqbn = fqbnOverride || proj.fqbn;
        if (!FQBN_RE.test(fqbn)) { sseHeaders(res); sseSend(res, 'err', 'FQBN 不合法: ' + fqbn); sseSend(res, 'done', { code: -1 }); return res.end(); }
        const p2 = Object.assign({}, proj, { fqbn });

        const afterArtifacts = (r, code) => {
          if (code === 0) sseSend(r, 'firmware', { files: collectFirmware(buildDirOf(p2)) });
        };

        if (op === 'build') {
          return runStream(res, {
            cmd: ARDUINO_CLI, args: arduinoCompileArgs(p2, null, false, verbose),
            cwd: p2.dir, label: 'arduino 编译', after: afterArtifacts,
          });
        }
        if (op === 'flash' || op === 'upload') {
          if (!PORT_RE.test(port)) { sseHeaders(res); sseSend(res, 'err', '串口不合法'); sseSend(res, 'done', { code: -1 }); return res.end(); }
          if (op === 'flash') {
            // 编译 + 烧录一步到位
            return runStream(res, {
              cmd: ARDUINO_CLI, args: arduinoCompileArgs(p2, port, true, verbose),
              cwd: p2.dir, label: 'arduino 编译并烧录', after: afterArtifacts,
            });
          }
          // 只烧已编译产物
          const args = ['upload', '--fqbn', p2.fqbn, '-p', port, '--input-dir', buildDirOf(p2)];
          if (verbose) args.push('--verbose');
          args.push(p2.dir);
          return runStream(res, { cmd: ARDUINO_CLI, args, cwd: p2.dir, label: 'arduino 烧录已有产物' });
        }
        if (op === 'erase') {
          if (!PORT_RE.test(port)) { sseHeaders(res); sseSend(res, 'err', '串口不合法'); sseSend(res, 'done', { code: -1 }); return res.end(); }
          // arduino-cli 没有独立擦除命令，用 esptool erase_flash
          const esp = which('esptool.py') || which('esptool');
          const eraseArgs = ['-m', 'esptool', '--port', port, 'erase_flash'];
          if (esp) {
            return runStream(res, { cmd: esp, args: ['--port', port, 'erase_flash'], cwd: p2.dir, label: '擦除 flash' });
          }
          // esptool 不在 PATH：看哪个解释器装了它（ESP-IDF 的 python_env 里通常有）
          const withEsp = findPythonWith('esptool');
          if (withEsp) {
            return runStream(res, { cmd: withEsp.path, args: eraseArgs, cwd: p2.dir, label: '擦除 flash' });
          }
          // 都没有：装进工具自带 .venv 再擦（与串口监视共用同一个环境）
          const prep = venvInstallSteps(['esptool'], '未找到 esptool');
          if (!prep) {
            sseHeaders(res);
            sseSend(res, 'err', '未找到 esptool，也没有可用的 Python 来安装它。');
            sseSend(res, 'err', '解决：pip install esptool；或用 ESPMON_PYTHON 指定一个装了 esptool 的解释器。');
            sseSend(res, 'done', { code: -1 });
            return res.end();
          }
          return runSteps(res, [
            ...prep,
            { note: '依赖就绪，开始擦除…' },
            { cmd: venvPythonPath(), args: eraseArgs, cwd: p2.dir, label: '擦除 flash' },
          ]);
        }
        sseHeaders(res); sseSend(res, 'err', '未知操作: ' + op); sseSend(res, 'done', { code: -1 });
        return res.end();
      }

      // ---- ESP-IDF 工程 ----
      if (!IDF_DIR && !which('idf.py')) {
        sseHeaders(res);
        sseSend(res, 'err', '未找到 ESP-IDF 环境。请设置环境变量 IDF_PATH 指向 esp-idf 目录。');
        sseSend(res, 'done', { code: -1 });
        return res.end();
      }
      if (op === 'erase' && !PORT_RE.test(port)) {
        sseHeaders(res); sseSend(res, 'err', '串口不合法'); sseSend(res, 'done', { code: -1 }); return res.end();
      }
      if ((op === 'flash' || op === 'upload') && !PORT_RE.test(port)) {
        sseHeaders(res); sseSend(res, 'err', '串口不合法'); sseSend(res, 'done', { code: -1 }); return res.end();
      }

      const labelMap = { build: 'IDF 编译', flash: 'IDF 编译并烧录', upload: 'IDF 仅烧录 app', erase: 'IDF 擦除 flash' };
      const after = (r, code) => {
        if (code === 0 && op !== 'erase') sseSend(r, 'firmware', { files: collectFirmware(buildDirOf(proj)) });
      };
      return runIdf(res, proj, idfCmdFor(proj, op, port, verbose), labelMap[op] || op, after);
    }

    // ---- 串口监视（SSE，走 pyserial） ----
    if (p === '/api/monitor') {
      const port = q.get('port') || '';
      const baud = q.get('baud') || '115200';
      if (!PORT_RE.test(port)) { sseHeaders(res); sseSend(res, 'err', '串口不合法'); sseSend(res, 'done', { code: -1 }); return res.end(); }
      if (!/^\d{4,7}$/.test(baud)) { sseHeaders(res); sseSend(res, 'err', '波特率不合法'); sseSend(res, 'done', { code: -1 }); return res.end(); }
      if (!fs.existsSync(MONITOR_SCRIPT)) {
        sseHeaders(res); sseSend(res, 'err', '缺少 tools/serial-monitor.py'); sseSend(res, 'done', { code: -1 }); return res.end();
      }
      const monArgs = [MONITOR_SCRIPT, '--port', port, '--baud', baud];
      const label = `串口监视 ${port}@${baud}`;

      // 1) 首选：找到「真的能 import serial」的解释器，直接开监视
      const mon = findMonitorPython();
      if (mon) {
        return runStream(res, { cmd: mon.path, args: monArgs, cwd: SELF_DIR, label, serialPort: port });
      }

      // 2) 没找到：在工具目录建 .venv 装一份 pyserial，装完接着开监视。
      //    只发生一次，之后都走上面的快路径。MONITOR_NO_AUTOINSTALL=1 可关掉。
      const prep = MONITOR_AUTO_INSTALL ? venvInstallSteps(['pyserial'], '未找到装了 pyserial 的解释器') : null;
      if (!prep) {
        sseHeaders(res);
        sseSend(res, 'err', MONITOR_AUTO_INSTALL ? '没有可用的 Python 解释器，无法自动补 pyserial。'
                                                 : '没有找到装了 pyserial 的 Python 解释器（已禁用自动安装）。');
        sseSend(res, 'err', '已尝试：' + (pythonCandidates().join('  |  ') || '(PATH 里没有 python)'));
        sseSend(res, 'err', '解决：① 安装 Python 3 并勾选 Add to PATH；'
                          + '② 用 ESPMON_PYTHON=<装了 pyserial 的 python> 指定解释器；'
                          + `③ 手工执行 ${basePythonHint()}`);
        sseSend(res, 'done', { code: 3 });
        return res.end();
      }
      return runSteps(res, [
        ...prep,
        { note: '依赖就绪，开始串口监视…' },
        { cmd: venvPythonPath(), args: monArgs, cwd: SELF_DIR, label, serialPort: port },
      ]);
    }

    // ---- 往正在运行的串口监视发数据（网页「发送」按钮）----
    // GET /api/serial-send?data=<urlencoded>&nl=1
    //   data  任意字符串，浏览器侧 encodeURIComponent，可以带换行等控制字符
    //   nl    是否在末尾补 \r\n（默认补）。AT 固件是「行」解析，必须补；
    //         但 CIPSEND 之后要发不带行尾的裸字节，那时传 nl=0
    if (p === '/api/serial-send') {
      const data = q.get('data') || '';
      if (data.length > 4096) return sendJSON(res, { ok: false, reason: '单次最多 4096 字符' }, 400);
      const nlRaw = q.get('nl');
      const nl = nlRaw == null || nlRaw === '' ? true : !/^(0|false|no|off)$/i.test(nlRaw);
      return sendJSON(res, serialSend(data, nl));
    }

    // ---- 打开工程目录（本机文件管理器） ----
    if (p === '/api/open') {
      const proj = getProject(q.get('project'));
      if (!proj) return sendJSON(res, { ok: false, reason: '工程不存在' }, 400);
      const opener = isWin() ? 'explorer' : (process.platform === 'darwin' ? 'open' : 'xdg-open');
      try {
        spawn(opener, [proj.dir], { detached: true, stdio: 'ignore' }).unref();
        return sendJSON(res, { ok: true });
      } catch (e) {
        return sendJSON(res, { ok: false, reason: e.message });
      }
    }

    return sendText(res, 'not found', 404);
  } catch (e) {
    try { sendText(res, 'server error: ' + e.message, 500); } catch (_) {}
  }
});

server.listen(PORT, HOST, () => {
  const info = envInfo();
  console.log('========================================');
  console.log(' esp-build-tool');
  console.log(`   地址      http://${HOST}:${PORT}`);
  console.log(`   工程根    ${WORKSPACE}`);
  console.log(`   arduino-cli  ${info.arduinoCli || '(未找到)'}${info.arduinoCliVersion ? '  ' + info.arduinoCliVersion : ''}`);
  console.log(`   ESP-IDF      ${info.idfDir || '(未找到)'}${info.idfVersion ? '  ' + info.idfVersion : ''}`);
  console.log(`   串口监视     ${info.monitorPython
    ? `${info.monitorPython}  (pyserial ${info.pyserial})`
    : '尚无可用解释器，点「串口监视」会先在 .venv 里自动装 pyserial'}`);
  const ps = scanProjects();
  console.log(`   发现工程  ${ps.length} 个: ${ps.map(x => x.name + '(' + x.type + ')').join(', ') || '(无)'}`);
  console.log('========================================');
});
