cmake_minimum_required(VERSION 4.3)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD 451f2fe2-a8a2-47c3-bc32-94786d8fc91b)

# 普通 cmake -S . -B build 也沿用既有 Clang，避免省略预设时选回系统 GCC。
# 不覆盖命令行、工具链文件或已有缓存中明确指定的编译器。
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_CXX_COMPILER /usr/bin/clang++ CACHE FILEPATH "C++ compiler")
endif()
project(tina LANGUAGES CXX)

# VS Code 的 clangd 固定读取 build/；该目录不能混入 GCC 的模块映射和参数。
# 其他编译器可使用独立构建目录。
if(CMAKE_BINARY_DIR STREQUAL "${CMAKE_SOURCE_DIR}/build"
   AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "build/ is reserved for the Clang configuration used by VS Code. "
        "Current compiler: ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}). "
        "Restore it with: cmake --fresh --preset default. "
        "Use a different build directory for other compilers."
    )
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Linux 线程与日志库。
find_package(Threads REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# 自动发现源码和模块。
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE APP_MODULE_CANDIDATES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.cppm"
)

# 只有声明了 export module 的文件才是模块接口；空文件和笔记不参与构建。
# 修改已有占位文件时也重新配置，补上模块声明后即可自动加入构建。
set(APP_MODULES)
foreach(MODULE_FILE IN LISTS APP_MODULE_CANDIDATES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MODULE_FILE}")
    file(STRINGS "${MODULE_FILE}" MODULE_DECLARATION
        REGEX "^[ \t]*export[ \t]+module[ \t]+"
    )
    if(MODULE_DECLARATION)
        list(APPEND APP_MODULES "${MODULE_FILE}")
    endif()
endforeach()

add_executable(${PROJECT_NAME} ${APP_SOURCES}
    src/net/event_loop_thread_poll.cppm)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_MODULE_STD ON
)

if(APP_MODULES)
    target_sources(${PROJECT_NAME}
        PRIVATE
        FILE_SET CXX_MODULES
        FILES ${APP_MODULES}
    )
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    Threads::Threads
    spdlog::spdlog
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${PROJECT_SOURCE_DIR}>"
)


# 启用 CTest。
enable_testing()

# 测试程序自己的 main()。
add_executable(test_buffer test/buffer.cpp)

# 测试程序也使用 C++23 import std。
set_target_properties(test_buffer PROPERTIES
    CXX_MODULE_STD ON
)

# 编译测试依赖的 net.buffer 模块。
target_sources(test_buffer
    PRIVATE
    FILE_SET CXX_MODULES
    FILES src/net/buffer.cppm
)

# 注册测试。
add_test(NAME buffer COMMAND test_buffer)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME rpc_integration
    COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/test/rpc_integration.py"
        "$<TARGET_FILE:tina>"
)

set_tests_properties(rpc_integration PROPERTIES
    TIMEOUT 20
    RUN_SERIAL TRUE
)