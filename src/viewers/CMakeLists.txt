qt_add_library(ch_viewers STATIC
    ViewerHandlerRegistry.h
    ViewerHandlerRegistry.cpp
)

target_include_directories(ch_viewers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ch_viewers PUBLIC Qt6::Core ch_models)
