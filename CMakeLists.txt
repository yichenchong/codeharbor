# 3.25 is the floor, not 3.24: CMakePresets.json declares schema "version": 6,
# which CMake 3.24 rejects outright ("Unrecognized 'version' field"). Since every
# documented build goes through `cmake --preset ...`, a 3.24 claim was a claim
# nobody could act on.
cmake_minimum_required(VERSION 3.25)

project(CodeHarbor
    VERSION 0.1.8
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
#
# The list is exactly what the tree links. Qt6::Sql is deliberately absent: all
# persistence goes through codeharbord over RPC (SPEC 11.2), so the client owns
# no local database and nothing links Qt6::Sql (see src/persistence/CMakeLists.txt).
find_package(Qt6 6.9 REQUIRED COMPONENTS
    Core
    Gui
    Quick
    Qml
    QuickControls2
    WebEngineCore
    WebEngineQuick
    WebChannel
    Network)

# Qt Test is used ONLY by the per-module test targets, every one of which sits
# behind `if(CODEHARBOR_BUILD_TESTS)`. Requiring it unconditionally made the
# tests-off release configure fail on a Qt install that ships no Test module.
if(CODEHARBOR_BUILD_TESTS)
    find_package(Qt6 6.9 REQUIRED COMPONENTS Test)
endif()

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
# The audited floor is libssh 0.11.2 (docs/DEVELOPMENT.md § Verified toolchain,
# and the per-symbol audit comment at the top of src/ssh/SshConnectionPool.cpp).
# This is a WARNING, not a hard requirement: several supported distributions
# still ship 0.10.x, the client compiles and links against it, and everything
# except the libssh-0.12.0 key-exchange workaround works there. Turning it into
# a `find_package` version constraint would refuse to configure on those
# machines, which is a worse outcome than an explicit warning.
set(CODEHARBOR_LIBSSH_FLOOR "0.11.2")
function(_ch_warn_if_old_libssh version how)
    if("${version}" STREQUAL ""
       OR "${version}" VERSION_GREATER_EQUAL "${CODEHARBOR_LIBSSH_FLOOR}")
        return()
    endif()
    message(WARNING
        "libssh ${version} (found via ${how}) is older than the audited floor "
        "${CODEHARBOR_LIBSSH_FLOOR}. It will build and connect, but it is missing "
        "the CVE-2025-5351 fix in the public-key export path this client calls on "
        "every connect, and the libssh-0.12.0 key-exchange workaround in "
        "src/ssh/SshConnectionPool.cpp needs the 0.11.0 algorithm-list syntax. "
        "See docs/DEVELOPMENT.md.")
endfunction()
if(_ch_libssh_target)
    add_library(ch_libssh INTERFACE)
    target_link_libraries(ch_libssh INTERFACE ${_ch_libssh_target})
    message(STATUS "libssh: found via CONFIG package (target ${_ch_libssh_target})"
                   " version ${libssh_VERSION}")
    _ch_warn_if_old_libssh("${libssh_VERSION}" "CONFIG package")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSSH IMPORTED_TARGET libssh)
    endif()
    if(LIBSSH_FOUND)
        add_library(ch_libssh INTERFACE)
        target_link_libraries(ch_libssh INTERFACE PkgConfig::LIBSSH)
        message(STATUS "libssh: found via pkg-config ${LIBSSH_VERSION}")
        _ch_warn_if_old_libssh("${LIBSSH_VERSION}" "pkg-config")
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
