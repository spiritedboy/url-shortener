cmake_minimum_required(VERSION 3.12)
project(url_shortener VERSION 1.0.0 LANGUAGES CXX)

# 使用 C++11 标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译产物输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ===== 查找 pthread =====
find_package(Threads REQUIRED)

# ===== 查找 hiredis（Redis C 客户端库）=====
find_path(HIREDIS_INCLUDE_DIR
    NAMES hiredis/hiredis.h
    PATHS /usr/include /usr/local/include
)
find_library(HIREDIS_LIBRARY
    NAMES hiredis
    PATHS /usr/lib /usr/local/lib
          /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu
)
if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
    message(FATAL_ERROR "未找到 hiredis，请安装: sudo apt-get install libhiredis-dev")
endif()
message(STATUS "找到 hiredis: ${HIREDIS_LIBRARY}")

# ===== 查找 MySQL/MariaDB 客户端库 =====
find_path(MYSQL_INCLUDE_DIR
    NAMES mysql/mysql.h
    PATHS /usr/include /usr/local/include
          /usr/include/mysql /usr/local/include/mysql
          /usr/include/mariadb /usr/local/include/mariadb
)
find_library(MYSQL_LIBRARY
    NAMES mysqlclient mysqlclient_r mariadbclient
    PATHS /usr/lib /usr/local/lib
          /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu
          /usr/lib/x86_64-linux-gnu/mariadb19
    PATH_SUFFIXES mysql mariadb
)
if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
    message(FATAL_ERROR "未找到 MySQL/MariaDB 客户端库，请安装: sudo apt-get install libmysqlclient-dev 或 libmariadb-dev")
endif()
message(STATUS "找到 MySQL 客户端: ${MYSQL_LIBRARY}")

# ===== 项目源文件列表 =====
set(SOURCES
    src/main.cpp
    src/config/Config.cpp
    src/logger/Logger.cpp
    src/db/MySQLPool.cpp
    src/redis/RedisPool.cpp
    src/cache/CacheManager.cpp
    src/shortener/UrlShortener.cpp
    src/http/HttpParser.cpp
    src/server/ThreadPool.cpp
    src/server/EventLoop.cpp
    src/server/Connection.cpp
    src/server/AdminServer.cpp
    src/server/RedirectServer.cpp
    src/server/Server.cpp
)

add_executable(url_shortener ${SOURCES})

target_include_directories(url_shortener PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${HIREDIS_INCLUDE_DIR}
    ${MYSQL_INCLUDE_DIR}
)

target_link_libraries(url_shortener PRIVATE
    ${HIREDIS_LIBRARY}
    ${MYSQL_LIBRARY}
    Threads::Threads
)

# 编译选项：警告 + 优化
target_compile_options(url_shortener PRIVATE
    -Wall -Wextra -Wshadow -Wno-unused-parameter -O2
)

# ===== 安装规则 =====
install(TARGETS url_shortener DESTINATION bin)
install(FILES config.ini DESTINATION etc/url-shortener)
install(DIRECTORY frontend/ DESTINATION share/url-shortener/frontend)

# ===== 构建信息摘要 =====
message(STATUS "==================================")
message(STATUS "  url_shortener 构建配置")
message(STATUS "  C++ 标准 : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  构建类型 : ${CMAKE_BUILD_TYPE}")
message(STATUS "  hiredis  : ${HIREDIS_LIBRARY}")
message(STATUS "  MySQL    : ${MYSQL_LIBRARY}")
message(STATUS "==================================")
