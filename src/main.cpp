#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string>
#include <stdexcept>

#include "config/Config.h"
#include "logger/Logger.h"
#include "db/MySQLPool.h"
#include "redis/RedisPool.h"
#include "cache/CacheManager.h"
#include "shortener/UrlShortener.h"
#include "server/Server.h"

// ============================================================
//  全局停止标志（信号处理器和主循环共用）
// ============================================================
static volatile sig_atomic_t g_running = 1;

// 信号处理函数（处理 SIGTERM / SIGINT 优雅退出）
static void signalHandler(int sig) {
    (void)sig;
    g_running = 0;
}

// ============================================================
//  守护进程化（双 fork + 关闭标准 I/O）
// ============================================================
static void daemonize() {
    // 第一次 fork：父进程退出，子进程脱离终端前台进程组
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork() 第一次失败");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        // 父进程退出
        exit(EXIT_SUCCESS);
    }

    // 创建新会话，成为会话领导，脱离控制终端
    if (setsid() < 0) {
        perror("setsid() 失败");
        exit(EXIT_FAILURE);
    }

    // 第二次 fork：防止重新获得控制终端
    pid = fork();
    if (pid < 0) {
        perror("fork() 第二次失败");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        // 中间进程退出
        exit(EXIT_SUCCESS);
    }

    // 设置文件创建掩码，确保守护进程创建的文件有正确权限
    umask(0022);

    // 将工作目录切换到根目录，避免占用挂载点
    // 注意：日志路径使用绝对路径时才有意义，相对路径建议在切换前解析
    // 此处使用可执行文件所在目录（由调用方设定），不切换到 /
    // chdir("/");  // 如果配置文件使用绝对路径，可以取消此注释

    // 将标准输入/输出/错误重定向到 /dev/null（守护进程不使用终端）
    int devNull = open("/dev/null", O_RDWR);
    if (devNull >= 0) {
        dup2(devNull, STDIN_FILENO);
        dup2(devNull, STDOUT_FILENO);
        dup2(devNull, STDERR_FILENO);
        close(devNull);
    }
}

// ============================================================
//  主函数
// ============================================================
int main(int argc, char* argv[]) {
    // ---- 解析命令行参数 ----
    std::string configPath = "./config.ini";
    bool        runAsDaemon = true;  // 默认以守护进程方式运行

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--no-daemon" || arg == "-D") {
            // 调试模式：不守护进程化，方便查看输出
            runAsDaemon = false;
        } else if (arg == "--help" || arg == "-h") {
            printf("用法: %s [-c config.ini] [--no-daemon]\n", argv[0]);
            printf("  -c / --config  <path>  指定配置文件路径（默认: ./config.ini）\n");
            printf("  -D / --no-daemon       不以守护进程方式运行（调试用）\n");
            return 0;
        }
    }

    // ---- 加载配置文件 ----
    if (!Config::instance().load(configPath)) {
        fprintf(stderr, "[main] 无法加载配置文件: %s\n", configPath.c_str());
        return EXIT_FAILURE;
    }

    // ---- 初始化日志（守护进程化前完成，以便捕获初始化错误）----
    std::string logFile  = Config::instance().get("log", "file",  "./url-shortener.log");
    std::string logLevel = Config::instance().get("log", "level", "info");

    LogLevel level = LogLevel::INFO;
    if      (logLevel == "debug") level = LogLevel::DEBUG;
    else if (logLevel == "warn")  level = LogLevel::WARN;
    else if (logLevel == "error") level = LogLevel::ERROR;

    if (!Logger::instance().init(logFile, level)) {
        fprintf(stderr, "[main] 日志初始化失败，日志路径: %s\n", logFile.c_str());
        return EXIT_FAILURE;
    }

    LOG_INFO("==================================================");
    LOG_INFO("  url-shortener 服务启动");
    LOG_INFO("  配置文件: " + configPath);
    LOG_INFO("  日志文件: " + logFile);
    LOG_INFO("==================================================");

    // ---- 守护进程化 ----
    if (runAsDaemon) {
        LOG_INFO("以守护进程模式启动...");
        // fork() 后子进程中只有调用线程存在，日志后台写线程会消失
        // 必须在 daemonize 之前停止日志线程，之后重新初始化
        Logger::instance().stop();
        daemonize();
        // daemon 子进程中重新初始化日志（启动新的后台写线程）
        if (!Logger::instance().init(logFile, level)) {
            // 日志初始化失败仅警告，程序继续运行
        }
        LOG_INFO("守护进程化完成，PID=" + std::to_string(getpid()));
    } else {
        LOG_INFO("以前台模式启动（调试），PID=" + std::to_string(getpid()));
    }

    // ---- 注册信号处理器 ----
    struct sigaction sa{};
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);  // kill 命令默认发送 SIGTERM
    sigaction(SIGINT,  &sa, nullptr);  // Ctrl+C
    signal(SIGPIPE, SIG_IGN);          // 忽略 SIGPIPE（写入已关闭连接时）

    // ---- 初始化 MySQL 连接池 ----
    if (!MySQLPool::instance().init()) {
        LOG_ERROR("MySQL 连接池初始化失败，程序退出");
        return EXIT_FAILURE;
    }

    // ---- 初始化 Redis 连接池 ----
    if (!RedisPool::instance().init()) {
        LOG_ERROR("Redis 连接池初始化失败，程序退出");
        return EXIT_FAILURE;
    }

    // ---- 初始化三级缓存管理器 ----
    CacheManager::instance().init();

    // ---- 初始化短链转换器 ----
    UrlShortener::instance().init();

    // ---- 启动服务器（非阻塞，两个 EventLoop 均在独立线程中运行）----
    try {
        Server server;
        LOG_INFO("服务器初始化完成，开始监听");

        server.start();

        // 主线程等待退出信号（SIGTERM / SIGINT）
        while (g_running) {
            sleep(1);
        }

        LOG_INFO("收到退出信号，正在停止服务器...");
        server.stop();
        LOG_INFO("服务器正常退出");

    } catch (const std::exception& e) {
        LOG_ERROR(std::string("服务器运行异常: ") + e.what());
        return EXIT_FAILURE;
    }

    // ---- 清理资源 ----
    MySQLPool::instance().destroy();
    RedisPool::instance().destroy();
    Logger::instance().stop();

    return EXIT_SUCCESS;
}
