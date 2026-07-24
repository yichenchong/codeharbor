qt_add_library(ch_persistence STATIC
    WorkspaceDb.h
    WorkspaceDb.cpp
)

target_include_directories(ch_persistence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Client-side only (SPEC 11.2): all persistence goes through codeharbord RPC via
# ch_remote, so there is no local SQLite and no Qt6::Sql dependency.
target_link_libraries(ch_persistence PUBLIC Qt6::Core ch_remote ch_models)

if(CODEHARBOR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
