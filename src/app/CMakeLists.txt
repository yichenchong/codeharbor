# Application controller as a static library so both the executable and the
# unit test link it (SPEC 4.1, workstream U).
qt_add_library(ch_app STATIC
    AppController.h
    AppController.cpp
    UiStateStore.h
    UiStateStore.cpp
)

target_include_directories(ch_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ch_app PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    ch_models
    ch_persistence
    ch_remote
    ch_agent
)

qt_add_executable(codeharbor
    main.cpp
)

target_link_libraries(codeharbor PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Qml
    Qt6::QuickControls2
    Qt6::WebEngineQuick
    Qt6::WebChannel
    # QML module (src/qml); linking the generated plugin registers the module.
    codeharbor_qmlplugin
    ch_app
    ch_models
    ch_persistence
    ch_ssh
    ch_terminal
    ch_viewers
    ch_remote
    ch_editor
    ch_agent
)

target_compile_definitions(codeharbor PRIVATE
    CODEHARBOR_VERSION="${PROJECT_VERSION}"
)

set_target_properties(codeharbor PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)

if(CODEHARBOR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
