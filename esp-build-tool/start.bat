@echo off
rem esp-build-tool 启动脚本（Windows）
rem 零依赖：环境自动探测，无需设置任何环境变量
rem 需要 ESP-IDF 编译时，请先在「ESP-IDF Command Prompt」里运行本脚本，
rem 或者在已 source 过 export.bat 的终端里执行，这样 idf.py 才在 PATH 中。
cd /d "%~dp0"
where node >nul 2>nul
if errorlevel 1 (
  echo [错误] 未找到 node，请先安装 Node.js 并加入 PATH。
  pause
  exit /b 1
)
node server.js
pause
