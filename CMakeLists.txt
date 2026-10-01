cmake_minimum_required(VERSION 3.20)
project(wsmud2_headless)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 触发器多线程 worker 使用 std::thread/mutex/condition_variable，须链接 pthread
find_package(Threads REQUIRED)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 内嵌 Raid 解释器脚本为 C++ 字符串（raid_interp.gen.hpp）
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/raid_interp.js INTERP_SRC)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/raid_interp.gen.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/raid_interp.gen.hpp @ONLY)
# 源码 js 改动必须触发重配置重新生成嵌码头文件——configure_file 默认不追踪输入变化，
# 否则 raid_interp.js 修复后 cmake --build 仍会用旧的嵌入脚本（首条命令仍延迟 cmdDelay）
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/src/raid_interp.js)

add_executable(wsmud2_headless
    main.cpp
    commands.cpp
    tui_ui.cpp
    src/json.cpp
    src/net.cpp
    src/ws.cpp
    src/mud.cpp
    src/tui.cpp
    src/trigger.cpp
    src/color.cpp
    src/quickjs/quickjs.c
)

# quickjs.c 是第三方代码，压制其编译警告
set_source_files_properties(src/quickjs/quickjs.c PROPERTIES COMPILE_OPTIONS "-w")

target_include_directories(wsmud2_headless PRIVATE ./src ${CMAKE_CURRENT_BINARY_DIR})

# -pthread：std::thread 链接（Linux/WSL 必需）
target_link_libraries(wsmud2_headless PRIVATE Threads::Threads)
