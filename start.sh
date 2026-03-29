#!/bin/bash
# 启动 url-shortener 守护进程

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="$SCRIPT_DIR/url_shortener"
CONFIG="$SCRIPT_DIR/config.ini"
PID_FILE="$SCRIPT_DIR/url-shortener.pid"

# 检查可执行文件
if [ ! -f "$BINARY" ]; then
    echo "错误：找不到可执行文件 $BINARY"
    exit 1
fi

# 检查配置文件
if [ ! -f "$CONFIG" ]; then
    echo "错误：找不到配置文件 $CONFIG"
    exit 1
fi

# 检查是否已在运行
if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE")
    if kill -0 "$PID" 2>/dev/null; then
        echo "服务已在运行中 (PID=$PID)"
        exit 0
    else
        echo "发现残留 PID 文件（进程已不存在），清理后重启..."
        rm -f "$PID_FILE"
    fi
fi

# 切换到脚本目录，确保相对路径（日志、前端目录等）解析正确
cd "$SCRIPT_DIR" || exit 1

# 启动守护进程（默认 daemon 模式，不需要 --no-daemon）
"$BINARY" -c "$CONFIG"

# 等待进程启动并写入 PID 文件（最多等 3 秒）
for i in $(seq 1 6); do
    sleep 0.5
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE")
        if kill -0 "$PID" 2>/dev/null; then
            echo "服务已启动 (PID=$PID)"
            exit 0
        fi
    fi
done

echo "错误：服务启动失败，请检查日志文件"
exit 1
