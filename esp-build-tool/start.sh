#!/usr/bin/env bash
# esp-build-tool 启动脚本
# 用法：./start.sh     然后浏览器打开 http://127.0.0.1:8787
set -euo pipefail
cd "$(dirname "$0")"

# 找 Node：优先环境里的 node，其次常见位置
if ! command -v node >/dev/null 2>&1; then
  for c in /usr/local/bin/node /opt/homebrew/bin/node "$HOME/.nvm/versions/node"/*/bin/node; do
    [ -x "$c" ] && { PATH="$(dirname "$c"):$PATH"; break; }
  done
fi
command -v node >/dev/null 2>&1 || { echo "未找到 node，请先安装 Node.js"; exit 1; }

# 需要 ESP-IDF 编译时，先 source 好环境（或设置 IDF_PATH），例如：
#   source ~/esp/esp-idf/export.sh
# 也可以在 IDF 的 venv 里直接跑，本脚本不强制。

echo "启动 esp-build-tool (Ctrl+C 退出)"
exec node server.js
