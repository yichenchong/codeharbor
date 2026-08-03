# 3.25 is the floor, not 3.24: CMakePresets.json declares schema "version": 6,
# which CMake 3.24 rejects outright ("Unrecognized 'version' field"). Since every
# documented build goes through `cmake --preset ...`, a 3.24 claim was a claim
# nobody could act on.
cmake_minimum_required(VERSION 3.25)

project(CodeHarbor
    VERSION 0.1.13
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

# Escape hatch used by src/qml/CMakeLists.txt: build a client whose editor and
# terminal panes have no page to load. Declared here as a real option (rather
# than only being read where it is used) so it shows up in `cmake -LH`, ccmake
# and the IDE preset UIs - README.md and docs/DEVELOPMENT.md both advertise it,
# and an undeclared cache variable is invisible in every one of those places.
option(CODEHARBOR_SKIP_WEB_BUNDLE
       "Do not embed the Monaco/xterm.js bundles (produces an unshippable client)" OFF)

# Source maps for the embedded web bundles, consumed by src/qml/CMakeLists.txt.
# Declared here, like the option above, because README.md and docs/DEVELOPMENT.md
# both advertise it on the command line and an undeclared cache variable is
# invisible in `cmake -LH`, ccmake and the IDE preset UIs.
#
# It is a tri-state STRING, not an option(), and that is the whole point. A
# cached boolean default would stick to whatever the build type was on the FIRST
# configure of a directory, so re-configuring an existing Debug tree as Release
# would go on embedding ~15 MB of maps in a binary labelled Release. AUTO is the
# cached value in that case, and it is re-derived from CMAKE_BUILD_TYPE on every
# configure; ON and OFF are explicit and persist, exactly as they did before.
set(CODEHARBOR_WEB_SOURCEMAP "AUTO" CACHE STRING
    "Embed JS source maps in the web bundles: ON, OFF, or AUTO (ON for a Debug configure, OFF otherwise)")
set_property(CACHE CODEHARBOR_WEB_SOURCEMAP PROPERTY STRINGS AUTO ON OFF)

# A compiler cache, if this machine happens to have one. It is a pure win when
# present (an unchanged translation unit becomes a file copy) and costs nothing
# when absent, so it is discovered rather than required: no flag to remember,
# and installing ccache later is picked up by the next configure. An explicit
# -DCMAKE_CXX_COMPILER_LAUNCHER=... on the command line still wins.
#
# ccache first: it is what the documented Linux/macOS setups have. sccache is
# the fallback because it is the one that is usually already installed on a
# machine that also builds Rust. C is listed alongside CXX even though this
# project() declares only CXX - a dependency added later that enables C should
# get the same treatment without anyone having to remember this block.
if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(CODEHARBOR_COMPILER_CACHE NAMES ccache sccache)
    if(CODEHARBOR_COMPILER_CACHE)
        set(CMAKE_C_COMPILER_LAUNCHER "${CODEHARBOR_COMPILER_CACHE}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CODEHARBOR_COMPILER_CACHE}")
        message(STATUS "compiler cache: ${CODEHARBOR_COMPILER_CACHE}")
    else()
        message(STATUS "compiler cache: not found (looked for ccache, sccache);"
                       " builds are uncached. Installing either one needs no"
                       " further configuration.")
    endif()
endif()

# Debug information for the test executables, traded against build time. There
# are 35 of them, one translation unit each, and together they are the single
# largest bucket in a Debug build of this tree (767 s of 1985 s of compile work
# measured on a 2-core box). Dropping them from `-g` to `-g1` - line tables and
# function boundaries, no local variable or type descriptions - measured 15.2 s
# -> 11.7 s for tst_qmlload.cpp, about 23%.
#
# OFF by default, deliberately. When anyone actually reaches for a debugger here
# it is almost always a failing TEST they are stepping through, and what -g1
# throws away is exactly what that session needs: the local variables and type
# descriptions. So this is a knob a developer turns on while grinding a build
# loop, not a default that quietly degrades everyone's gdb session. Neither the
# application binary nor a packaged artifact is affected either way - only
# `tst_*` targets are touched, and only in a config that has debug info at all.
option(CODEHARBOR_TEST_MINIMAL_DEBUGINFO
       "Compile tst_* with -g1 (line tables only, no local variables) for faster test builds" OFF)

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

    # Fails if review/debug scaffolding (e.g. a deliberate mutation marker left
    # behind while proving a gate can fail) reaches the tree. Cheap, and it
    # closes a hole that convention alone did not.
    if(UNIX)
        add_test(NAME tst_no_scaffolding
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/no_scaffolding.sh
                         ${CMAKE_CURRENT_SOURCE_DIR})
        add_subdirectory(tests)
        # Refuses a release tag whose commit carries a different version. Runs
        # entirely inside a throwaway git repository under TMPDIR, so it neither
        # touches this checkout nor creates a tag in it. Skips (77) when git or
        # node is absent rather than failing a build that never needed them.
        add_test(NAME tst_bump_version_tag
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/bump_version_tag.sh
                         ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(tst_bump_version_tag PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()

add_subdirectory(src)

# CODEHARBOR_TEST_MINIMAL_DEBUGINFO, applied here rather than in each of the ten
# src/*/tests/CMakeLists.txt: the test targets all exist by now, so one walk of
# the build system reaches every `tst_*` and keeps reaching new ones nobody has
# written yet. -g1 is appended AFTER the config's own -g, and GCC/Clang take the
# last debug-level flag on the command line, so it wins without editing
# CMAKE_CXX_FLAGS_DEBUG (which would hit the application too).
#
# The generator expression restricts it to the configs that carry debug info at
# all: adding -g1 to a Release test build would ADD information rather than
# remove it. MSVC has no equivalent spelling, so it is simply not offered there.
function(_codeharbor_minimal_test_debuginfo dir)
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_subdir IN LISTS _subdirs)
        _codeharbor_minimal_test_debuginfo("${_subdir}")
    endforeach()
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
        # AUTOMOC contributes a `tst_<name>_autogen` UTILITY target alongside each
        # test, and target_compile_options is an error on one of those.
        get_target_property(_type ${_target} TYPE)
        if(_target MATCHES "^tst_" AND _type STREQUAL "EXECUTABLE")
            target_compile_options(${_target} PRIVATE
                $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>>:-g1>)
        endif()
    endforeach()
endfunction()
if(CODEHARBOR_TEST_MINIMAL_DEBUGINFO AND NOT MSVC)
    _codeharbor_minimal_test_debuginfo("${CMAKE_CURRENT_SOURCE_DIR}")
endif()
