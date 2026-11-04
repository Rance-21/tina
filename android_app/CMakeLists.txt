cmake_minimum_required(VERSION 3.24)

project(tina_android_client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)

qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

qt_add_executable(tina_ide_client
    src/main.cpp
    src/ffi_client_bridge.hpp
    src/ffi_client_bridge.cpp
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
    RESOURCES
        assets/bg_16_10.png
)

set_target_properties(tina_ide_client PROPERTIES
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(tina_ide_client PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)
