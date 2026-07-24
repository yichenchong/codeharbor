qt_add_library(ch_viewers STATIC
    ViewerHandlerRegistry.h
    ViewerHandlerRegistry.cpp
    InternalUrlSchemeHandler.h
    InternalUrlSchemeHandler.cpp
    ViewerProfiles.h
    ViewerProfiles.cpp
    ViewerModel.h
    ViewerModel.cpp
)

target_include_directories(ch_viewers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ch_viewers PUBLIC
    Qt6::Core
    Qt6::WebEngineCore
    Qt6::WebEngineQuick
    Qt6::WebChannel
    ch_remote
    ch_models
)

if(CODEHARBOR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
