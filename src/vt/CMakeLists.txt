# The native VT engine for the mobile clients.
#
# It exists because the desktop terminal pane is xterm.js inside Qt WebEngine
# (src/qml/TerminalPaneView.qml), and Qt WebEngine ships on neither Android nor
# iOS. Nothing here may depend on WebEngine, WebChannel, Quick or QML: the engine
# is a headless model over raw PTY bytes, which is what lets tst_vt drive every
# escape sequence from string literals with no SSH connection and no device.
#
# Qt6::Gui, not just Qt6::Core, for QRgb - the cell colour type. No QGuiApplication
# is ever constructed by this library or its test.
qt_add_library(ch_vt STATIC
    VtTypes.h
    VtTypes.cpp
    VtParser.h
    VtParser.cpp
    VtScreen.h
    VtScreen.cpp
    VtKeys.h
    VtKeys.cpp
)

target_include_directories(ch_vt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ch_vt PUBLIC Qt6::Core Qt6::Gui)

if(CODEHARBOR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
