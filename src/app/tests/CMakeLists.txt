qt_add_executable(tst_appcontroller tst_appcontroller.cpp)
target_link_libraries(tst_appcontroller PRIVATE
    ch_app
    Qt6::Test
    Qt6::Core
    Qt6::Gui
)
add_test(NAME tst_appcontroller COMMAND tst_appcontroller)

# Connection profiles (SPEC 4.1/12.1) plus the QML sheet that edits them. The
# sheet is loaded straight from the source tree — it is not in the QML module
# yet — so the path is compiled in; Quick/QuickControls2 provide the QQuickView
# and the Basic style it is written against. Headless recipe pinned like the
# other QML-touching targets (no WebEngine here, so no Chromium flags).
qt_add_executable(tst_serverprofiles tst_serverprofiles.cpp)
target_link_libraries(tst_serverprofiles PRIVATE
    ch_app
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    # ConnectSheet imports the CodeHarbor module for the shared Theme singleton.
    # Linking the generated plugin registers that module for this source-loaded
    # test just as it is registered for the shipped application.
    codeharbor_qmlplugin
)
target_compile_definitions(tst_serverprofiles PRIVATE
    CH_CONNECTSHEET_QML="${CMAKE_CURRENT_SOURCE_DIR}/../../qml/ConnectSheet.qml"
)
add_test(NAME tst_serverprofiles COMMAND tst_serverprofiles)
set_tests_properties(tst_serverprofiles PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    TIMEOUT 120
)

# Persisted per-region split layouts (SPEC 4.5). Qt6::Network provides the
# QLocalSocket pair that stands in for codeharbord and Qt6::Qml the engine that
# re-checks the published trees through the very accessors ViewerRegion.qml
# uses; the final case is a LIVE round-trip through SessionBootstrap against the
# real fixture and QSKIPs when CH_LIVE_SSH is unset, hence the `live` label (the
# label only FILTERS, so the fake-server cases still run in the default suite).
qt_add_executable(tst_sessionlayouts tst_sessionlayouts.cpp)
target_link_libraries(tst_sessionlayouts PRIVATE
    ch_app
    ch_ssh
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
)
add_test(NAME tst_sessionlayouts COMMAND tst_sessionlayouts)
set_tests_properties(tst_sessionlayouts PROPERTIES LABELS live TIMEOUT 120)

# SessionBootstrap's connection-state machine and reconnect ladder (SPEC 5.6),
# plus the bounded connect pre-flight and the GUI-responsiveness cases that
# came out of the connect-stall hunt. Channel loss is driven through overridden
# test seams, so it needs no SSH server and no fixture; the backoff is
# time-scaled down and the pre-flight cases run against loopback QTcpServers on
# a 300 ms budget, so the whole target still runs in a couple of seconds.
# Qt6::Network is what QTcpServer/QTcpSocket come from.
qt_add_executable(tst_sessionbootstrap tst_sessionbootstrap.cpp)
target_link_libraries(tst_sessionbootstrap PRIVATE
    ch_app
    ch_ssh
    ch_remote
    ch_agent
    Qt6::Test
    Qt6::Core
    Qt6::Network
)
add_test(NAME tst_sessionbootstrap COMMAND tst_sessionbootstrap)

# LIVE reconnect gate: a real SSH session, really dropped (SPEC 5.6). QSKIPs
# without CH_LIVE_SSH. Sized for a 1 s backoff rung plus two cold node starts.
qt_add_executable(tst_livereconnect tst_livereconnect.cpp)
target_link_libraries(tst_livereconnect PRIVATE
    ch_app
    ch_ssh
    ch_remote
    ch_agent
    Qt6::Test
    Qt6::Core
)
add_test(NAME tst_livereconnect COMMAND tst_livereconnect)
set_tests_properties(tst_livereconnect PROPERTIES LABELS live TIMEOUT 300)

# Notifier (SPEC 6.2). tst_notifier forces a bus-less environment in its own
# main() so it exercises the headless no-op path everywhere; tst_notifierlive
# is the only proof that a notification reaches a real display.
qt_add_executable(tst_notifier tst_notifier.cpp)
target_link_libraries(tst_notifier PRIVATE ch_app Qt6::Test Qt6::Core)
add_test(NAME tst_notifier COMMAND tst_notifier)

# tst_notifierlive needs a session bus with an org.freedesktop.Notifications
# daemon. Two properties keep a missing daemon from reading as a proof:
#
#  * LABELS desktop, NOT live. Every `live` target needs the SSH fixture; this
#    one needs a desktop session and nothing else, so `ctest -L live` must not
#    imply notifications were exercised. Demand the proof with `-L desktop`.
#    (Unlabelled runs such as `ctest --preset dev` still include it.)
#  * SKIP_REGULAR_EXPRESSION on the banner the test prints when no daemon is
#    present, so ctest reports Skipped instead of Passed. Failing outright
#    would be worse — a daemon-less box is legitimate — but a silent pass is
#    how unproven capability ships. Keep this string identical to
#    kNotVerifiedToken in tst_notifierlive.cpp.
qt_add_executable(tst_notifierlive tst_notifierlive.cpp)
target_link_libraries(tst_notifierlive PRIVATE ch_app Qt6::Test Qt6::Core)
add_test(NAME tst_notifierlive COMMAND tst_notifierlive)
set_tests_properties(tst_notifierlive PROPERTIES
    LABELS desktop
    SKIP_REGULAR_EXPRESSION "NOTIFICATION DELIVERY NOT VERIFIED"
)

# LIVE gate (docs/PLAN.md U): the real shell against a real codeharbord over a
# real SSH session, plus two launches of the real GUI binary. The SSH cases
# QSKIP when CH_LIVE_SSH is unset so the default `ctest` run stays green
# everywhere; the in-process QML region-width case needs no fixture and always
# runs.
#
# ch_headlessquit is LD_PRELOAD'ed into the relaunched GUI binary so it can be
# asked to quit through its own event loop; see headlessquit.cpp.
add_library(ch_headlessquit MODULE headlessquit.cpp)
set_target_properties(ch_headlessquit PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(ch_headlessquit PRIVATE Qt6::Core)

qt_add_executable(tst_liveshell tst_liveshell.cpp)
target_link_libraries(tst_liveshell PRIVATE
    ch_app
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::WebEngineQuick
    Qt6::WebChannel
    # Linking the generated plugin registers the CodeHarbor QML module, so the
    # gate can load the very same qrc:/qt/qml/CodeHarbor/Main.qml the shipped
    # binary loads and assert on the restored region widths.
    codeharbor_qmlplugin
    ch_models
    ch_persistence
    ch_remote
    ch_ssh
    ch_viewers
    ch_editor
    ch_agent
)
target_compile_definitions(tst_liveshell PRIVATE
    CH_LIVESHELL_APP="$<TARGET_FILE:codeharbor>"
    CH_LIVESHELL_QUIT_SHIM="$<TARGET_FILE:ch_headlessquit>"
)
add_dependencies(tst_liveshell codeharbor ch_headlessquit)
add_test(NAME tst_liveshell COMMAND tst_liveshell)
# LABELS live selects the SSH cases; they QSKIP without CH_LIVE_SSH. The QML
# region-width case needs no fixture and therefore runs in the default suite
# too, so the headless recipe is pinned here (same as src/qml/tests).
set_tests_properties(tst_liveshell PROPERTIES
    LABELS live
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --no-sandbox --disable-dev-shm-usage"
    TIMEOUT 300
)

# THE COLD-START ACCEPTANCE GATE (tst_coldstart.cpp): a fresh config dir, the
# real qrc Main.qml on top of main.cpp's exact object graph, and the whole
# walkthrough — connect sheet, host key, workspace, live terminal pane, remote
# file save, relaunch — driven through the UI objects.
#
# It links everything the shipped binary links (codeharbor_qmlplugin registers
# the CodeHarbor QML module; ch_terminal/ch_editor/ch_viewers provide the pane
# factories the QML instantiates) and needs the same headless WebEngine recipe
# as the other WebEngine-touching targets. NEVER add --single-process: Chromium
# aborts as soon as a second QWebEngineProfile exists, and step 7 builds a
# second graph — hence a second profile — on purpose.
#
# LABELS live: initTestCase QSKIPs the whole gate without CH_LIVE_SSH. The
# timeout covers two cold `node` starts, two SSH handshakes, a cold tmux server
# and two full QML/WebEngine launches.
qt_add_executable(tst_coldstart tst_coldstart.cpp)
target_link_libraries(tst_coldstart PRIVATE
    ch_app
    ch_agent
    ch_editor
    ch_models
    ch_persistence
    ch_remote
    ch_ssh
    ch_terminal
    ch_viewers
    codeharbor_qmlplugin
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::WebChannel
    Qt6::WebEngineQuick
)
add_test(NAME tst_coldstart COMMAND tst_coldstart)
set_tests_properties(tst_coldstart PROPERTIES
    LABELS live
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --no-sandbox --disable-dev-shm-usage"
    TIMEOUT 900
)
