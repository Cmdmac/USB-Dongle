// index_html.h - 网页管理页（WebServer 返回），含 WebSocket 串口终端
// 注意：本文件是 .h（非 .ino），不会被 Arduino 预处理插入函数原型，
// 所以这里的 JS/CSS/HTML 结构不受 .ino 原型坑影响。
#pragma once

// R"()" 原始字符串——C++11，内容里的引号无需转义
static const char INDEX_HTML[] = R"HTML(
<!doctype html><html lang=zh><head><meta charset=utf-8>
<meta name=viewport content='width=device-width,initial-scale=1'>
<title>ESP32C3 无线串口</title>
<style>
*{box-sizing:border-box}
body{font-family:-apple-system,sans-serif;margin:0;background:#f2f3f5;color:#222}
.wrap{max-width:820px;margin:0 auto;padding:16px}
h2{margin:8px 0 16px}
.card{background:#fff;border-radius:10px;padding:16px;margin-bottom:14px;box-shadow:0 1px 3px rgba(0,0,0,.08)}
h3{margin:0 0 12px;font-size:15px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
label{font-size:12px;color:#666;display:block;margin-bottom:4px}
input,select{width:100%;padding:7px;border:1px solid #ccd;border-radius:6px;font-size:13px}
button{padding:8px 16px;border:0;border-radius:6px;background:#2b6cff;color:#fff;cursor:pointer;font-size:13px}
button.sec{background:#e4e8ef;color:#333}
button.danger{background:#e05252}
#term{width:100%;height:260px;background:#101317;color:#d8f0d8;border:0;border-radius:6px;
font:12px/1.5 Menlo,Consolas,monospace;padding:10px;overflow:auto;white-space:pre-wrap}
#termIn{width:100%;box-sizing:border-box;margin-top:6px;padding:8px;border:1px solid #ccd;border-radius:6px;
font:13px Menlo,Consolas,monospace}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.pill{font-size:12px;padding:3px 9px;border-radius:12px;background:#eef;color:#335}
.st{font-size:13px;line-height:1.9}
#msg{font-size:12px;color:#2b7;min-height:16px}
</style></head><body><div class=wrap>
<h2>ESP32-C3 无线串口 <span class=pill id=ver></span></h2>

<div class=card><h3>运行状态</h3><div class=st id=status>加载中...</div></div>

<div class=card><h3>网页串口终端 <button class=sec onclick=termToggle() id=termBtn>连接</button></h3>
<div id=term></div>
<div class=row><input id=termIn placeholder='输入后回车发送到串口' autocomplete=off>
<label style='font-size:12px;color:#666;white-space:nowrap;margin:0'>
<input type=checkbox id=termCrlf checked style='width:auto;margin-right:4px'>行尾 +CRLF</label></div>
</div>

<div class=card><h3>网络唤醒</h3><div class=row>
<button class=sec onclick=wolAll()>全部唤醒</button></div></div>

<div class=card><h3>配置</h3>
<div class=grid>
<div><label>Wi-Fi SSID</label><input id=wifi_ssid></div>
<div><label>Wi-Fi 密码</label><input id=wifi_pass></div>
<div><label>主机名</label><input id=hostname></div>
<div><label>管理/数据密码</label><input id=password></div>
<div><label>网络模式</label><select id=net_mode>
<option value=0>TCP 服务端</option><option value=1>TCP 客户端</option>
<option value=2>Telnet</option><option value=3>关闭</option></select></div>
<div><label>本地端口</label><input id=net_port type=number></div>
<div><label>远端主机</label><input id=remote_host></div>
<div><label>远端端口</label><input id=remote_port type=number></div>
<div><label>换行转换</label><select id=nl_xlate>
<option value=0>原样</option><option value=1>LF→CRLF</option></select></div>
</div><p></p>
<div class=row><button onclick=saveCfg()>保存配置</button>
<button class=sec onclick=location.href='/ota'>固件升级</button>
<button class=danger onclick=doReboot()>重启设备</button>
<span id=msg></span></div></div>
</div>

<script>
let ws=null,termOn=false,cfgFilled=false;
async function getJSON(u){let r=await fetch(u);if(!r.ok)throw new Error('HTTP '+r.status);return r.json();}
async function loadStatus(){
try{
let s=await getJSON('/api/status');
document.getElementById('ver').textContent='v'+s.version;
document.getElementById('status').innerHTML=
'IP: <b>'+s.wifi.ip+'</b> &nbsp; Wi-Fi: '+s.wifi.connected+
' &nbsp; 网络: '+s.net.mode+':'+s.net.port+' ('+s.net.clients+')'+
' &nbsp; 串口: '+s.serial.side+' &nbsp; 运行: '+s.uptime_s+'s';
}catch(e){/* 设备切网/重启期间会短暂失败：保留上次显示，别把页面刷空 */}
}
// 配置表单只在进页面时读一次，之后绝不自动回填。
// ⚠️ 绝不能把回填放进 5s 轮询：轮询会拿设备里的旧值覆盖用户**正在敲**的输入框，
// 表现就是"填完 Wi-Fi 名、去填密码，回头 SSID 又空了"，而且完全没有提示。
// 需要重新同步时由 saveCfg() 显式调用本函数。
async function loadCfg(){
try{
let c=await getJSON('/api/config');
for(let k of ['wifi_ssid','wifi_pass','hostname','password','net_port','remote_host','remote_port'])
document.getElementById(k).value=c[k]||'';
document.getElementById('net_mode').value=c.net_mode;
document.getElementById('nl_xlate').value=c.nl_xlate||0;
cfgFilled=true;
}catch(e){/* 401（设了管理密码尚未通过认证）或切网中：保持 cfgFilled=false，下次再试 */}
}
async function load(){await loadStatus();if(!cfgFilled)await loadCfg();}
function termToggle(){
if(termOn){ws&&ws.close();termOn=false;document.getElementById('termBtn').textContent='连接';return;}
let proto=location.protocol==='https:'?'wss':'ws';
// 端口必须写死 81：WebSocket 由独立的 WebSocketsServer 提供，而页面本身是
// WebServer(80) 发的，两者不同端口（旧 IDF 版两者同在一个 httpd 上，所以那时
// 用 location.host 才对；改 Arduino 版后 WS 搬到了 81，这里没跟着改就会连不上）。
// 与固件里的 WS_PORT 保持一致。路径随意，写 /ws 只为可读。
ws=new WebSocket(proto+'://'+location.hostname+':81/ws');termOn=true;
ws.binaryType='arraybuffer';   // 串口原始字节走 binary 帧，避免 UTF-8 解码破坏
let authed=false,asked=false;
document.getElementById('termBtn').textContent='断开';
ws.onopen=()=>{let t=document.getElementById('term');t.textContent+='[已连接]\n';t.scrollTop=t.scrollHeight;};
ws.onmessage=e=>{
let t=document.getElementById('term');
if(typeof e.data==='string'){
// 应用层鉴权握手：服务端发 "AUTH" -> 回 "AUTH:<密码>" -> 收 "OK"
if(e.data==='AUTH'&&!authed){
let pw=prompt('终端已设置密码，请输入:');
if(pw===null)pw='';
ws.send('AUTH:'+pw);asked=true;return;}
if(e.data==='OK'){authed=true;let t2=document.getElementById('term');
t2.textContent+='[鉴权通过]\n';t2.scrollTop=t2.scrollHeight;return;}
}
let txt=typeof e.data==='string'?e.data:(function(){
let u=new Uint8Array(e.data),s='';
for(let i=0;i<u.length;i++)s+=String.fromCharCode(u[i]);
return s;})();
t.textContent+=txt;t.scrollTop=t.scrollHeight;};
ws.onclose=()=>{termOn=false;authed=false;document.getElementById('termBtn').textContent='连接';
let t=document.getElementById('term');
t.textContent+=asked&&!authed?'[鉴权失败或超时]\n':'[已断开]\n';};
}
// 输入框回车 -> 发到串口（按 UTF-8 编码，多字节字符也正确）
// ⚠️ 默认补 CRLF：bridgeNetRx() 是原样透传，网络->串口方向没有任何换行转换
// （nl_xlate 只管串口->网络）。而不带行尾的裸文本，多数设备根本不当一条命令。
document.getElementById('termIn').addEventListener('keydown',function(e){
if(e.key!=='Enter')return;
let v=this.value;
if(!v)return;
let cr=document.getElementById('termCrlf').checked?'\r\n':'';
if(ws&&ws.readyState===1)ws.send(v+cr);   // 字符串 send 走 text 帧，UTF-8 编码
this.value='';
let t=document.getElementById('term');
t.textContent+='> '+v+(cr?'\\r\\n':'')+'\n';t.scrollTop=t.scrollHeight;
});
async function saveCfg(){let c={};
for(let k of ['wifi_ssid','wifi_pass','hostname','password','net_port','remote_host','remote_port'])
c[k]=document.getElementById(k).value;
c.net_mode=+document.getElementById('net_mode').value;
c.nl_xlate=+document.getElementById('nl_xlate').value;
let m=document.getElementById('msg');
try{
let r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(c)});
if(!r.ok){m.textContent='保存失败：HTTP '+r.status+(r.status===401?'（管理密码不对）':'');return;}
let j=await r.json();
if(j.wifi_changed){
// 换 Wi-Fi 后设备会 disconnect 再 begin，本页（来自旧网络）必然断开，
// 这时候再去重读配置只会失败并把输入框刷空，所以保留用户填的内容并给出去向提示。
m.textContent='已保存，正在切换 Wi-Fi：本页会断开，请连到新网络后用新 IP 访问';
}else{
m.textContent='已保存';
setTimeout(loadCfg,300);   // 重读一次，显示设备侧真实落盘值
}
}catch(e){m.textContent='保存请求失败：'+e.message+'(设备可能已在切换网络)';}
}
async function wolAll(){await fetch('/api/wol',{method:'POST',headers:{'Content-Type':'application/json'},body:'{"all":true}'});}
async function doReboot(){if(confirm('确认重启?'))await fetch('/api/reboot',{method:'POST'});}
load();setInterval(load,5000);
</script></body></html>
)HTML";
