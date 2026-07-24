# QML module for the CodeHarbor UI, kept as a sibling of app/ per the proposed
# architecture (SPEC 14). Built as a static QML module library that the app
# executable links against.
qt_add_library(codeharbor_qml STATIC)

qt_add_qml_module(codeharbor_qml
    URI CodeHarbor
    VERSION 1.0
    # Output dir must end in the module target path (URI) so qmllint/qmlls
    # tooling resolves the module correctly.
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/CodeHarbor
    QML_FILES
        Main.qml
        SessionsSidebar.qml
        ViewerRegion.qml
        TerminalRegion.qml
        TerminalPaneView.qml
)

target_link_libraries(codeharbor_qml PUBLIC
    Qt6::Quick
    Qt6::QuickControls2
)
