qt_add_library(ch_persistence STATIC
    WorkspaceDb.h
    WorkspaceDb.cpp
)

target_include_directories(ch_persistence PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ch_persistence PUBLIC Qt6::Core Qt6::Sql ch_models)
