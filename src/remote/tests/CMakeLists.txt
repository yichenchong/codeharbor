qt_add_executable(tst_rpcclient tst_rpcclient.cpp)

target_link_libraries(tst_rpcclient PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
    ch_remote
)

# Repo root so the best-effort live test can spawn the Node codeharbord service
# via `node <repo>/remote/src/codeharbord.ts rpc --stdio`.
target_compile_definitions(tst_rpcclient PRIVATE
    CH_REPO_ROOT="${CMAKE_SOURCE_DIR}"
)

add_test(NAME tst_rpcclient COMMAND tst_rpcclient)

# Console-mode on Windows, for the same reason as src/app/tests: qt_add_executable
# marks executables as GUI subsystem, which on Windows swallows QTest's stdout and
# stderr, so a failing assertion reports nothing at all through ctest.
if(WIN32)
    set_target_properties(tst_rpcclient PROPERTIES WIN32_EXECUTABLE FALSE)
endif()
