qt_add_executable(tst_viewers tst_viewers.cpp)

target_link_libraries(tst_viewers PRIVATE
    ch_viewers
    # QLocalServer/QLocalSocket: the local RPC harness that drives ViewerModel's
    # file.readFile round trips without a server.
    Qt6::Network
    Qt6::Test
)

add_test(NAME tst_viewers COMMAND tst_viewers)

# LIVE gate for the viewer workstream (docs/PLAN.md V): renders REAL remote
# bytes through the codeharbor-internal:// scheme and lays out the REAL
# recursive ViewerRegion split tree, headless, against a real sshd and a real
# codeharbord. Registered unconditionally and labelled `live`; the test QSKIPs
# when CH_LIVE_SSH is unset so the default `ctest` run stays green everywhere.
#
# The environment is the headless recipe this box needs: Chromium must stay off
# the GPU and out of its sandbox, and Quick must use the software renderer
# because there is no display.
#
# --single-process is DELIBERATELY ABSENT: Chromium refuses it the moment a
# second QWebEngineProfile exists — it aborts the process outright with
# "Single mode supports only single profile". The viewer stack creates two by
# design (sandboxed external + privileged internal, SPEC 7.3), and the default
# profile a WebEngineView materialises before its `profile` binding resolves is
# enough on its own. Everything here still runs fully headless without it, and
# Chromium's raster still composites into QQuickWindow::grabWindow(); only the
# renderer is out-of-process.
set(CODEHARBOR_LIVE_VIEWER_TEST_ENV
    "QT_QPA_PLATFORM=offscreen"
    "QT_QUICK_BACKEND=software"
    "QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --no-sandbox --disable-dev-shm-usage"
)

qt_add_executable(tst_liveviewers tst_liveviewers.cpp)

target_link_libraries(tst_liveviewers PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::WebEngineQuick
    # Linking the generated plugin registers the CodeHarbor QML module and its
    # qrc resources, so qrc:/qt/qml/CodeHarbor/ViewerRegion.qml resolves. The
    # target is defined in src/qml, which CMake adds after this directory;
    # target references are resolved at generate time, so that is fine.
    codeharbor_qmlplugin
    ch_viewers
    # SessionBootstrap: the production pool -> channel -> RPC-client wiring.
    ch_app
    ch_ssh
    ch_remote
)

add_test(NAME tst_liveviewers COMMAND tst_liveviewers)
set_tests_properties(tst_liveviewers PROPERTIES
    LABELS live
    ENVIRONMENT "${CODEHARBOR_LIVE_VIEWER_TEST_ENV}"
    TIMEOUT 900
)

# Console-mode on Windows, for the same reason as src/app/tests: qt_add_executable
# marks executables as GUI subsystem, which on Windows swallows QTest's stdout and
# stderr, so a failing assertion reports nothing at all through ctest.
# tst_liveviewers drives a real QML/WebEngine window and is left as a GUI executable.
if(WIN32)
    set_target_properties(tst_viewers PROPERTIES WIN32_EXECUTABLE FALSE)
endif()
