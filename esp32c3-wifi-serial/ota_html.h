// ota_html.h - OTA 升级页（WebServer 返回）
#pragma once

static const char OTA_HTML[] = R"HTML(
<!doctype html><html lang=zh><head><meta charset=utf-8>
<meta name=viewport content='width=device-width,initial-scale=1'>
<title>固件升级</title>
<style>
*{box-sizing:border-box}
body{font-family:-apple-system,sans-serif;margin:0;background:#f2f3f5;color:#222}
.wrap{max-width:560px;margin:0 auto;padding:16px}
.card{background:#fff;border-radius:10px;padding:20px;margin-bottom:14px;box-shadow:0 1px 3px rgba(0,0,0,.08)}
h2{margin:8px 0 16px;font-size:18px}
input[type=file]{width:100%;margin-bottom:14px;font-size:13px}
button{padding:10px 24px;border:0;border-radius:6px;background:#2b6cff;color:#fff;cursor:pointer;font-size:14px}
button:disabled{opacity:.5;cursor:not-allowed}
#bar{height:12px;background:#e8ebf0;border-radius:6px;margin-top:16px;overflow:hidden;display:none}
#barFill{height:100%;width:0;background:#2b7;background:linear-gradient(90deg,#2b7,#2b6cff);transition:width .2s}
#msg{font-size:13px;margin-top:10px;min-height:18px;color:#666}
</style></head><body><div class=wrap>
<div class=card>
<h2>固件升级（OTA）</h2>
<p style="font-size:13px;color:#666;line-height:1.7">
选择编译出的 <b>app .bin</b>（Arduino 输出目录里的 <code>xxx.ino.bin</code>）。<br>
上传完成后设备自动重启，期间请勿断电。</p>
<input type=file id=file accept=".bin">
<button id=btn onclick=up()>上传并升级</button>
<div id=bar><div id=barFill></div></div>
<div id=msg></div>
</div>
<p><a href=/ style="font-size:13px">&larr; 返回管理页</a></p>
</div>
<script>
function up(){
var f=document.getElementById('file').files[0];
if(!f){alert('请先选择 .bin 文件');return;}
var btn=document.getElementById('btn');btn.disabled=true;
document.getElementById('bar').style.display='block';
document.getElementById('msg').textContent='上传中...';
var xhr=new XMLHttpRequest();
xhr.open('POST','/ota/upload',true);
xhr.upload.onprogress=function(e){
if(e.lengthComputable){
var p=(e.loaded/e.total*100).toFixed(1);
document.getElementById('barFill').style.width=p+'%';
document.getElementById('msg').textContent='上传中 '+p+'%';
}};
xhr.onload=function(){
var m=document.getElementById('msg');
if(xhr.status==200){m.textContent='升级成功，设备重启中...';}
else{m.textContent='失败: '+xhr.status+' '+xhr.responseText;btn.disabled=false;}
};
xhr.onerror=function(){document.getElementById('msg').textContent='网络错误';btn.disabled=false;};
xhr.send(f);
}
</script></body></html>
)HTML";
