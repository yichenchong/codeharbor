cmake_minimum_required(VERSION 3.24)

project(CodeHarbor
    VERSION 0.1.1
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

# Qt 6.9+ is required: QQuickWebEngineProfile's (storageName, parent) constructor
# arrived in 6.9, and it is the only profile-creation form that does not emit the
# "use WebEngineProfilePrototype" deprecation warning that tst_qmlload treats as a
# failure. 6.6/6.8 do not compile (see src/viewers/ViewerProfiles.cpp).
find_package(Qt6 6.9 REQUIRED COMPONENTS
    Core
    Gui
    Quick
    Qml
    QuickControls2
    Sql
    WebEngineCore
    WebEngineQuick
    WebChannel
    Network
    Test)

qt_standard_project_setup(REQUIRES 6.9)

# libssh provides SSH connection/channel/PTY transport (see src/ssh). Discovery
# is normalized behind the interface target `ch_libssh` so the ssh module does
# not care how it was found:
#   * CONFIG package  — vcpkg (Windows), Homebrew (macOS), or a CMake install.
#   * pkg-config      — system libssh on Linux.
find_package(libssh CONFIG QUIET)
set(_ch_libssh_target "")
if(libssh_FOUND)
    if(TARGET ssh)
        set(_ch_libssh_target ssh)
    elseif(TARGET libssh::libssh)
        set(_ch_libssh_target libssh::libssh)
    endif()
endif()
if(_ch_libssh_target)
    add_library(ch_libssh INTERFACE)
    target_link_libraries(ch_libssh INTERFACE ${_ch_libssh_target})
    message(STATUS "libssh: found via CONFIG package (target ${_ch_libssh_target})")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSSH IMPORTED_TARGET libssh)
    endif()
    if(LIBSSH_FOUND)
        add_library(ch_libssh INTERFACE)
        target_link_libraries(ch_libssh INTERFACE PkgConfig::LIBSSH)
        message(STATUS "libssh: found via pkg-config ${LIBSSH_VERSION}")
    endif()
endif()
if(NOT TARGET ch_libssh)
    message(WARNING
        "libssh not found (tried CONFIG package and pkg-config). The ssh module "
        "will build without it (CH_HAVE_LIBSSH=0) and be non-functional. See "
        "docs/DEVELOPMENT.md for install instructions.")
endif()

# Enable CTest before adding subdirectories so module test targets register.
if(CODEHARBOR_BUILD_TESTS)
    enable_testing()

    # Fails if review/debug scaffolding (e.g. a deliberate MUTATION-TEST line
    # left behind while proving a gate can fail) reaches the tree. Cheap, and it
    # closes a hole that convention alone did not.
    if(UNIX)
        add_test(NAME tst_no_scaffolding
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/no_scaffolding.sh
                         ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endif()

add_subdirectory(src)
