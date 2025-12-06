cmake_minimum_required(VERSION 3.10)
project(MyWebServer)

# 1. 设置C++标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 2. 生成compile_commands.json供coc.nvim使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 3. 编译选项
# -g: 开启调试信息（GDB调试必须）
# -Wall: 开启所有警告
# -pthread: 必须，Linux下使用多线程库
# -O0: 调试阶段不优化，方便追踪变量;生产环境可改为 -O2
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -Wall -pthread -O0")

# 4. 头文件搜索路径
include_directories(${PROJECT_SOURCE_DIR})

# 5. 源文件管理
set(SOURCE_FILES
    main.cpp
    webserver.cpp
    http/http_conn.cpp
    timer/lst_timer.cpp
)

# 6. 生成可执行文件
add_executable(server ${SOURCE_FILES})

# 7.链接库
# target_link_libraries(server mysqlclient)
