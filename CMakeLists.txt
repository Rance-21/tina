cmake_minimum_required(VERSION 4.3)

# CMake 4.3 中的 import std 支持仍需开启实验开关；升级 CMake 时需检查此 ID。
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD 451f2fe2-a8a2-47c3-bc32-94786d8fc91b)
project(tina LANGUAGES CXX)

# 使用标准 C++23，不启用编译器特有的语言扩展。
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 自动查找 src/ 及其子目录中的普通源码和 C++ 模块接口。
# CONFIGURE_DEPENDS 让新增文件在下次构建时触发 CMake 重新配置。
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE APP_MODULES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.cppm")

add_executable(${PROJECT_NAME} ${APP_SOURCES})
# 允许源码直接 import std，不需要标准库头文件。
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_MODULE_STD ON)
if(APP_MODULES)
    target_sources(${PROJECT_NAME} PRIVATE FILE_SET CXX_MODULES FILES ${APP_MODULES})
endif()

# 多配置生成器也直接把可执行文件放在项目根目录。
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${PROJECT_SOURCE_DIR}>"
)

# 以后引入静态库或动态库时，在这里链接其 CMake 目标：
# target_link_libraries(${PROJECT_NAME} PRIVATE your_library)
