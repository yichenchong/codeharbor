cmake_minimum_required(VERSION 3.24)

project(CodeHarbor
    VERSION 0.1.0
    DESCRIPTION "Remote-first development workspace client"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(CODEHARBOR_BUILD_TESTS "Build C++ unit tests" ON)

# Qt 6.6+ is required for the QML modules and WebEngine/WebChannel integration.
find_package(Qt6 6.6 REQUIRED COMPONENTS
    Core
    Gui
    Quick
    Qml
    QuickControls2
    Sql
    WebEngineQuick
    WebChannel
    Network)

qt_standard_project_setup(REQUIRES 6.6)

# libssh provides SSH connection/channel/PTY transport (see src/ssh).
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBSSH IMPORTED_TARGET libssh)
endif()
if(NOT LIBSSH_FOUND)
    message(WARNING
        "libssh not found via pkg-config. The ssh module will fail to link "
        "until libssh is installed (see docs/PLAN.md, workstream S).")
endif()

add_subdirectory(src)

if(CODEHARBOR_BUILD_TESTS)
    enable_testing()
endif()
