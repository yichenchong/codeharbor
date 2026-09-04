# 3.25 is the floor, not 3.24: CMakePresets.json declares schema "version": 6,
# which CMake 3.24 rejects outright ("Unrecognized 'version' field"). Since every
# documented build goes through `cmake --preset ...`, a 3.24 claim was a claim
# nobody could act on.
cmake_minimum_required(VERSION 3.25)

project(CodeHarbor
    VERSION 0.4.2
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

# Bound how many links run at once on Windows.
#
# This tree links a lot of executables: the desktop client, the mobile client,
# and (with tests on) more than thirty tst_* binaries, seven of which the mobile
# client added. Under Ninja they all link concurrently, and on Windows that
# repeatedly failed in CI inside cmake's own `-E vs_link_exe` step with "Access
# is denied." or "The process cannot access the file because it is being used by
# another process" -- a different target each time. That is file contention on a
# freshly written .exe, which on a Windows runner is characteristically the
# on-access malware scanner opening the new image while the linker still needs
# it. It cost the v0.4.0 release two re-runs of an otherwise green job.
#
# A link job pool serialises the link steps without slowing compilation, which
# shrinks the window in which two new executables are being written at once.
# Stated honestly: this REDUCES the collision probability, it does not remove
# the cause, because a scanner can still open a single new binary at the wrong
# moment. It is set only for Windows, where the failures happen; POSIX linkers
# do not have this problem and the parallelism is worth keeping there.
if(WIN32)
    set_property(GLOBAL PROPERTY JOB_POOLS codeharbor_link_pool=2)
    set(CMAKE_JOB_POOL_LINK codeharbor_link_pool)
endif()

option(CODEHARBOR_BUILD_TESTS "Build C++ unit tests" ON)

# Which presentation layer(s) this tree produces.
#
# The desktop client is a Qt WebEngine application: every rich surface it has -
# terminal, editor, Markdown/image/PDF/web viewers - is a Chromium page driven
# over QWebChannel. Qt ships NO WebEngine for Android and NO WebEngine for iOS,
# and that is not a packaging gap that a flag can close. So the mobile client is
# a genuinely different presentation layer (src/vt + src/mobile, native Qt
# Quick) over the SAME portable backend (models, remote, persistence, ssh,
# terminal, editor, agent, app).
#
# CODEHARBOR_BUILD_DESKTOP is forced OFF on Android/iOS rather than merely
# defaulted off: a mobile configure that found some WebEngine on the host prefix
# path and tried to link it would fail deep in the link step instead of here.
option(CODEHARBOR_BUILD_DESKTOP "Build the desktop WebEngine client (src/qml, src/viewers, codeharbor)" ON)

# ON everywhere, deliberately including desktop Linux/macOS/Windows. The mobile
# shell is plain Qt Quick with no device-only API, so building and testing it on
# a workstation is the fast loop; requiring an NDK or an iPhone to compile
# src/mobile would mean it only ever got compiled on a release runner.
option(CODEHARBOR_BUILD_MOBILE "Build the mobile client (src/vt, src/mobile, codeharbor_mobile)" ON)

if(ANDROID OR IOS)
    if(CODEHARBOR_BUILD_DESKTOP)
        message(STATUS "Qt WebEngine does not exist for Android or iOS;"
                       " forcing CODEHARBOR_BUILD_DESKTOP=OFF")
    endif()
    set(CODEHARBOR_BUILD_DESKTOP OFF CACHE BOOL
        "Build the desktop WebEngine client (src/qml, src/viewers, codeharbor)" FORCE)
endif()

if(NOT CODEHARBOR_BUILD_DESKTOP AND NOT CODEHARBOR_BUILD_MOBILE)
    message(FATAL_ERROR
        "Both CODEHARBOR_BUILD_DESKTOP and CODEHARBOR_BUILD_MOBILE are OFF; "
        "this configure would build backend libraries and no client at all.")
endif()

# Every tst_* target in this tree is a HOST executable that ctest launches
# locally, and a cross-compiled one cannot run here. Leaving tests ON for a
# device configure therefore produces nothing runnable AND actively breaks the
# packaging targets on Android: Qt gives every executable its own
# androiddeployqt/APK target and adds it to the global `apk` target, so
# `cmake --build --preset android-arm64-apk` would build a dozen packages
# instead of one, and the seven test executables that share
# src/mobile/tests/CMakeLists.txt would all deploy into the same
# `android-build` directory (Qt warns about exactly this and the artifacts mix).
#
# Forced rather than defaulted, for the same reason CODEHARBOR_BUILD_DESKTOP is:
# the mobile presets already pass OFF, so the only way to reach this is an
# explicit -DCODEHARBOR_BUILD_TESTS=ON on a cross configure, where the honest
# answer is that the request cannot be satisfied.
if(ANDROID OR IOS)
    if(CODEHARBOR_BUILD_TESTS)
        message(STATUS "Cross-compiling for ${CMAKE_SYSTEM_NAME}; the tst_* targets are"
                       " host executables ctest cannot run there - forcing"
                       " CODEHARBOR_BUILD_TESTS=OFF")
    endif()
    set(CODEHARBOR_BUILD_TESTS OFF CACHE BOOL "Build C++ unit tests" FORCE)
endif()

# Bundle identifier for the mobile client. Declared here (not buried in
# src/mobile) because packaging/ios/Info.plist.in and packaging/android/
# AndroidManifest.xml must agree with it, and because a fork shipping its own
# build needs one place to change.
set(CODEHARBOR_MOBILE_BUNDLE_ID "dev.codeharbor.mobile" CACHE STRING
    "Application/bundle identifier for the Android and iOS clients")

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
# are dozens of them, one translation unit each, and together they are the
# single largest bucket in a Debug build. Dropping them from `-g` to `-g1` -
# line tables and function boundaries, no local variable or type descriptions -
# measured 15.2 s -> 11.7 s for tst_qmlload.cpp, about 23%.
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

# Qt 6.10+ is required. Two independent reasons:
#
# 1. QQuickWebEngineProfile's (storageName, parent) constructor arrived in 6.9,
#    and it is the only profile-creation form that does not emit the
#    "use WebEngineProfilePrototype" deprecation warning that tst_qmlload treats
#    as a failure. 6.6/6.8 do not compile (see src/viewers/ViewerProfiles.cpp).
# 2. Qt 6.9 crashes when a WebEngineView is destroyed while one of its
#    navigations is still outstanding: Chromium delivers the navigation decision
#    afterwards and QQuickWebEngineViewPrivate::navigationRequested() wraps it
#    for JavaScript through the QML engine the dead view no longer has, so
#    QJSEngine::newQObject() dereferences null. The viewer panes hit this every
#    time the user picks "Open as" on a page that has not finished loading
#    (reproduced deterministically on 6.9.3 by tst_paneidentity's
#    anUnrelatedRepublishKeepsAnOpenAsChoice). Nothing in QML can hand a dying
#    object an engine or cancel a decision Chromium already dispatched -
#    WebEngineView::stop() on the live outgoing view was measured and does not
#    prevent it - so the floor is the fix. 6.10 is clean in Debug, Release and
#    under AddressSanitizer.
#
# The unconditional list is exactly what the portable backend and BOTH clients
# link. Qt6::Sql is deliberately absent: all persistence goes through
# codeharbord over RPC (SPEC 11.2), so the client owns no local database and
# nothing links Qt6::Sql (see src/persistence/CMakeLists.txt).
#
# WebEngineCore/WebEngineQuick/WebChannel are requested SEPARATELY, and only for
# a desktop configure. They are not merely unused on Android and iOS - Qt ships
# no such modules for those platforms at all, so leaving them in the REQUIRED
# list would abort a mobile configure before it produced a single target. The
# 6.10 floor above is a WebEngine floor, and it stays as the project-wide
# minimum anyway: 6.10 is also the version whose Qt Quick, Android and iOS
# support the mobile client is written against, and keeping one floor for the
# whole tree means a single Qt install serves every preset.
find_package(Qt6 6.10 REQUIRED COMPONENTS
    Core
    Gui
    Quick
    Qml
    QuickControls2
    Network)

if(CODEHARBOR_BUILD_DESKTOP)
    find_package(Qt6 6.10 REQUIRED COMPONENTS
        WebEngineCore
        WebEngineQuick
        WebChannel)
endif()

# Optional mobile-only viewer backends, discovered the same way libssh (below)
# and QtDBus (src/app/CMakeLists.txt) are: absent module must still build.
#
# The mobile client has no Chromium to fall back on, so a missing module is a
# missing capability rather than a missing optimisation - which is why the
# result is published to QML through ch::MobileCapabilities instead of being
# silently compiled out. Without Qt6::Pdf the PDF page says so and offers to
# open the file as bytes; without Qt6::WebView an http(s) link is shown as
# plain text and is never handed to an external browser (SPEC 7.5).
#
# QUIET, not REQUIRED: neither module is part of a default Qt install. Both
# default to 0 so src/mobile can use the values unconditionally, and the probe
# itself is skipped entirely for a desktop-only configure, which links neither.
set(CH_HAVE_QTPDF 0)
set(CH_HAVE_QTWEBVIEW 0)
if(CODEHARBOR_BUILD_MOBILE)
    # PdfQuick as well as Pdf: the page is QML, so the C++ library alone is not
    # enough to render anything.
    find_package(Qt6 6.10 QUIET COMPONENTS Pdf PdfQuick)
    if(Qt6Pdf_FOUND AND Qt6PdfQuick_FOUND)
        set(CH_HAVE_QTPDF 1)
        message(STATUS "Qt PDF: found; the mobile PDF viewer renders pages natively")
    else()
        message(STATUS "Qt PDF: not found; the mobile PDF viewer is unavailable (CH_HAVE_QTPDF=0)")
    endif()

    # WebViewQuick as well as WebView, for exactly the reason PdfQuick is
    # required above: Qt6::WebView is the C++ platform-web-view wrapper, while
    # the `QtWebView` QML module the page imports lives in the SEPARATE
    # Qt6::WebViewQuick target (qtwebview's src/quick declares
    # URI "QtWebView"). Reporting the capability on WebView alone would tell
    # QML the page is available and then fail its import at runtime - and on a
    # static Qt (which is what Qt for iOS is, the one platform where this
    # module is actually present) that failure is a blank page, not a warning
    # anybody sees at build time.
    find_package(Qt6 6.10 QUIET COMPONENTS WebView WebViewQuick)
    if(Qt6WebView_FOUND AND Qt6WebViewQuick_FOUND)
        set(CH_HAVE_QTWEBVIEW 1)
        message(STATUS "Qt WebView: found; the mobile web viewer uses the system web view")
    else()
        message(STATUS "Qt WebView: not found; the mobile web viewer is unavailable (CH_HAVE_QTWEBVIEW=0)")
    endif()
endif()

# Qt Test is used ONLY by the per-module test targets, every one of which sits
# behind `if(CODEHARBOR_BUILD_TESTS)`. Requiring it unconditionally made the
# tests-off release configure fail on a Qt install that ships no Test module.
if(CODEHARBOR_BUILD_TESTS)
    find_package(Qt6 6.10 REQUIRED COMPONENTS Test)
endif()

qt_standard_project_setup(REQUIRES 6.10)

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
# The audited runtime floor is libssh 0.11.2 (docs/DEVELOPMENT.md § Verified
# toolchain, and the per-symbol audit comment in src/ssh/SshConnectionPool.cpp).
# The vcpkg registry still offers 0.12.0, whose hybrid ML-KEM exchange cannot
# complete a handshake against a modern sshd. SshConnectionPool subtracts that
# algorithm only when the runtime version is exactly 0.12.0 - never 0.12.1 or
# newer. The separate Windows release overlay is pinned to 0.12.2, which contains
# the upstream fix, and does not change the audited runtime floor for system
# packages on Linux or macOS.
set(CODEHARBOR_LIBSSH_FLOOR "0.11.2")
function(_ch_warn_if_old_libssh version how)
    if("${version}" VERSION_EQUAL "0.12.0")
        message(WARNING
            "libssh 0.12.0 (found via ${how}) cannot complete a handshake "
            "against a modern sshd without the ML-KEM subtraction workaround. "
            "SshConnectionPool applies it only to exactly 0.12.0, never to "
            "0.12.1 or newer; upgrade when possible. The Windows release "
            "overlay is separately pinned to libssh 0.12.2.")
        return()
    endif()
    if("${version}" STREQUAL ""
       OR "${version}" VERSION_GREATER_EQUAL "${CODEHARBOR_LIBSSH_FLOOR}")
        return()
    endif()
    message(WARNING
        "libssh ${version} (found via ${how}) is older than the audited runtime "
        "floor ${CODEHARBOR_LIBSSH_FLOOR}. It may build, but it is missing the "
        "CVE-2025-5351 fix in the public-key export path this client calls on "
        "every connect. The Windows release overlay is separately pinned to "
        "libssh 0.12.2. See docs/DEVELOPMENT.md.")
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

# A cross-compile has no system package manager to fall back on, and neither the
# NDK toolchain file nor the Qt-for-Android/iOS prefix carries libssh. So the
# generic warning above ("install libssh-dev") is actively misleading there: the
# fix is a second prefix path, not an apt package. Says so explicitly, because a
# mobile client with CH_HAVE_LIBSSH=0 builds, installs, launches, and then
# cannot connect to anything - a failure that only shows up on a device.
if((ANDROID OR IOS) AND NOT TARGET ch_libssh)
    message(STATUS
        "libssh: cross-compiling for ${CMAKE_SYSTEM_NAME} and nothing was found. "
        "Build it for this ABI with vcpkg using the in-tree overlay "
        "(packaging/vcpkg-ports, triplet arm64-android or arm64-ios) and point "
        "the configure at it by exporting CH_LIBSSH_PREFIX - the mobile presets "
        "append it to CMAKE_PREFIX_PATH. See docs/DEVELOPMENT.md "
        "'Mobile clients (Android / iOS)'.")
endif()

# Enable CTest before adding subdirectories so module test targets register.
if(CODEHARBOR_BUILD_TESTS)
    enable_testing()

    # Repository-wide gates. They are shell scripts, so they run on UNIX only;
    # `tests/` (the live-fixture CTest fixture) guards itself the same way and
    # is added unconditionally below.
    #
    # Every one of them carries an explicit TIMEOUT, as every other test in this
    # tree does. CTest's default is 1500 s, and every one of these can hang
    # rather than fail: the scaffolding scan walks the whole worktree, and the
    # bump-version test runs git and node inside a throwaway repository, where a
    # git subprocess waiting on a credential or editor prompt would otherwise
    # stall the suite for twenty-five minutes before reporting anything.
    if(UNIX)
        # Fails if review/debug scaffolding (e.g. a deliberate mutation marker
        # left behind while proving a gate can fail) reaches the tree. Cheap,
        # and it closes a hole that convention alone did not.
        add_test(NAME tst_no_scaffolding
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/no_scaffolding.sh
                         ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(tst_no_scaffolding PROPERTIES TIMEOUT 120)
        # Refuses a release tag whose commit carries a different version. Runs
        # entirely inside a throwaway git repository under TMPDIR, so it neither
        # touches this checkout nor creates a tag in it. Skips (77) when git or
        # node is absent rather than failing a build that never needed them.
        add_test(NAME tst_bump_version_tag
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/bump_version_tag.sh
                         ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(tst_bump_version_tag PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 300)
        # Checks that the in-tree vcpkg overlay patches are still well-formed
        # unified diffs, and that the portfile's PATCHES list and the patch
        # files on disk name exactly the same set. Nothing an ordinary Linux or
        # macOS build does reads them: they are applied on a Windows release
        # runner, against a tarball downloaded there, so a malformed or
        # misnamed one is invisible until a release is already tagged and its
        # Windows job dies during extraction.
        add_test(NAME tst_vcpkg_patches
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/vcpkg_patches.sh
                         ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(tst_vcpkg_patches PROPERTIES TIMEOUT 60)
        # Guards the parser that names every release artifact
        # (.github/scripts/release-version.sh). Like the vcpkg patch check, it
        # covers something no ordinary build reads: the script runs only on a
        # release runner, where a version it failed to parse would name every
        # asset after an empty string and fail the publish job after the whole
        # cross-OS build - with the tag already permanent.
        add_test(NAME tst_release_version
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/release_version.sh
                         ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(tst_release_version PROPERTIES TIMEOUT 60)
    endif()

    add_subdirectory(tests)
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
