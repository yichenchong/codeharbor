# Overlay of vcpkg's `libssh` port, pinned forward to 0.12.2.
#
# vcpkg's registry port is still 0.12.0, whose hybrid ML-KEM key exchange
# (mlkem768x25519-sha256, first in libssh's DEFAULT_KEY_EXCHANGE) hands
# ssh_buffer_pack() an un-cast `int` where it reads a `size_t`. On the MS x64 ABI
# that is the first stack-passed argument, so its high dword is stack residue and
# packing fails with "Failed to construct client init buffer" before any host key
# is seen. Upstream fixed the cast in 0.12.1; 0.12.2 is the current release.
#
# Everything except VERSION/SHA512 mirrors the registry port, so the built tree
# keeps the layout, features and CMake config the client already expects.
#
# DO NOT "correct" the patches. They are unified diffs, so every line that does
# not start with `+` or `-` is CONTEXT that must reproduce the upstream file
# byte for byte. 0001-export-pkgconfig-file.patch quotes a line that upstream
# libssh really does misspell — `set(LIBSSSH_PC_REQUIRES_PRIVATE "")`, with
# three S — while the rest of upstream reads the two-S name. Upstream's
# initialiser is therefore dead, which is harmless: `string(APPEND ...)` creates
# the two-S variable on first use. Rewriting the context line to the two-S
# spelling makes the patch stop matching, and dropping its leading space makes
# git reject the whole hunk as a corrupt patch. Either way the Windows build
# fails at extraction, before anything is compiled.
vcpkg_download_distfile(distfile
    URLS https://www.libssh.org/files/0.12/libssh-${VERSION}.tar.xz
    FILENAME libssh-${VERSION}.tar.xz
    SHA512 92b95d64772906fc0fe497fed4dc34a160f2397f71ef3871dc1ea0fe1e8e3c00df699ed0efdc0b754feb23e2140bda17be41d16aee48ff715487384023fdd467
)
vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${distfile}"
    PATCHES
        0001-export-pkgconfig-file.patch
        0003-no-source-write.patch
        0004-file-permissions-constants.patch
        android-glob-tilde.diff
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        pcap    WITH_PCAP
        server  WITH_SERVER
        zlib    WITH_ZLIB
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DCMAKE_REQUIRE_FIND_PACKAGE_OpenSSL=ON
        -DWITH_EXAMPLES=OFF
        -DWITH_GSSAPI=OFF
        -DWITH_NACL=OFF
        -DWITH_SYMBOL_VERSIONING=OFF
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/libssh/libssh.h"
        "#ifdef LIBSSH_STATIC"
        "#if 1"
    )
endif()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/libssh)

# The zlib guard in the config written just below is correct as written: the
# vcpkg_check_features() call above sets WITH_ZLIB to ON/OFF in THIS scope
# (set(... PARENT_SCOPE) in scripts/cmake/vcpkg_check_features.cmake), so the
# embedded conditional expands to if("ON")/if("OFF") at port time — it is not an
# unresolved if(""). Left upstream-verbatim; this comment only stops the
# port-time expansion being misread as a deferred variable (BD28).
file(READ "${CURRENT_PACKAGES_DIR}/share/libssh/libssh-config.cmake" cmake_config)
file(WRITE "${CURRENT_PACKAGES_DIR}/share/libssh/libssh-config.cmake" "
include(CMakeFindDependencyMacro)
if(MINGW32)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_dependency(Threads)
endif()
find_dependency(OpenSSL)
if(\"${WITH_ZLIB}\")
    find_dependency(ZLIB)
endif()
${cmake_config}"
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
