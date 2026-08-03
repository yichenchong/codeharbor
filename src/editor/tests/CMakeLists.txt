qt_add_executable(tst_editorcontroller tst_editorcontroller.cpp)

target_link_libraries(tst_editorcontroller PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
    ch_editor
)

add_test(NAME tst_editorcontroller COMMAND tst_editorcontroller)

# LIVE gate for the editor workstream (docs/PLAN.md E): the REAL packaged Monaco
# page in a WebEngineView, bridged over a REAL QWebChannel to the REAL
# ch::EditorController, whose file.* calls cross a REAL ssh channel into a REAL
# remote codeharbord. Registered unconditionally and labelled `live`; the test
# QSKIPs when CH_LIVE_SSH is unset so the default `ctest` run stays green
# everywhere.
#
# The environment is the headless recipe this box needs: Chromium off the GPU and
# out of its sandbox, Quick on the software renderer because there is no display.
#
# --single-process is DELIBERATELY ABSENT: Chromium aborts ("Single mode supports
# only single profile") the moment a second QWebEngineProfile exists, and the
# viewer stack (which supplies this pane's privileged profile) creates two by
# design (SPEC 7.3).
set(CODEHARBOR_LIVE_EDITOR_TEST_ENV
    "QT_QPA_PLATFORM=offscreen"
    "QT_QUICK_BACKEND=software"
    "QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --no-sandbox --disable-dev-shm-usage"
)

qt_add_executable(tst_liveeditor tst_liveeditor.cpp)

target_link_libraries(tst_liveeditor PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::WebEngineQuick
    # qwebchannel.js, which the packaged page loads from qrc:///qtwebchannel/.
    Qt6::WebChannel
    # Linking the generated plugin registers the CodeHarbor QML module and its
    # qrc resources, so both qrc:/qt/qml/CodeHarbor/EditorPaneView.qml and the
    # embedded bundle at qrc:/codeharbor/web/editor/index.html resolve. The
    # targets are defined in src/qml, which CMake adds after this directory;
    # target references are resolved at generate time, so that is fine.
    codeharbor_qmlplugin
    codeharbor_qml
    ch_editor
    # ViewerModel/ViewerProfiles: the privileged WebEngine profile the pane binds.
    ch_viewers
    # SessionBootstrap: the production pool -> channel -> RPC-client wiring.
    ch_app
    ch_ssh
    ch_remote
    ch_models
)

add_test(NAME tst_liveeditor COMMAND tst_liveeditor)
set_tests_properties(tst_liveeditor PROPERTIES
    LABELS live
    ENVIRONMENT "${CODEHARBOR_LIVE_EDITOR_TEST_ENV}"
    TIMEOUT 900
)
set_tests_properties(tst_liveeditor PROPERTIES
    FIXTURES_REQUIRED codeharbor_live_fixture)

# LIVE gate for an OPEN EDITOR across a dropped-and-restored SSH session
# (SPEC 5.6 + 8.7). file.watch subscriptions live inside the codeharbord
# PROCESS (remote/src/files.ts FileWatchService), so a reconnect replaces them
# with nothing; this gate kills a real session and requires the editor to come
# back seeing external changes. No WebEngine here — the controller is driven
# directly, so the failure it reports is a reconnect failure and not a page
# failure — hence no Chromium flags and a much smaller timeout than
# tst_liveeditor. QSKIPs without CH_LIVE_SSH.
qt_add_executable(tst_liveeditorreconnect tst_liveeditorreconnect.cpp)

target_link_libraries(tst_liveeditorreconnect PRIVATE
    Qt6::Test
    Qt6::Core
    # ch_app links Qt6::Gui.
    Qt6::Gui
    ch_editor
    # SessionBootstrap: the production pool -> channel -> RPC-client wiring,
    # and the reconnect ladder this gate drops the session through.
    ch_app
    ch_ssh
    ch_remote
    ch_agent
    ch_models
)

add_test(NAME tst_liveeditorreconnect COMMAND tst_liveeditorreconnect)
set_tests_properties(tst_liveeditorreconnect PROPERTIES
    LABELS live
    TIMEOUT 600
)
set_tests_properties(tst_liveeditorreconnect PROPERTIES
    FIXTURES_REQUIRED codeharbor_live_fixture)

# Console-mode on Windows, for the same reason as src/app/tests: qt_add_executable
# marks executables as GUI subsystem, which on Windows swallows QTest's stdout and
# stderr, so a failing assertion reports nothing at all through ctest.
# tst_liveeditor drives a real QML/WebEngine window and is left as a GUI executable.
if(WIN32)
    set_target_properties(
        tst_editorcontroller
        tst_liveeditorreconnect
        PROPERTIES WIN32_EXECUTABLE FALSE)
endif()
