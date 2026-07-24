qt_add_library(ch_ssh STATIC
    SshConnectionPool.h
    SshConnectionPool.cpp
)

target_include_directories(ch_ssh PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ch_ssh PUBLIC Qt6::Core ch_models)

# libssh is optional at configure time so the tree still configures without it,
# but the module cannot function until it is linked (see docs/PLAN.md, S).
if(LIBSSH_FOUND)
    target_link_libraries(ch_ssh PUBLIC PkgConfig::LIBSSH)
    target_compile_definitions(ch_ssh PUBLIC CH_HAVE_LIBSSH=1)
else()
    target_compile_definitions(ch_ssh PUBLIC CH_HAVE_LIBSSH=0)
endif()
