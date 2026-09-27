cmake_minimum_required(VERSION 3.20)
project(wsmud2_headless)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 内嵌 Raid 解释器脚本为 C++ 字符串（raid_interp.gen.hpp）
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/src/raid_interp.js INTERP_SRC)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/raid_interp.gen.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/raid_interp.gen.hpp @ONLY)

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
    src/quickjs/quickjs.c
)

# quickjs.c 是第三方代码，压制其编译警告
set_source_files_properties(src/quickjs/quickjs.c PROPERTIES COMPILE_OPTIONS "-w")

target_include_directories(wsmud2_headless PRIVATE ./src ${CMAKE_CURRENT_BINARY_DIR})
