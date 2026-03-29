#!/bin/bash
# 编译并部署 url-shortener
# 用法: ./build.sh [部署目录]
#   部署目录默认为 /home/opt
#
# 功能：
#   1. 运行 cmake 配置 + 编译
#   2. 将可执行文件、前端资源、管理脚本拷贝到部署目录
#   3. 首次部署时拷贝默认 config.ini（不覆盖已有配置）

set -e

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPLOY_DIR="${1:-/home/opt}"

echo "=== url-shortener 编译部署 ==="
echo "项目目录: $PROJECT_DIR"
echo "部署目录: $DEPLOY_DIR"
echo ""

# ---- 编译 ----
echo "配置 CMake..."
cmake -S "$PROJECT_DIR" -B "$PROJECT_DIR/build" -DCMAKE_BUILD_TYPE=Release

echo "开始编译..."
cmake --build "$PROJECT_DIR/build" -- -j$(nproc)
echo "编译完成: $PROJECT_DIR/build/bin/url_shortener"
echo ""

# ---- 创建部署目录 ----
mkdir -p "$DEPLOY_DIR/frontend"
mkdir -p "$DEPLOY_DIR/scripts"

# ---- 拷贝可执行文件 ----
cp "$PROJECT_DIR/build/bin/url_shortener" "$DEPLOY_DIR/"
echo "已部署: url_shortener -> $DEPLOY_DIR/"

# ---- 拷贝前端资源 ----
cp -rT "$PROJECT_DIR/frontend" "$DEPLOY_DIR/frontend"
echo "已部署: frontend/ -> $DEPLOY_DIR/frontend/"

# ---- 拷贝管理脚本 ----
cp "$PROJECT_DIR/scripts/start.sh" \
   "$PROJECT_DIR/scripts/stop.sh"  \
   "$PROJECT_DIR/scripts/init.sh"  \
   "$DEPLOY_DIR/scripts/"
chmod +x "$DEPLOY_DIR/scripts/"*.sh
echo "已部署: scripts/ -> $DEPLOY_DIR/scripts/"

# ---- 拷贝默认配置（不覆盖已有配置）----
if [ ! -f "$DEPLOY_DIR/config.ini" ]; then
    cp "$PROJECT_DIR/config.ini" "$DEPLOY_DIR/"
    echo "已部署: config.ini -> $DEPLOY_DIR/  （首次部署，请按需修改）"
else
    echo "配置文件已存在，保持不变: $DEPLOY_DIR/config.ini"
fi

echo ""
echo "=== 部署完成 ==="
echo "  启动服务: $DEPLOY_DIR/scripts/start.sh"
echo "  停止服务: $DEPLOY_DIR/scripts/stop.sh"
