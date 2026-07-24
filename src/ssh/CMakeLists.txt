qt_add_library(ch_ssh STATIC
    SshConnectionPool.h
    SshConnectionPool.cpp
)

target_include_directories(ch_ssh PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ch_ssh PUBLIC Qt6::Core ch_models)

# libssh is normalized to the ch_libssh interface target by the top-level
# CMakeLists (CONFIG package or pkg-config). Optional at configure time so the
# tree still configures without it; non-functional until present (docs/PLAN.md S).
if(TARGET ch_libssh)
    target_link_libraries(ch_ssh PUBLIC ch_libssh)
    target_compile_definitions(ch_ssh PUBLIC CH_HAVE_LIBSSH=1)
else()
    target_compile_definitions(ch_ssh PUBLIC CH_HAVE_LIBSSH=0)
endif()
