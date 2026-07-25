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
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
)
add_test(NAME tst_sessionlayouts COMMAND tst_sessionlayouts)
set_tests_properties(tst_sessionlayouts PROPERTIES LABELS live TIMEOUT 120)

# SessionBootstrap's connection-state machine and reconnect ladder (SPEC 5.6).
# Drives channel loss through overridden test seams, so it needs no SSH server
# and no fixture; the backoff is time-scaled down and the whole target runs in
# well under a second.
qt_add_executable(tst_sessionbootstrap tst_sessionbootstrap.cpp)
target_link_libraries(tst_sessionbootstrap PRIVATE
    ch_app
    ch_ssh
    ch_remote
    ch_agent
    Qt6::Test
    Qt6::Core
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
# needs a real notification daemon and QSKIPs without one.
qt_add_executable(tst_notifier tst_notifier.cpp)
target_link_libraries(tst_notifier PRIVATE ch_app Qt6::Test Qt6::Core)
add_test(NAME tst_notifier COMMAND tst_notifier)

qt_add_executable(tst_notifierlive tst_notifierlive.cpp)
target_link_libraries(tst_notifierlive PRIVATE ch_app Qt6::Test Qt6::Core)
add_test(NAME tst_notifierlive COMMAND tst_notifierlive)
set_tests_properties(tst_notifierlive PROPERTIES LABELS live)

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
