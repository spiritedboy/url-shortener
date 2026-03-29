#!/bin/bash
# 停止 url-shortener 守护进程
# 脚本位于部署目录的 scripts/ 子目录下，自动定位上级部署目录

DEPLOY_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PID_FILE="$DEPLOY_DIR/url-shortener.pid"

if [ ! -f "$PID_FILE" ]; then
    echo "PID 文件不存在，服务可能未运行"
    # 兜底：尝试用进程名查找
    PID=$(pgrep -x url_shortener 2>/dev/null)
    if [ -n "$PID" ]; then
        echo "发现进程 PID=$PID，正在停止..."
        kill -TERM "$PID"
    fi
    exit 0
fi

PID=$(cat "$PID_FILE")

if ! kill -0 "$PID" 2>/dev/null; then
    echo "进程 PID=$PID 已不存在，清理 PID 文件"
    rm -f "$PID_FILE"
    exit 0
fi

echo "正在停止服务 (PID=$PID)..."
kill -TERM "$PID"

# 等待进程退出（最多 10 秒）
for i in $(seq 1 20); do
    sleep 0.5
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "服务已停止"
        rm -f "$PID_FILE"
        exit 0
    fi
done

echo "服务未在 10 秒内退出，强制终止..."
kill -KILL "$PID"
rm -f "$PID_FILE"
echo "服务已强制终止"
