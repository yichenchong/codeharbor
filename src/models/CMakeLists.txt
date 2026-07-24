qt_add_library(ch_models STATIC
    Ids.h
    SessionState.h
    SessionState.cpp
)

target_include_directories(ch_models PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ch_models PUBLIC Qt6::Core)
