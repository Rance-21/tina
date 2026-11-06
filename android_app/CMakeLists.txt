cmake_minimum_required(VERSION 3.24)

project(tina_android_client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network)

qt_standard_project_setup(REQUIRES 6.5)
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.8)
    qt_policy(SET QTP0004 NEW)
endif()

qt_add_executable(tina_ide_client
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
)

qt_add_qml_module(tina_ide_client
    URI TinaIde
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/GlassPanel.qml
        qml/CodePane.qml
    RESOURCES
        assets/bg_16_10.png
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
        PREFIX "/qt/qml/TinaIde" FILES assets/bg_16_10.png
        qml/Main.qml qml/CodePane.qml qml/GlassPanel.qml)
    add_test(NAME workspace COMMAND tina_workspace_test)
    set_tests_properties(workspace PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()
