# QML module for the mobile shell, kept as a sibling of src/mobile the same way
# src/qml is a sibling of src/app (SPEC 14). Built as a static QML module library
# that codeharbor_mobile links against.
#
# The URI is CodeHarbor.Mobile, NOT CodeHarbor: both modules can be present in
# one build tree (CODEHARBOR_BUILD_MOBILE defaults ON everywhere, so the mobile
# shell is buildable on a desktop host), and two modules claiming one URI would
# have the linker decide which Main.qml the binary gets.
qt_add_library(codeharbor_mobile_qml STATIC)

# MobileTheme.qml is this module's single colour/metric vocabulary. It must be
# marked a QML singleton BEFORE qt_add_qml_module() reads the file list, and this
# property must agree with the `pragma Singleton` at the top of the file — if
# either side is missing, `import CodeHarbor.Mobile` still succeeds and
# `MobileTheme.x` silently resolves to undefined, which in QML is a blank colour
# rather than an error.
set_source_files_properties(MobileTheme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# EndpointField.js is the SINGLE copy of the endpoint-field rule, shared verbatim
# with the desktop module (src/qml/EndpointField.js) rather than forked: a second
# drifting copy is exactly how a value this page accepts becomes a profile the
# store silently rejects. It lives outside this directory, so its resource path
# has to be stated explicitly — without the alias, qt_add_qml_module derives the
# path from the file's location relative to THIS directory and the `../../`
# prefix makes `import "EndpointField.js"` unresolvable at runtime.
#
# Source-file properties are directory-scoped, so this alias is invisible to
# src/qml's own module, which keeps embedding the file under its own URI.
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/qml/EndpointField.js
                           PROPERTIES QT_RESOURCE_ALIAS EndpointField.js)

qt_add_qml_module(codeharbor_mobile_qml
    URI CodeHarbor.Mobile
    VERSION 1.0
    # Output dir must end in the module target path (URI) so qmllint/qmlls
    # tooling resolves the module correctly.
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/CodeHarbor/Mobile
    QML_FILES
        MobileTheme.qml
        MobileMain.qml
        MobileBusy.qml
        MobileErrorBar.qml
        SessionPickerPage.qml
        PanePickerPage.qml
        PaneHostPage.qml
        TerminalPage.qml
        KeyBar.qml
        ViewerTextPage.qml
        ViewerMarkdownPage.qml
        ViewerImagePage.qml
        ViewerDirectoryPage.qml
        ViewerEditorPage.qml
        ViewerUnsupportedPage.qml
        ConnectPage.qml
        KeyImportSheet.qml
        HostTrustSheet.qml
        ${CMAKE_SOURCE_DIR}/src/qml/EndpointField.js
)

target_link_libraries(codeharbor_mobile_qml PUBLIC
    Qt6::Quick
    Qt6::QuickControls2
)

# The two pages whose IMPORTS only exist in some builds. They cannot sit in the
# QML_FILES list above: `import QtQuick.Pdf` / `import QtWebView` in a compiled
# QML file fails the whole module when the optional Qt package was not found, so
# a build without QtPdf would lose every page rather than just the PDF one. The
# matching QML-side gate is ch::MobileCapabilities (hasPdf / hasWebView), which
# is what stops anything ROUTING here on such a build; this is the compile-side
# half of the same gate.
if(CH_HAVE_QTPDF AND TARGET Qt6::PdfQuick)
    qt_target_qml_sources(codeharbor_mobile_qml QML_FILES ViewerPdfPage.qml)
endif()
if(CH_HAVE_QTWEBVIEW AND TARGET Qt6::WebViewQuick)
    qt_target_qml_sources(codeharbor_mobile_qml QML_FILES ViewerWebPage.qml)
endif()
