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
