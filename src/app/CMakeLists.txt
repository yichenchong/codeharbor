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
    ch_models
    ch_persistence
    ch_ssh
    ch_terminal
    ch_viewers
    ch_remote
)

target_compile_definitions(codeharbor PRIVATE
    CODEHARBOR_VERSION="${PROJECT_VERSION}"
)

set_target_properties(codeharbor PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)
