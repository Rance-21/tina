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

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    find_program(TINA_LLVM_AR NAMES llvm-ar)
    find_program(TINA_LLVM_RANLIB NAMES llvm-ranlib)
    if(NOT CMAKE_CXX_COMPILER_AR AND TINA_LLVM_AR)
        set(CMAKE_CXX_COMPILER_AR "${TINA_LLVM_AR}" CACHE FILEPATH "LLVM archiver" FORCE)
    endif()
    if(NOT CMAKE_CXX_COMPILER_RANLIB AND TINA_LLVM_RANLIB)
        set(CMAKE_CXX_COMPILER_RANLIB "${TINA_LLVM_RANLIB}" CACHE FILEPATH "LLVM ranlib" FORCE)
    endif()
endif()

# 顶层统一设置编译与链接参数，确保 CMake 自动合成的 C++23 std 模块库（__cmake_cxx_std_23）
# 与业务目标共享完全一致的指令集（-march=native）与 ThinLTO 配置。
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections
        $<$<NOT:$<CONFIG:Debug>>:-O3>
        $<$<NOT:$<CONFIG:Debug>>:-march=native>
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<NOT:$<CONFIG:Debug>>>:-flto=thin>
    )
    add_link_options(
        -Wl,--gc-sections
        $<$<NOT:$<CONFIG:Debug>>:-O3>
        $<$<NOT:$<CONFIG:Debug>>:-march=native>
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<NOT:$<CONFIG:Debug>>>:-flto=thin>
    )
endif()

set(TINA_RUNTIME_OUTPUT_DIR
    "$<$<CONFIG:Debug>:${CMAKE_CURRENT_BINARY_DIR}>$<$<NOT:$<CONFIG:Debug>>:${PROJECT_SOURCE_DIR}>"
)

# Linux 线程与日志库。
find_package(Threads REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# 自动发现源码和模块。
set(APP_SOURCES "${PROJECT_SOURCE_DIR}/src/main.cppm")
set_source_files_properties(${APP_SOURCES} PROPERTIES LANGUAGE CXX)
file(GLOB_RECURSE APP_MODULE_CANDIDATES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.cppm"
)

# 只有声明了 export module 的文件才是模块接口；空文件和笔记不参与构建。
# 修改已有占位文件时也重新配置，补上模块声明后即可自动加入构建。
set(APP_MODULES)
foreach(MODULE_FILE IN LISTS APP_MODULE_CANDIDATES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MODULE_FILE}")
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug"
       AND MODULE_FILE MATCHES "/src/(ide/client_|file/|net/address\\.cppm$)")
        continue()
    endif()
    file(STRINGS "${MODULE_FILE}" MODULE_DECLARATION
        REGEX "^[ \t]*export[ \t]+module[ \t]+"
    )
    if(MODULE_DECLARATION)
        list(APPEND APP_MODULES "${MODULE_FILE}")
    endif()
endforeach()

add_executable(${PROJECT_NAME} ${APP_SOURCES})

set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_MODULE_STD ON
    RUNTIME_OUTPUT_DIRECTORY "${TINA_RUNTIME_OUTPUT_DIR}"
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


# 启用 CTest。
enable_testing()

# 测试程序自己的 main()。
add_executable(test_buffer test/buffer.cpp)

# 测试程序也使用 C++23 import std。
set_target_properties(test_buffer PROPERTIES
    CXX_MODULE_STD ON
)

# 编译测试依赖的 net.buffer 模块。
set(NET_BUFFER_MODULES
    src/net/buffer.cppm
    src/net/storage.cppm
    src/net/frame_send_queue.cppm
    src/rpc/protocol.cppm
    src/rpc/payload.cppm
)

target_sources(test_buffer
    PRIVATE
    FILE_SET CXX_MODULES
    FILES ${NET_BUFFER_MODULES}
)

# 注册测试。
add_test(NAME buffer COMMAND test_buffer)

add_executable(test_frame_send_queue test/frame_send_queue.cppm)
set_source_files_properties(test/frame_send_queue.cppm PROPERTIES LANGUAGE CXX)
set_target_properties(test_frame_send_queue PROPERTIES CXX_MODULE_STD ON)
target_sources(test_frame_send_queue
    PRIVATE
    FILE_SET CXX_MODULES
    FILES ${NET_BUFFER_MODULES}
)
add_test(NAME frame_send_queue COMMAND test_frame_send_queue)

add_executable(test_workspace_cache test/workspace_cache.cpp)
set_target_properties(test_workspace_cache PROPERTIES
    CXX_MODULE_STD ON
)
target_sources(test_workspace_cache
    PRIVATE
    FILE_SET CXX_MODULES
    FILES
        ${NET_BUFFER_MODULES}
        src/net/epoll.cppm
        src/net/event_loop.cppm
        src/runtime/log.cppm
        src/ide/protocol.cppm
        src/ide/server_protocol.cppm
        src/service/file.cppm
        src/service/workspace_cache.cppm
        src/service/workspace_cache_lru.cppm
        src/service/file_watcher.cppm
        src/ide/server_file.cppm
        src/ide/server_directory.cppm
        src/ide/server_file_read.cppm
        src/ide/server_file_write.cppm
)
target_link_libraries(test_workspace_cache PRIVATE
    Threads::Threads
    spdlog::spdlog
)
add_test(NAME workspace_cache COMMAND test_workspace_cache)

add_executable(test_spsc_queue test/spsc_queue.cpp)
set_target_properties(test_spsc_queue PROPERTIES CXX_MODULE_STD ON)
target_sources(test_spsc_queue
    PRIVATE
    FILE_SET CXX_MODULES
    FILES src/runtime/spsc_queue.cppm
)
target_link_libraries(test_spsc_queue PRIVATE Threads::Threads)
add_test(NAME spsc_queue COMMAND test_spsc_queue)

add_executable(test_worker_pool test/worker_pool.cpp)
set_target_properties(test_worker_pool PROPERTIES CXX_MODULE_STD ON)
target_sources(test_worker_pool
    PRIVATE
    FILE_SET CXX_MODULES
    FILES
        src/runtime/config.cppm
        src/runtime/cpu_affinity.cppm
        src/runtime/spsc_queue.cppm
        src/runtime/dispatcher.cppm
        src/runtime/worker_pool.cppm
        src/runtime/log.cppm
)
target_link_libraries(test_worker_pool PRIVATE Threads::Threads spdlog::spdlog)
add_test(NAME worker_pool COMMAND test_worker_pool)

add_executable(queue_microbench test/queue_microbench.cpp)
set_target_properties(queue_microbench PROPERTIES CXX_MODULE_STD ON)
target_sources(queue_microbench
    PRIVATE
    FILE_SET CXX_MODULES
    FILES
        src/runtime/config.cppm
        src/runtime/cpu_affinity.cppm
        src/runtime/spsc_queue.cppm
        src/runtime/dispatcher.cppm
        src/runtime/worker_pool.cppm
        src/runtime/log.cppm
)
target_link_libraries(queue_microbench PRIVATE Threads::Threads spdlog::spdlog)

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

add_test(
    NAME workerpool_integration
    COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/test/workerpool_integration.py"
        "$<TARGET_FILE:tina>"
)

set_tests_properties(workerpool_integration PROPERTIES
    TIMEOUT 20
    RUN_SERIAL TRUE
)

# 压测程序 tina_bench。
add_executable(tina_bench benchmark/main.cpp)

set_target_properties(tina_bench PROPERTIES
    CXX_MODULE_STD ON
    RUNTIME_OUTPUT_DIRECTORY "${TINA_RUNTIME_OUTPUT_DIR}"
)

target_sources(tina_bench
    PRIVATE
    FILE_SET CXX_MODULES
    FILES
        ${NET_BUFFER_MODULES}
        src/net/socket.cppm
        src/net/epoll.cppm
        src/ide/protocol.cppm
        src/ide/client_protocol.cppm
        benchmark/stats.cppm
        benchmark/workload.cppm
        benchmark/client.cppm
        benchmark/mixed_fixture.cppm
        benchmark/mixed_stats.cppm
        benchmark/ide_mixed.cppm
        src/service/workspace_cache.cppm
        src/service/workspace_cache_lru.cppm
)

target_link_libraries(tina_bench PRIVATE
    Threads::Threads
)

add_test(NAME benchmark_mixed
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/test/benchmark_mixed.py"
            "$<TARGET_FILE:tina>" "$<TARGET_FILE:tina_bench>")
set_tests_properties(benchmark_mixed PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

