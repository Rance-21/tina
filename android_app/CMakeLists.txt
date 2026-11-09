cmake_minimum_required(VERSION 3.24)

project(tina_android_client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Android 和桌面预览共用本地地址配置；真实域名不写入可提交的源码。
# include 会让 CMake 跟踪此文件，修改地址后再次构建即可自动重新生成配置头。
set(tina_client_config "${CMAKE_CURRENT_SOURCE_DIR}/client.local.cmake")
if(NOT EXISTS "${tina_client_config}")
    message(FATAL_ERROR
        "缺少客户端本地配置，请复制 android_app/client.local.cmake.example 为 "
        "android_app/client.local.cmake，再填写服务端地址和端口。")
endif()
include("${tina_client_config}")
if(NOT "${TINA_SERVER_HOST}" MATCHES "^[A-Za-z0-9_.:-]+$")
    message(FATAL_ERROR "TINA_SERVER_HOST 必须是域名或 IP 地址，不要填写协议或端口。")
endif()
if(NOT "${TINA_SERVER_PORT}" MATCHES "^[1-9][0-9]*$"
   OR TINA_SERVER_PORT LESS 1 OR TINA_SERVER_PORT GREATER 65535)
    message(FATAL_ERROR "TINA_SERVER_PORT 必须是 1 到 65535 之间的整数。")
endif()
configure_file(src/client_config.hpp.in generated/client_config.hpp @ONLY)
get_filename_component(tina_project_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network)

qt_standard_project_setup(REQUIRES 6.5)
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.8)
    qt_policy(SET QTP0004 NEW)
endif()

qt_add_executable(tina_ide_client
    MANUAL_FINALIZATION
    src/main.cpp
    src/ffi_client_bridge.hpp
    src/ffi_client_bridge.cpp
    src/workspace_document.hpp
    src/workspace_document.cpp
    src/code_highlighter.hpp
    src/code_highlighter.cpp
)

target_include_directories(tina_ide_client PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

# 桌面和 APK 直接打包仓库里的同一份背景图。
set(wallpaper "${CMAKE_CURRENT_SOURCE_DIR}/../background.jpg")
set_source_files_properties("${wallpaper}" PROPERTIES QT_RESOURCE_ALIAS assets/background.jpg)

qt_add_qml_module(tina_ide_client
    URI TinaIde
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/GlassPanel.qml
        qml/CodePane.qml
        qml/UiIcon.qml
    RESOURCES
        "${wallpaper}"
)

set_target_properties(tina_ide_client PROPERTIES
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

if(ANDROID)
    # 工作区 RPC 需要网络权限；使用 Qt Android arm64-v8a Kit 构建平板版本。
    set_property(TARGET tina_ide_client APPEND PROPERTY
        QT_ANDROID_PERMISSIONS android.permission.INTERNET)
endif()

target_link_libraries(tina_ide_client PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
)

# 等 Qt 收集完 QML 和 Android 依赖再完成目标，随后挂接安装包输出动作。
function(tina_finalize_client)
    qt_finalize_executable(tina_ide_client)
    if(NOT ANDROID)
        return()
    endif()
    # Qt 的中间文件仍留在构建目录；每次标准 APK 构建完成后自动更新根目录安装包。
    # 同时兼容 Qt 单应用和按目标划分 Android 构建目录的设置。
    if(QT_USE_TARGET_ANDROID_BUILD_DIR)
        set(tina_apk_dir "${CMAKE_CURRENT_BINARY_DIR}/android-build-tina_ide_client")
    else()
        set(tina_apk_dir "${CMAKE_CURRENT_BINARY_DIR}/android-build")
    endif()
    add_custom_command(TARGET tina_ide_client_make_apk POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${tina_apk_dir}/tina_ide_client.apk"
            "${tina_project_root}/tina_ide_client.apk"
        BYPRODUCTS "${tina_project_root}/tina_ide_client.apk"
        COMMENT "将最新 APK 输出到项目根目录"
        VERBATIM
    )
endfunction()
cmake_language(DEFER CALL tina_finalize_client)

if(NOT ANDROID)
    # 只调整 PC 客户端输出，不影响 Android 原生库和测试程序的位置。
    # 生成器表达式可避免多配置构建器额外创建 Debug / Release 子目录。
    set_target_properties(tina_ide_client PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<1:${tina_project_root}>"
    )
endif()

# 模拟服务端测试只在桌面显式开启，Android APK 不携带测试依赖。
option(TINA_BUILD_TESTS "构建工作区通信与键盘交互测试" OFF)
if(TINA_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(tina_workspace_test
        tests/workspace_test.cpp
        src/ffi_client_bridge.cpp src/ffi_client_bridge.hpp
        src/workspace_document.cpp src/workspace_document.hpp
        src/code_highlighter.cpp src/code_highlighter.hpp
    )
    target_include_directories(tina_workspace_test PRIVATE src)
    target_compile_definitions(tina_workspace_test PRIVATE
        TINA_QML_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
    target_link_libraries(tina_workspace_test PRIVATE
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::Test)
    qt_add_resources(tina_workspace_test test_wallpaper
        PREFIX "/qt/qml/TinaIde" FILES "${wallpaper}"
        qml/Main.qml qml/CodePane.qml qml/GlassPanel.qml qml/UiIcon.qml)
    add_test(NAME workspace COMMAND tina_workspace_test)
    set_tests_properties(workspace PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()
