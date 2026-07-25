qt_add_executable(tst_terminalcontroller tst_terminalcontroller.cpp)
target_link_libraries(tst_terminalcontroller PRIVATE Qt6::Test ch_terminal)
add_test(NAME tst_terminalcontroller COMMAND tst_terminalcontroller)

# Per-pane plumbing the QML terminal pane is built on: TerminalFactory
# (ownership, attach refusal, kill command) + TerminalBridge (the WebChannel
# face of a pane). No server needed — attaching a real PTY is the live gate
# below.
qt_add_executable(tst_terminalfactory tst_terminalfactory.cpp)
target_link_libraries(tst_terminalfactory PRIVATE
    Qt6::Core
    Qt6::Test
    ch_terminal
    ch_ssh
)
add_test(NAME tst_terminalfactory COMMAND tst_terminalfactory)

# LIVE gate (docs/PLAN.md T): a real tmux session over a real SSH PTY, driven
# through the production TerminalController transport seam. Registered
# unconditionally and labelled `live`; the test QSKIPs when CH_LIVE_SSH is unset
# so the default `ctest` run stays green everywhere.
qt_add_executable(tst_liveterminal tst_liveterminal.cpp)
target_link_libraries(tst_liveterminal PRIVATE
    Qt6::Core
    Qt6::Test
    ch_terminal
    ch_ssh
)
add_test(NAME tst_liveterminal COMMAND tst_liveterminal)
set_tests_properties(tst_liveterminal PROPERTIES LABELS live)

# LIVE gate: the production attach path (TerminalFactory -> real PTY channel ->
# real tmux -> TerminalBridge), including detach/kill against the real server.
# Same rules as tst_liveterminal: labelled `live`, QSKIPs without CH_LIVE_SSH.
qt_add_executable(tst_liveterminalfactory tst_liveterminalfactory.cpp)
target_link_libraries(tst_liveterminalfactory PRIVATE
    Qt6::Core
    Qt6::Test
    ch_terminal
    ch_ssh
)
add_test(NAME tst_liveterminalfactory COMMAND tst_liveterminalfactory)
set_tests_properties(tst_liveterminalfactory PROPERTIES LABELS live)

# The packaged xterm.js page inside the REAL TerminalPaneView, over a REAL
# QWebChannel. Needs no server, but it needs Chromium: labelled `live` and
# QSKIPping when WebEngine cannot run, exactly like tst_webengine_headless.
#
# The environment is this box's headless recipe: Chromium off the GPU and out
# of its sandbox, Quick on the software renderer because there is no display.
# --single-process is DELIBERATELY ABSENT: Chromium aborts ("Single mode
# supports only single profile") once a second QWebEngineProfile exists, and
# the viewer stack creates two by design (SPEC 7.2).
set(CODEHARBOR_TERMINAL_PAGE_TEST_ENV
    "QT_QPA_PLATFORM=offscreen"
    "QT_QUICK_BACKEND=software"
    "QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --no-sandbox --disable-dev-shm-usage"
)

qt_add_executable(tst_terminalpage tst_terminalpage.cpp)
target_link_libraries(tst_terminalpage PRIVATE
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
    # qrc resources, so both qrc:/qt/qml/CodeHarbor/TerminalPaneView.qml and the
    # embedded bundle at qrc:/codeharbor/web/terminal/index.html resolve. The
    # targets live in src/qml, which CMake adds after this directory; target
    # references are resolved at generate time, so that is fine.
    codeharbor_qmlplugin
    codeharbor_qml
    ch_terminal
    # ViewerModel/ViewerProfiles: the privileged WebEngine profile the pane binds.
    ch_viewers
    ch_remote
    ch_models
    ch_ssh
)
add_test(NAME tst_terminalpage COMMAND tst_terminalpage)
set_tests_properties(tst_terminalpage PROPERTIES
    LABELS live
    ENVIRONMENT "${CODEHARBOR_TERMINAL_PAGE_TEST_ENV}"
    TIMEOUT 300
)
