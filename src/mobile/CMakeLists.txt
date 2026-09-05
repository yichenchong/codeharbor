# The mobile presentation layer (Android + iOS), plus the desktop-hosted build
# of the same shell so it is developable and testable without a device.
#
# WHY THIS EXISTS AS A SECOND SHELL RATHER THAN A RESPONSIVE MODE OF src/qml:
# every rich desktop surface is a Qt WebEngine view (TerminalPaneView,
# EditorPaneView and the four Viewer* views all host a QWebEngineView), and
# WebEngine ships on neither Android nor iOS. So the mobile client cannot reuse
# one line of the desktop QML, while EVERY layer beneath it — ch_models,
# ch_remote, ch_persistence, ch_ssh, ch_agent, ch_terminal, ch_editor, ch_app —
# is portable as written and is reused here unchanged. This directory is
# therefore the whole of the port: a native Qt Quick presentation over the
# existing spine.
#
# NOTHING here may link, include or import Qt WebEngine, Qt WebChannel or
# QtWidgets (SPEC 7.5 / 2.4). ch_viewers is deliberately absent from the link
# list below for exactly that reason; the Qt-Core-only URL/MIME classification
# it used to hide is available as ch_viewers_core.
#
# QtDBus is the one entry on that list with a caveat worth stating rather than
# claiming falsely: ch_app links Qt6::DBus PUBLIC when a configure finds it
# (Notifier's Linux desktop notifications), so a mobile shell built on a Linux
# WORKSTATION does inherit it through ch_app. That is the development host, not
# a shipped client: Qt provides no QtDBus for Android or iOS, so a device
# configure compiles the same ch_app with CH_HAVE_DBUS=0 and links nothing.
# Nothing in this directory may reference QtDBus itself.
qt_add_library(ch_mobile STATIC
    MobileAppController.h
    MobileAppController.cpp
    MobileCapabilities.h
    MobileCapabilities.cpp
    PaneListModel.h
    PaneListModel.cpp
    MobileTerminalSession.h
    MobileTerminalSession.cpp
    MobileTerminalView.h
    MobileTerminalView.cpp
    MobileViewerService.h
    MobileViewerService.cpp
    MobileImageProvider.h
    MobileImageProvider.cpp
    MarkdownModel.h
    MarkdownModel.cpp
    # Credentials. MobileKeyStore holds key material in memory by default,
    # persists only a REFERENCE to a file the user manages, and — for the keys the
    # user explicitly asks it to — keeps one owner-only copy in app-private
    # storage, which is the shape SPEC 11.2's allowlist admits.
    MobileKeyStore.h
    MobileKeyStore.cpp
    MobileKeyReference.h
    MobileKeyReference.cpp
)

target_include_directories(ch_mobile PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(ch_mobile PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Qml
    Qt6::Network
    ch_vt
    ch_app
    # The Qt-Core-only half of the viewer subsystem: URL/MIME classification,
    # which is what decides which single pane the mobile shell shows.
    ch_viewers_core
    ch_terminal
    ch_editor
    ch_remote
    ch_models
    ch_persistence
    ch_ssh
    ch_agent
)

# ch_mobile is ALSO a QML module — one with no QML files, whose entire purpose is
# registering the C++ types the shell's QML names: ch::MobileAppController (whose
# NavStage enumerators drive the whole stage -> page mapping),
# ch::MobileTerminalView, ch::MobileTerminalSession, ch::PaneListModel and
# ch::MobileCapabilities. Each declares QML_ELEMENT, so the registration is
# STATIC and generated at build time.
#
# WHY NOT runtime qmlRegister* calls from main.cpp, which is where this started:
# src/mobile/qml is a COMPILED QML module, and Qt marks a compiled module's URI
# PROTECTED. A runtime registration into it is REFUSED ("Cannot install element
# ... into protected module") — and refused quietly: the shell still loaded, and
# only whichever type a QML file happened to name first still resolved. The
# terminal session type was the one that lost, so the terminal pane appeared and
# could never attach, with the reason in a warning nobody reads.
#
# The SEPARATE URI is therefore deliberate: CodeHarbor.Mobile is the QML module
# (src/mobile/qml), CodeHarbor.Mobile.Core is this C++ type module. Two targets
# cannot share one URI.
qt_add_qml_module(ch_mobile
    URI CodeHarbor.Mobile.Core
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/CodeHarbor/Mobile/Core
)

# Feature probes owned by the root CMakeLists (optional find_package of Qt6::Pdf
# and Qt6::WebView). Normalised here rather than trusted: an UNDEFINED variable
# would expand to `CH_HAVE_QTPDF=`, which is not a preprocessor-legal definition
# and fails the compile with a message about neither Qt nor PDF.
if(NOT DEFINED CH_HAVE_QTPDF OR CH_HAVE_QTPDF STREQUAL "")
    set(CH_HAVE_QTPDF 0)
endif()
if(NOT DEFINED CH_HAVE_QTWEBVIEW OR CH_HAVE_QTWEBVIEW STREQUAL "")
    set(CH_HAVE_QTWEBVIEW 0)
endif()

target_compile_definitions(ch_mobile PUBLIC
    CH_HAVE_QTPDF=${CH_HAVE_QTPDF}
    CH_HAVE_QTWEBVIEW=${CH_HAVE_QTWEBVIEW}
)

# The two optional Qt modules are linked into ch_mobile (PUBLIC, so the
# executable inherits them) rather than into the QML module: a QML `import
# QtQuick.Pdf` / `QtWebView` only resolves at runtime if the module's plugin was
# linked into the binary, and it is the binary, not the QML library, that Qt
# scans for statically linked plugins.
if(CH_HAVE_QTPDF AND TARGET Qt6::PdfQuick)
    target_link_libraries(ch_mobile PUBLIC Qt6::PdfQuick)
endif()
# Both halves of Qt WebView: Qt6::WebView is the C++ wrapper main.cpp needs, and
# Qt6::WebViewQuick is the target that CARRIES the `QtWebView` QML module the
# page imports. Linking only the former leaves the import unresolvable in a
# statically linked binary (Qt for iOS), which is a blank page at runtime.
if(CH_HAVE_QTWEBVIEW AND TARGET Qt6::WebView AND TARGET Qt6::WebViewQuick)
    target_link_libraries(ch_mobile PUBLIC Qt6::WebView Qt6::WebViewQuick)
endif()

# Resolving a persisted key reference back to bytes is platform work: an Android
# SAF document URI and an Apple security-scoped bookmark are the two ways to
# READ a user-managed file without copying it into app storage. The Apple half is
# Objective-C++ (Foundation/NSURL bookmarks), so it is a separate .mm file;
# MobileKeyReference.cpp carries the generic path and the Android implementation.
#
# IOS, not APPLE. Every function in that file is reached only under Q_OS_IOS —
# MobileKeyReference.cpp guards all three call sites — because a security-scoped
# bookmark is how a sandboxed iOS app keeps access to a document the user picked.
# On a macOS desktop build of this shell a reference is an ordinary path and the
# generic implementation handles it, so compiling this file for macOS built
# Objective-C++ that nothing could call. It is also how a real compile error in
# it first surfaced in the macOS DESKTOP job instead of in an iOS one.
if(IOS)
    target_sources(ch_mobile PRIVATE MobileKeyReferenceIos.mm)
    set_source_files_properties(MobileKeyReferenceIos.mm
                               PROPERTIES COMPILE_FLAGS "-fobjc-arc")

    # The other half of the Apple-only credential work, gated on IOS for exactly
    # the reasons above: NSURLIsExcludedFromBackupKey on a saved key file. Only
    # iOS needs it — Android excludes its whole sandbox through
    # android:allowBackup="false" in packaging/android/AndroidManifest.xml, and a
    # macOS desktop build of this shell is a development host rather than a
    # client whose backups matter — so compiling it anywhere else would build
    # Objective-C++ nothing could call.
    target_sources(ch_mobile PRIVATE MobileKeyStoreIos.mm)
    set_source_files_properties(MobileKeyStoreIos.mm
                               PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()

add_subdirectory(qml)

qt_add_executable(codeharbor_mobile
    main.cpp
)

target_link_libraries(codeharbor_mobile PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Qml
    Qt6::QuickControls2
    # Linking the generated plugin is what registers the CodeHarbor.Mobile QML
    # module, so main.cpp can load qrc:/qt/qml/CodeHarbor/Mobile/MobileMain.qml.
    codeharbor_mobile_qmlplugin
    # ch_mobile's own QML module plugin: linking it is what registers the C++
    # types (CodeHarbor.Mobile.Core) the shell's QML names.
    ch_mobileplugin
    ch_mobile
    ch_vt
    ch_app
    ch_viewers_core
    ch_models
    ch_persistence
    ch_remote
    ch_ssh
    ch_terminal
    ch_editor
    ch_agent
)

target_compile_definitions(codeharbor_mobile PRIVATE
    CODEHARBOR_VERSION="${PROJECT_VERSION}"
)

# WIN32_EXECUTABLE / MACOSX_BUNDLE are deliberately NOT set unconditionally the
# way src/app does: this target is also built on desktop hosts purely so the
# mobile shell can be developed and tested there, and a Windows GUI-subsystem
# binary swallows the QML warnings that are the only diagnostic a bare QML load
# failure produces. The two properties that ARE needed are per-platform and set
# below.
# The application identifier, shared by both platforms and defaulted in the root
# CMakeLists. Re-defaulted here for the same reason the feature probes are: an
# empty value would silently produce an unsigned, unlaunchable bundle rather
# than a configure error.
if(NOT DEFINED CODEHARBOR_MOBILE_BUNDLE_ID OR CODEHARBOR_MOBILE_BUNDLE_ID STREQUAL "")
    set(CODEHARBOR_MOBILE_BUNDLE_ID dev.codeharbor.mobile)
endif()

# Android's two version fields, derived from the project version so they cannot
# drift from the tag. Without these, androiddeployqt substitutes its own
# defaults into the manifest's %%INSERT_VERSION_CODE%%/%%INSERT_VERSION_NAME%%
# placeholders, and 0.4.1 and 0.4.2 both shipped as versionCode 1 /
# versionName 1.0 - verified by decoding the published APKs. The consequences
# are not cosmetic: Play rejects an upload whose versionCode has not increased,
# so the store route was blocked at the first update, and the phone's own
# application list reported 1.0 for every build ever made.
#
# The code must be a single monotonically increasing integer, so it is packed
# as major*10000 + minor*100 + patch (0.4.2 -> 402). That ordering holds for as
# long as minor and patch stay below 100, which is asserted rather than assumed
# because the failure is a release that cannot be superseded.
if(PROJECT_VERSION_MINOR GREATER_EQUAL 100 OR PROJECT_VERSION_PATCH GREATER_EQUAL 100)
    message(FATAL_ERROR
        "Version ${PROJECT_VERSION} breaks the Android versionCode packing: "
        "major*10000 + minor*100 + patch stops increasing once minor or patch "
        "reaches 100. Widen the packing before releasing this version.")
endif()
math(EXPR CODEHARBOR_ANDROID_VERSION_CODE
     "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")

if(ANDROID)
    # AndroidManifest.xml and res/ live here (packaging/android). Qt copies the
    # directory over its own generated template, so the manifest in the tree is
    # authoritative for permissions, the application id and the icon set. No
    # build.gradle or gradle.properties is shipped beside it on purpose: Qt's
    # own templates are used unmodified, and the SDK levels below are what keeps
    # the generated gradle.properties in step with the manifest instead of
    # silently disagreeing with it.
    set_target_properties(codeharbor_mobile PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR ${CMAKE_SOURCE_DIR}/packaging/android
        QT_ANDROID_MIN_SDK_VERSION 28
        QT_ANDROID_TARGET_SDK_VERSION 35
        QT_ANDROID_VERSION_NAME ${PROJECT_VERSION}
        QT_ANDROID_VERSION_CODE ${CODEHARBOR_ANDROID_VERSION_CODE}
    )

    # The manifest carries the application id LITERALLY: androiddeployqt reads
    # the `package` attribute out of it and writes that string into
    # gradle.properties as androidPackageName, and there is no CMake property
    # that can supply it instead. The iOS bundle identifier, by contrast, is
    # substituted from CODEHARBOR_MOBILE_BUNDLE_ID. One identifier with two
    # spellings is the kind of divergence nobody notices until an APK and an IPA
    # claim to be different applications, so the configure reads the attribute
    # back and refuses to disagree with it.
    file(READ ${CMAKE_SOURCE_DIR}/packaging/android/AndroidManifest.xml _ch_manifest)
    if(NOT _ch_manifest MATCHES "package=\"([A-Za-z0-9_.]+)\"")
        message(FATAL_ERROR
            "packaging/android/AndroidManifest.xml has no `package` attribute; "
            "androiddeployqt would fall back to org.qtproject.example.")
    elseif(NOT "${CMAKE_MATCH_1}" STREQUAL "${CODEHARBOR_MOBILE_BUNDLE_ID}")
        message(FATAL_ERROR
            "packaging/android/AndroidManifest.xml declares package=\""
            "${CMAKE_MATCH_1}\" but CODEHARBOR_MOBILE_BUNDLE_ID is \""
            "${CODEHARBOR_MOBILE_BUNDLE_ID}\". The Android application id and "
            "the iOS bundle identifier are one identifier; edit the manifest to "
            "match, or set -DCODEHARBOR_MOBILE_BUNDLE_ID to the manifest value.")
    endif()
    unset(_ch_manifest)
elseif(IOS)
    # The plist template uses @-style substitutions (@PROJECT_VERSION@,
    # @CODEHARBOR_MOBILE_BUNDLE_ID@, @CMAKE_OSX_DEPLOYMENT_TARGET@), so @ONLY:
    # it also contains literal Xcode build settings spelled $(EXECUTABLE_NAME),
    # which CMake's ${} substitution would eat if it were enabled.
    configure_file(
        ${CMAKE_SOURCE_DIR}/packaging/ios/Info.plist.in
        ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
        @ONLY
    )

    # Qt's iOS finalizer otherwise copies its own LaunchScreen.storyboard into
    # the bundle and substitutes the storyboard's name into the plist through
    # @qt_ios_launch_screen_plist_entry@. This plist has no such placeholder -
    # it declares the modern key-only launch screen (an empty UILaunchScreen
    # dictionary, valid since iOS 14 and well below the 17.0 deployment target),
    # so Qt's storyboard would ship unreferenced. Opting out keeps the bundle to
    # exactly what the plist declares.
    set(QT_NO_SET_DEFAULT_IOS_LAUNCH_SCREEN TRUE)
    set_target_properties(codeharbor_mobile PROPERTIES
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${CODEHARBOR_MOBILE_BUNDLE_ID}
    )
endif()

if(CODEHARBOR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
