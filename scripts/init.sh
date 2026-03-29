#!/bin/bash
# 首次部署初始化脚本
# 用途：在新服务器上检查依赖、创建目录结构、拷贝默认配置
# 运行时机：首次部署前手动执行一次

set -e

DEPLOY_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG="$DEPLOY_DIR/config.ini"

echo "=== url-shortener 初始化 ==="
echo "部署目录: $DEPLOY_DIR"
echo ""

# ---- 检查系统依赖 ----
check_cmd() {
    if ! command -v "$1" &>/dev/null; then
        echo "错误：未找到命令 '$1'，请先安装"
        exit 1
    fi
}

echo "检查系统依赖..."
check_cmd mysql
check_cmd redis-cli
echo "  依赖检查通过"

# ---- 检查 Redis 连通性 ----
echo -n "检查 Redis 连接... "
if redis-cli ping &>/dev/null; then
    echo "OK"
else
    echo "失败"
    echo "请确保 Redis 正在运行：redis-server --daemonize yes"
    exit 1
fi

# ---- 从 config.ini 读取 MySQL 连接信息并检查连通性 ----
echo -n "检查 MySQL 连接... "
if [ -f "$CONFIG" ]; then
    MYSQL_HOST=$(awk -F'=' '/^\s*host\s*=/{gsub(/\s|;.*/,"",$2); print $2; exit}' "$CONFIG")
    MYSQL_PORT=$(awk -F'=' '/^\s*port\s*=/{gsub(/\s|;.*/,"",$2); print $2; exit}' "$CONFIG")
    MYSQL_USER=$(awk -F'=' '/^\s*user\s*=/{gsub(/\s|;.*/,"",$2); print $2; exit}' "$CONFIG")
    MYSQL_PASS=$(awk -F'=' '/^\s*password\s*=/{gsub(/\s|;.*/,"",$2); print $2; exit}' "$CONFIG")
    if mysqladmin ping \
        -h "${MYSQL_HOST:-127.0.0.1}" \
        -P "${MYSQL_PORT:-3306}" \
        -u "${MYSQL_USER:-root}" \
        ${MYSQL_PASS:+-p"$MYSQL_PASS"} \
        --silent 2>/dev/null; then
        echo "OK"
    else
        echo "失败"
        echo "请检查 MySQL 是否运行以及 config.ini 中的连接配置"
        exit 1
    fi
else
    echo "未找到 config.ini，跳过 MySQL 检查"
fi

# ---- 创建目录结构 ----
echo "创建部署目录结构..."
mkdir -p "$DEPLOY_DIR/frontend"
mkdir -p "$DEPLOY_DIR/scripts"
echo "  $DEPLOY_DIR/"
echo "  $DEPLOY_DIR/frontend/"
echo "  $DEPLOY_DIR/scripts/"

# ---- 拷贝默认配置（不覆盖已有配置）----
TEMPLATE_CONFIG="$(dirname "$0")/../config.ini"
if [ ! -f "$CONFIG" ] && [ -f "$TEMPLATE_CONFIG" ]; then
    cp "$TEMPLATE_CONFIG" "$CONFIG"
    echo "已复制默认配置到 $CONFIG，请按实际环境修改后再启动服务"
elif [ -f "$CONFIG" ]; then
    echo "配置文件已存在，保持不变：$CONFIG"
fi

echo ""
echo "=== 初始化完成 ==="
echo "下一步："
echo "  1. 编辑配置文件: $CONFIG"
echo "  2. 运行 build.sh 编译并部署"
echo "  3. 启动服务: $DEPLOY_DIR/scripts/start.sh"
