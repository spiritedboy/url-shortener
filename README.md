# 短链接转换系统（URL Shortener）

> 基于 C++11 + Reactor + epoll ET + 三级缓存的高性能短链接服务

---

## 目录

- [功能特性](#功能特性)
- [系统架构](#系统架构)
- [设计思路](#设计思路)
  - [短链生成算法](#短链生成算法)
  - [三级缓存架构](#三级缓存架构)
  - [网络模型](#网络模型)
  - [守护进程](#守护进程)
  - [日志系统](#日志系统)
- [目录结构](#目录结构)
- [依赖库](#依赖库)
- [编译与安装](#编译与安装)
- [配置说明](#配置说明)
- [运行](#运行)
- [API 文档](#api-文档)
- [前端界面](#前端界面)
- [压力测试](#压力测试)
- [常见问题](#常见问题)

---

## 功能特性

| 特性 | 说明 |
|------|------|
| 短链生成 | MurmurHash3 + Base62 编码，相同 URL 始终返回相同短码 |
| 三级缓存 | 内存 LRU → Redis → MySQL，读取极低延迟 |
| 双端口 | 管理接口（前端+API）与重定向接口分离 |
| 高并发 | Reactor + epoll ET + EPOLLONESHOT + pthread 线程池 |
| 分页查询 | `/api/links` 支持 `page` / `page_size` 参数，避免大数据集一次性传输 |
| 守护进程 | 双 fork 实现，可后台稳定运行 |
| 日志 | 异步队列写文件，不影响主线程性能；守护模式下 fork 前后正确重启后台线程 |
| 超时保护 | MySQL 连接/读/写均设置 30 秒超时，防止网络异常导致工作线程永久阻塞 |
| 连接池自动重连 | MySQL / Redis 每次借出连接前做存活检测（mysql_ping / PING），断连自动重建，服务器重启后即刻恢复；连接不可用时抛异常防止空指针崩溃 |
| 安全 | SQL 注入防护（mysql_real_escape_string）、XSS 防护（前端 HTML 转义）、DELETE 短码 Base62 校验（防路径穿越）、Redis 命令二进制安全（%b 格式符）、URL 长度限制 8KB |
| 缓存穿透防护 | 不存在的短码缓存空值哨兵（Redis 120s TTL），避免无效请求反复穿透到 MySQL |
| 资源保护 | 最大并发连接数 10000、HTTP 请求头上限 100 个、读缓冲区上限 8MB、写失败自动重试 |

---

## 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                        客户端 / 浏览器                         │
└────────────┬────────────────────────┬───────────────────────┘
             │ HTTP :8080              │ HTTP :8000
             ▼                         ▼
┌────────────────────┐    ┌────────────────────────────────────┐
│   Admin EventLoop  │    │        Redirect EventLoop          │
│   (管理接口 + SPA) │    │     (GET /{code} → 302 跳转)       │
└────────┬───────────┘    └──────────────┬─────────────────────┘
         │  epoll ET + EPOLLONESHOT       │
         └──────────┬────────────────────┘
                    ▼
         ┌─────────────────────┐
         │    ThreadPool       │
         │   (N个pthread线程)   │
         └──────────┬──────────┘
                    ▼
         ┌─────────────────────┐
         │     UrlShortener    │  ← 短链转换逻辑
         └──────────┬──────────┘
                    ▼
         ┌─────────────────────┐
         │    CacheManager     │  ← 三级缓存门面
         ├──────────┬──────────┤
         │ L1:内存LRU│ L2:Redis │
         │  (10000条)│ (TTL缓存)│
         └──────────┴────┬─────┘
                         │
              ┌──────────▼──────────┐
              │   L3: MySQL 持久化   │
              │  url_mappings 表     │
              └─────────────────────┘
```

---

## 设计思路

### 短链生成算法

**核心原则：确定性 + 低碰撞率**

```
输入 URL
  │
  ▼ 规范化（可选：忽略 ? 后内容）
  │
  ▼ MurmurHash3-32（哈希值 h32）
  │
  ▼ 拓展到 64 位（h64 = h32 << 32 | ~h32）
  │
  ▼ Base62 编码（字符集 0-9A-Za-z），取前 N 位（默认 6 位）
  │
  ▼ 碰撞检测：查缓存，若短码已被不同 URL 占用
  │             → 在输入后追加 \x00 + 碰撞计数器，重新哈希
  │
  ▼ 写入三级缓存，返回短码
```

- **相同 URL 返回相同短码**：规范化输入一致，哈希函数确定性，结果固定
- **忽略查询参数**：配置 `ignore_query_string = true` 后，`https://a.com/p?x=1` 与 `https://a.com/p?y=2` 生成相同短码
- **碰撞处理**：哈希空间为 62^6 ≈ 568 亿，冲突概率极低；即使发生也通过追加计数器重试

### 三级缓存架构

```
查询 code → 长 URL：

L1 内存 LRU（命中率最高，速度最快）
  ├─ 命中 → 返回，完成
  └─ 未命中 ↓

L2 Redis（通过连接池访问，毫秒级）
  ├─ 命中 → 回填 L1，返回
  └─ 未命中 ↓

L3 MySQL（持久化存储，永不丢失）
  ├─ 命中 → 回填 L1+L2，返回
  └─ 未命中 → 返回空（不存在）
```

| 层级 | 实现 | 淘汰策略 | 特点 |
|------|------|---------|------|
| L1 内存 | `unordered_map` + `std::list` | LRU O(1) | 最快，但重启丢失 |
| L2 Redis | hiredis + 连接池 | Key TTL（可配置）| 毫秒级，跨进程共享 |
| L3 MySQL | mysqlclient + 连接池 | 无淘汰 | 持久化，不丢失 |

**连接池自动重连**：
MySQL 和 Redis 连接池在每次 `acquire()` 借出连接时发送存活探测（MySQL `mysql_ping()`、Redis `PING`），检测失败则自动销毁旧连接并重建新连接。无论是长时间空闲断开还是服务器意外重启，借出的连接始终可用。

**缓存穿透防护**：
当三级缓存全部未命中（短码不存在）时，在 Redis 中写入空值哨兵 `__NULL__`（TTL 120秒）。后续相同短码的请求在 L2 层直接命中哨兵并返回 404，不再穿透到 MySQL。当新创建的短码与哨兵冲突时，自动清除哨兵并写入真实值。

**LRU 数据结构设计**：
- `std::list<pair<K,V>>`：双向链表，头部为最近访问，尾部为最久未访问
- `std::unordered_map<K, list::iterator>`：O(1) 查找链表节点
- `list::splice()`：O(1) 将节点移至链表头部

### 网络模型

**Reactor + epoll ET + EPOLLONESHOT + pthread 线程池**

```
主线程（EventLoop）：
  epoll_wait → 监听 serverFd + 连接 fd
     │
     ├── serverFd 可读 → acceptAll()（循环 accept 至 EAGAIN）
     │                    └─ 新连接 setNonBlocking
     │                           + epoll_ctl(ADD, EPOLLIN|ET|ONESHOT)
     │
     └── 连接 fd 可读 → 从 connections_ 取出 Connection
                          └─ 提交到 ThreadPool

工作线程（ThreadPool 中的 pthread）：
  执行 handler(conn) →
    1. conn->readAll()        （循环 read 至 EAGAIN，ET 模式必须读完）
    2. HttpParser::parse()    （解析 HTTP 请求）
    3. 路由分发（AdminServer / RedirectServer）
    4. conn->sendAll()        （循环 write 直到发完）
    5. EventLoop::closeConnection() 或 rearmConnection()
```

**为什么使用 EPOLLONESHOT？**  
保证同一个 fd 在同一时刻只会被一个工作线程处理，彻底避免并发访问同一连接的竞态条件。工作线程处理完毕后，通过 `epoll_ctl(MOD, ...)` 重新启用监听（keep-alive）或关闭连接。

**为什么使用 ET 模式？**  
边沿触发只在状态"变化"时通知，减少无效唤醒次数。代价是必须循环读/写直到 `EAGAIN`，本项目在 `readAll()` 和 `sendAll()` 中严格实现了这一点。

### 守护进程

采用经典双 fork 方案：

```cpp
fork() → 父进程退出（脱离前台进程组）
  └─ setsid()  → 创建新会话，成为会话领导
       └─ fork() → 中间进程退出（防止重新获得控制终端）
            └─ umask(0022)
               将 stdin/stdout/stderr 重定向到 /dev/null
               写入 PID 文件（可选）
               正式启动服务
```

调试时可使用 `--no-daemon` 参数以前台模式运行。

### 日志系统

- **异步队列**：主线程将日志字符串 `push` 到 `std::queue`，不阻塞
- **后台线程**：专属 pthread 批量 `pop` 并写入文件（`flush` 刷写）
- **格式**：`[2026-03-23 12:00:00] [INFO ] 消息内容`
- **级别过滤**：低于配置级别的日志直接丢弃，零开销

---

## 目录结构

```
url-shortener/
├── CMakeLists.txt          # CMake 构建配置
├── build.sh                # 编译 + 部署一键脚本
├── config.ini              # 配置文件模板
├── README.md               # 本文档
├── frontend/
│   └── index.html          # 前端单页应用（SPA）
├── scripts/
│   ├── init.sql            # 数据库初始化 SQL
│   ├── start.sh            # 启动守护进程
│   └── stop.sh             # 停止守护进程
└── src/
    ├── main.cpp             # 入口：守护进程 + 初始化 + 启动
    ├── config/
    │   ├── Config.h         # INI 配置解析器（单例）
    │   └── Config.cpp
    ├── logger/
    │   ├── Logger.h         # 异步文件日志（单例）
    │   └── Logger.cpp
    ├── utils/
    │   ├── Hash.h           # MurmurHash3-32（纯头文件）
    │   └── Base62.h         # Base62 编解码（纯头文件）
    ├── db/
    │   ├── MySQLPool.h      # MySQL RAII 连接池（单例）
    │   └── MySQLPool.cpp
    ├── redis/
    │   ├── RedisPool.h      # Redis RAII 连接池（单例）
    │   └── RedisPool.cpp
    ├── cache/
    │   ├── LRUCache.h       # 模板 LRU 缓存（纯头文件）
    │   ├── CacheManager.h   # 三级缓存管理器（单例）
    │   └── CacheManager.cpp
    ├── shortener/
    │   ├── UrlShortener.h   # URL 转换器（单例）
    │   └── UrlShortener.cpp
    ├── http/
    │   ├── HttpParser.h     # HTTP 请求解析器
    │   ├── HttpParser.cpp
    │   └── HttpResponse.h   # HTTP 响应构建（纯头文件）
    └── server/
        ├── ThreadPool.h     # pthread 线程池
        ├── ThreadPool.cpp
        ├── EventLoop.h      # epoll ET 事件循环（Reactor）
        ├── EventLoop.cpp
        ├── Connection.h     # 连接读写缓冲
        ├── Connection.cpp
        ├── AdminServer.h    # 管理接口（REST API + 静态文件）
        ├── AdminServer.cpp
        ├── RedirectServer.h # 重定向接口
        ├── RedirectServer.cpp
        ├── Server.h         # 服务器主控（双端口启动）
        └── Server.cpp
```

---

## 依赖库

| 库 | 用途 | 安装命令（Ubuntu/Debian）|
|----|------|-------------------------|
| libhiredis-dev | Redis C 客户端 | `sudo apt-get install libhiredis-dev` |
| libmysqlclient-dev | MySQL C 客户端 | `sudo apt-get install libmysqlclient-dev` |
| pthread | POSIX 线程 | 系统自带 |
| cmake ≥ 3.12 | 构建工具 | `sudo apt-get install cmake` |
| g++ ≥ 5.0（C++11）| 编译器 | `sudo apt-get install g++` |

**一键安装所有依赖（Ubuntu）：**
```bash
sudo apt-get update
sudo apt-get install -y cmake g++ libhiredis-dev libmysqlclient-dev
```

---

## 编译与安装

### 方式一：使用 build.sh（推荐）

```bash
# 克隆项目
git clone git@github.com:spiritedboy/url-shortener.git
cd url-shortener

# 编译并部署到指定目录（默认 /home/opt）
bash build.sh /home/opt
```

`build.sh` 会自动完成以下工作：
1. 运行 CMake 配置和编译
2. 将可执行文件、前端资源、管理脚本拷贝到部署目录
3. 首次部署时自动拷贝默认 `config.ini`（已有则不覆盖）

### 方式二：手动编译

```bash
# 1. 进入项目根目录
cd url-shortener

# 2. 创建并进入构建目录
mkdir -p build && cd build

# 3. 生成 Makefile（Release 模式）
cmake .. -DCMAKE_BUILD_TYPE=Release

# 4. 编译（使用所有 CPU 核心加速）
cmake --build . -- -j$(nproc)

# 5. 可执行文件位于
ls bin/url_shortener
```

手动将文件拷贝到部署目录：
```bash
DEPLOY_DIR=/home/opt
mkdir -p $DEPLOY_DIR/frontend $DEPLOY_DIR/scripts
cp build/bin/url_shortener $DEPLOY_DIR/
cp -rT frontend $DEPLOY_DIR/frontend
cp scripts/start.sh scripts/stop.sh scripts/init.sql $DEPLOY_DIR/scripts/
chmod +x $DEPLOY_DIR/scripts/*.sh
cp config.ini $DEPLOY_DIR/   # 初次部署
```

---

## 配置说明

编辑 `config.ini` 文件：

```ini
[server]
admin_port = 8080          ; 管理接口端口（前端页面 + REST API）
redirect_port = 8000       ; 短链跳转端口（80 需要 root 权限）
worker_threads = 4         ; 工作线程数（建议 = CPU 核数 × 2）
frontend_dir = ./frontend  ; 前端 HTML 所在目录（相对于可执行文件）
redirect_base_url = http://127.0.0.1:8000 ; 跳转服务对外访问地址（前端展示短链用，末尾不加斜杠）

[shortener]
code_length = 6             ; 短码长度（6 位 = 62^6 ≈ 568 亿种组合）
ignore_query_string = false ; true: 忽略 URL 中 ? 后的内容

[memory_cache]
max_size = 10000            ; 内存 LRU 最大条目数

[redis]
host = 127.0.0.1
port = 6379
password =                  ; 留空则不认证
pool_size = 8               ; Redis 连接池大小
ttl = 3600                  ; 缓存 TTL（秒），0 = 不过期

[mysql]
host = 127.0.0.1
port = 3306
user = root
password =
database = url_shortener    ; 数据库名（不存在会自动创建）
pool_size = 8

[log]
file = ./url-shortener.log  ; 日志文件路径
level = info                ; debug / info / warn / error
```

---

## 运行

### 使用管理脚本（推荐）

部署目录（默认 `/home/opt`）下的 `scripts/` 子目录提供三个脚本：

| 脚本 | 用途 |
|------|------|
| `scripts/init.sql` | 数据库初始化 SQL，首次部署时导入 MySQL |
| `scripts/start.sh` | 启动守护进程，检测重复运行，等待 PID 文件确认启动成功 |
| `scripts/stop.sh` | 发送 SIGTERM 优雅退出，超时后 SIGKILL 强制终止 |

```bash
cd /home/opt

# 首次部署：初始化数据库
mysql -u root -p < scripts/init.sql

# 编辑配置文件（数据库、端口等）
vim config.ini

# 启动服务
bash scripts/start.sh

# 停止服务
bash scripts/stop.sh
```

### 直接运行可执行文件

```bash
# 以守护进程模式启动（默认）
cd /home/opt
./url_shortener -c config.ini

# 以前台模式启动（调试）
./url_shortener -c config.ini --no-daemon
```

**监听 80 端口**（需要 root 权限）：
```bash
# 方式一：直接以 root 运行（不推荐）
sudo ./url_shortener

# 方式二：使用 setcap 赋予绑定 1024 以下端口的权限
sudo setcap 'cap_net_bind_service=+ep' ./url_shortener
./url_shortener   # 普通用户运行即可绑定 80 端口
```

### systemd 服务配置（推荐生产环境）

> [!WARNING]
> 使用 systemd 托管时，必须以 `--no-daemon` 参数前台运行。若使用守护进程模式（默认），程序会 fork 后父进程退出，systemd 误判服务已停止并发送 SIGTERM，导致子进程立即退出。

创建 `/etc/systemd/system/url-shortener.service`：

```ini
[Unit]
Description=URL Shortener Service
After=network.target mysql.service redis.service

[Service]
Type=simple
WorkingDirectory=/opt/url-shortener
ExecStart=/opt/url-shortener/url_shortener -c /opt/url-shortener/config.ini --no-daemon
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable url-shortener
sudo systemctl start url-shortener
sudo systemctl status url-shortener
```

---

## API 文档

所有 API 均由管理接口（`admin_port`）提供，支持 CORS。

### GET `/`
返回前端 HTML 页面。

---

### GET `/api/links`
获取短链映射列表，支持分页。

**查询参数**：

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `page` | int | 1 | 页码（从 1 开始）|
| `page_size` | int | 20 | 每页条数（最大 200）|

**示例**：`GET /api/links?page=2&page_size=20`

**响应示例**：
```json
{
  "success": true,
  "total": 10003,
  "page": 2,
  "page_size": 20,
  "data": [
    { "code": "abc123", "url": "https://www.example.com/very/long/path" },
    { "code": "xyz789", "url": "https://another.example.com/" }
  ]
}
```

- `total`：数据库中的总记录数
- `data`：当前页的数据列表

---

### POST `/api/links`
创建短链接。

**请求体**：
```json
{ "url": "https://www.example.com/very/long/path" }
```

**响应示例**：
```json
{
  "success": true,
  "data": {
    "code": "abc123",
    "url": "https://www.example.com/very/long/path",
    "short_url": "http://localhost:8000/abc123"
  }
}
```

**幂等性**：相同的 URL 重复调用返回相同的 `code`。

---

### DELETE `/api/links/{code}`
删除指定短码。

**示例**：`DELETE /api/links/abc123`

**响应**：
```json
{ "success": true }
```

---

### GET `/{code}`（重定向端口）
根据短码跳转到原始 URL，返回 `302 Found`。

```
HTTP/1.1 302 Found
Location: https://www.example.com/very/long/path
```

短码不存在时返回 `404 Not Found`。

---

## 前端界面

访问 `http://localhost:8080` 即可进入管理界面：

- **创建**：在输入框中粘贴长 URL，点击"生成短链"即得短码
- **列表**：分页展示所有短链（每页 20 条），支持当前页实时搜索过滤
- **分页**：底部显示页码控件，支持跳页与上下翻页，总数量实时显示
- **复制**：点击表格中的 📋 按钮可一键复制完整短链 URL（兼容非 HTTPS 环境）
- **删除**：点击"删除"按钮并确认后移除映射，自动刷新当前页
- **跳转**：点击短码可直接在新标签页打开（验证跳转功能）

---

## 压力测试

### 安装工具

```bash
# wrk（推荐，支持 Lua 脚本定制请求）
sudo apt-get install wrk

# 或 ab（Apache Bench，简单直接）
sudo apt-get install apache2-utils
```

### 压测重定向接口（核心热路径）

先创建一条短链，获取 code：
```bash
curl -s http://localhost:8080/api/links \
  -X POST -H "Content-Type: application/json" \
  -d '{"url":"https://www.github.com"}' | grep -o '"code":"[^"]*"'
```

假设拿到 `abc123`，压测跳转（8000 端口）：
```bash
# wrk：12 线程 / 200 并发 / 持续 30 秒
wrk -t12 -c200 -d30s http://localhost:8000/abc123

# ab：10000 次请求 / 200 并发
ab -n 10000 -c 200 http://localhost:8000/abc123
```

### 压测创建短链接口（写入路径）

创建 Lua 脚本让每次提交不同 URL：
```lua
-- post.lua
wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"
counter = 0
function request()
  counter = counter + 1
  wrk.body = '{"url":"https://www.example.com/page/' .. counter .. '"}'
  return wrk.format()
end
```

```bash
wrk -t4 -c50 -d30s -s post.lua http://localhost:8080/api/links
```

### 压测分页查询接口

```bash
wrk -t8 -c100 -d30s "http://localhost:8080/api/links?page=1&page_size=20"
```

### 结果指标说明

| 指标 | 说明 |
|------|------|
| `Req/Sec` | 每秒请求数（QPS） |
| `Latency avg` | 平均响应延迟 |
| `Latency 99%` | P99 延迟（99% 请求在此时间内完成） |
| `Non-2xx` | 非成功响应数，不为 0 则说明有错误 |

### 压测时监控服务状态

```bash
# 实时 CPU / 内存占用
watch -n1 'ps aux | grep url_shortener'

# TCP 连接数统计
watch -n1 'ss -s'

# 实时日志
tail -F ./url-shortener.log
```

---

## 常见问题

**Q: 绑定 80 端口提示"Permission denied"?**  
A: 需要 root 权限或使用 `setcap` 授权，参见[运行](#运行)章节。

**Q: Redis 连接失败，服务能正常运行吗？**  
A: 不能。Redis 连接池初始化失败会导致服务退出。请确保 Redis 正在运行：`redis-server --daemonize yes`

**Q: MySQL 数据库不存在会怎样？**  
A: 服务启动时会自动创建数据库 `url_shortener` 和数据表 `url_mappings`，无需手动建库。

**Q: 如何调整短码长度？**  
A: 修改 `config.ini` 中的 `code_length`，重启服务生效。注意已有记录不受影响。

**Q: 日志文件在哪里？**  
A: 由 `config.ini` 的 `[log] file` 配置项决定，默认为 `./url-shortener.log`（可执行文件同目录）。

**Q: 守护进程崩溃如何排查？**  
A: 查看日志文件中的 ERROR 级别条目，或使用 `--no-daemon` 参数以前台模式运行查看输出。

---

## 许可证

MIT License

<!-- 本项目持续迭代中，欢迎提交 Issue 和 PR。 -->
