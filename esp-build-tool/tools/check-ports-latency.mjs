// 回归检查：串口枚举绝不能再把事件循环挡住。
//
// 背景（2026-09-30 修过的真 bug）：
//   /api/ports 以前走 execFileSync(arduino-cli board list)，同步阻塞整个 Node
//   事件循环。实测单次枚举 1.8~2.4s，期间 /api/env 这种本该 0.2s 的接口被拖到
//   2.0s；连点 3 次「刷新」变成 1.8/3.6/5.4s 完美串行 —— 现象就是「网页点一下
//   整个卡死」。代码里 execFileSync 看上去人畜无害，所以只能靠跑一遍来防回归。
//
// 用法：node tools/check-ports-latency.mjs      退出码 0 = 通过
import http from 'node:http';
import net from 'node:net';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function freePort() {
  return new Promise((res, rej) => {
    const s = net.createServer();
    s.once('error', rej);
    s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); });
  });
}

function get(port, p) {
  return new Promise((res, rej) => {
    const t = Date.now();
    const req = http.get({ host: '127.0.0.1', port, path: p, timeout: 60000 }, r => {
      let b = '';
      r.on('data', d => (b += d));
      r.on('end', () => res({ ms: Date.now() - t, body: b, code: r.statusCode }));
    });
    req.on('error', rej);
    req.on('timeout', () => req.destroy(new Error('timeout')));
  });
}

const sleep = ms => new Promise(r => setTimeout(r, ms));

const port = await freePort();
const srv = spawn(process.execPath, [path.join(DIR, 'server.js')],
  { env: { ...process.env, PORT: String(port) }, stdio: ['ignore', 'pipe', 'pipe'] });
let srvLog = '';
srv.stdout.on('data', d => (srvLog += d));
srv.stderr.on('data', d => (srvLog += d));

let failed = false;
try {
  // 等就绪（最多 15s）
  for (let i = 0; i < 60; i++) {
    try { await get(port, '/api/env'); break; }
    catch (_) { if (i === 59) throw new Error('服务端没起来：\n' + srvLog); await sleep(250); }
  }

  // 关键：两个请求同时发。枚举若是同步的，env 会被拖到和 ports 一样久。
  const [ports, env] = await Promise.all([get(port, '/api/ports'), get(port, '/api/env')]);
  const pj = JSON.parse(ports.body || '{}');
  console.log(`  /api/ports   ${String(ports.ms).padStart(5)}ms  来源=${pj.source}  服务端自报=${pj.ms}ms  串口=[${(pj.ports || []).join(',')}]`);
  console.log(`  /api/env     ${String(env.ms).padStart(5)}ms  （应与枚举耗时无关）`);

  if (ports.ms > 600 && env.ms > ports.ms * 0.5) {
    failed = true;
    console.log(`\n✗ 不通过：枚举期间 /api/env 被拖到 ${env.ms}ms（ports ${ports.ms}ms）`);
    console.log('  说明枚举又变成阻塞式的了 —— 检查 listPortsAsync/winPortsFrom* 里有没有 execFileSync。');
  } else if (ports.ms > 600) {
    console.log(`\n✓ 通过：枚举 ${ports.ms}ms 期间 /api/env 仍只有 ${env.ms}ms（未被拖住）`);
  } else {
    console.log(`\n✓ 通过：枚举本身很快（${ports.ms}ms，走 ${pj.source} 快路径）`);
  }
} catch (e) {
  failed = true;
  console.log('✗ 检查过程出错：' + e.message);
} finally {
  srv.kill();
}
process.exit(failed ? 1 : 0);
