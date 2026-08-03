# Application controller as a static library so both the executable and the
# unit test link it (SPEC 4.1, workstream U).
qt_add_library(ch_app STATIC
    AppController.h
    AppController.cpp
    SessionBootstrap.h
    SessionBootstrap.cpp
    ServerProfiles.h
    ServerProfiles.cpp
    SessionLayouts.h
    SessionLayouts.cpp
    UiStateStore.h
    UiStateStore.cpp
    AppSettings.h
    AppSettings.cpp
    Notifier.h
    Notifier.cpp
    WindowChromeNative.h
    WindowChromeNative.cpp
    LogBuffer.h
    LogBuffer.cpp
    GroupPaletteService.h
    GroupPaletteService.cpp
)

target_include_directories(ch_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ch_app PUBLIC
    Qt6::Core
    Qt6::Gui
    # QTcpSocket backs SessionBootstrap's bounded connect pre-flight.
    Qt6::Network
    Qt6::Quick
    ch_models
    ch_persistence
    ch_remote
    ch_ssh
    ch_agent
    ch_terminal
    # AppController forwards server.info.recoveryDir to the per-pane editor
    # factory (SPEC 11.3 crash recovery); it references ch::EditorFactory.
    ch_editor
)

# QtDBus backs the desktop notifications in Notifier on Linux - a Phase 4
# deliverable in SPEC 16, which has no section of its own. It is optional at
# configure time exactly like libssh in src/ssh: without it the module still
# builds (CH_HAVE_DBUS=0) and Notifier::available() is false, so notify()
# degrades to a silent no-op.
find_package(Qt6 6.9 QUIET COMPONENTS DBus)
if(Qt6DBus_FOUND)
    target_link_libraries(ch_app PUBLIC Qt6::DBus)
    target_compile_definitions(ch_app PUBLIC CH_HAVE_DBUS=1)
    message(STATUS "QtDBus: found; desktop notifications enabled")
else()
    target_compile_definitions(ch_app PUBLIC CH_HAVE_DBUS=0)
    message(STATUS "QtDBus: not found; desktop notifications disabled (CH_HAVE_DBUS=0)")
endif()

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
