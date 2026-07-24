qt_add_executable(tst_workspacedb tst_workspacedb.cpp)

target_link_libraries(tst_workspacedb PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
    ch_persistence
)

# Repo root so the best-effort live test can spawn the Node codeharbord service
# via `node <repo>/remote/src/codeharbord.ts rpc --stdio`.
target_compile_definitions(tst_workspacedb PRIVATE
    CH_REPO_ROOT="${CMAKE_SOURCE_DIR}"
)

add_test(NAME tst_workspacedb COMMAND tst_workspacedb)
