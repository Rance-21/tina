cmake_minimum_required(VERSION 4.3)
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD 451f2fe2-a8a2-47c3-bc32-94786d8fc91b)
project(tina LANGUAGES CXX)

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

add_executable(${PROJECT_NAME} ${APP_SOURCES})

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
