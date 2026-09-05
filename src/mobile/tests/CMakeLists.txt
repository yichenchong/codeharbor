# Mobile shell unit gates. Every target here is headless and needs no SSH
# server, no fixture and no device: the RPC peer is driven over a QLocalSocket
# pair standing in for codeharbord, exactly as src/app/tests does.

# Flattening the two server-authoritative split trees into the single-pane
# picker list: order, paneKey shape, titles, kinds, and the malformed/over-deep
# trees that must yield an empty model rather than a crash. Pure model logic, so
# QtCore and QtGui (QRgb/QGuiApplication-free QTest) are all it needs.
qt_add_executable(tst_panelist tst_panelist.cpp)
target_link_libraries(tst_panelist PRIVATE
    ch_mobile
    Qt6::Test
    Qt6::Core
    Qt6::Gui
)
add_test(NAME tst_panelist COMMAND tst_panelist)

# The two-step navigation itself: Servers -> Sessions -> Panes -> one pane, the
# stale-load discard on rapid session switching, the single-live-pane invariant,
# the UiStateStore round trip, and the server profile store the connect form
# writes through. Driven against the REAL AppController and SessionLayouts over
# an in-process QIODevice standing in for the RPC transport, so it deliberately
# does NOT link Qt6::Network: an in-process device dispatches synchronously,
# which makes response ordering a decision rather than a race. Same reasoning
# (and the same FakeTransport shape) as src/app/tests.
#
# It DOES link the QML module, for two cases only: the affordance that takes the
# user from the Dev Session list back to the connect form is a button on a real
# page, and "the controller's back() moves the stage" says nothing about whether
# anything on screen is bound to it - which is exactly the defect those cases
# were written for. That is also why the target runs offscreen rather than
# guiless; nothing here needs a display or a device.
qt_add_executable(tst_mobilenav tst_mobilenav.cpp)
target_link_libraries(tst_mobilenav PRIVATE
    # Registers CodeHarbor.Mobile (the QML pages), exactly as the executable
    # does; without it loadFromModule() would find nothing.
    codeharbor_mobile_qmlplugin
    # Registers CodeHarbor.Mobile.Core (the C++ types QML names).
    ch_mobileplugin
    ch_mobile
    ch_vt
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)
add_test(NAME tst_mobilenav COMMAND tst_mobilenav)
# Offscreen and the software renderer, for the same reason tst_mobileshell needs
# them: a headless box has no GPU to render a real Quick scene through.
set_tests_properties(tst_mobilenav PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

# The native mobile terminal pane: byte-exact acknowledgeOutput() accounting
# across split flushes (the flow-control contract ch::TerminalBridge used to
# carry), key/paste encoding against the screen's live modes, visibility gating,
# close() detaching rather than killing, and ch::MobileTerminalView's grid
# derivation plus its damage tracking. A QGuiApplication is needed for the
# QQuickPaintedItem and for QFontMetricsF, so the target runs offscreen; no SSH
# session and no device are involved, because the PTY channel is a QIODevice
# stand-in at exactly the seam TerminalController::setTransport() exists for.
qt_add_executable(tst_mobileterminal tst_mobileterminal.cpp)
target_link_libraries(tst_mobileterminal PRIVATE
    ch_mobile
    ch_vt
    ch_terminal
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
)
add_test(NAME tst_mobileterminal COMMAND tst_mobileterminal)
set_tests_properties(tst_mobileterminal PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The viewer data path: ch::MobileViewerService against a fake codeharbord peer,
# covering base64-vs-UTF-8 decoding, truncation reporting, the revision round
# trip on write, listing order, outside-root resolution, the image cache
# handshake and the inline size cap. Qt6::Network supplies the QLocalSocket pair
# that stands in for the RPC transport, exactly as src/remote/tests does.
qt_add_executable(tst_mobileviewers tst_mobileviewers.cpp)
target_link_libraries(tst_mobileviewers PRIVATE
    ch_mobile
    ch_remote
    ch_viewers_core
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Network
)
add_test(NAME tst_mobileviewers COMMAND tst_mobileviewers)
set_tests_properties(tst_mobileviewers PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The Markdown pipeline is a SECURITY gate, not a formatting nicety: server
# bytes must never become markup (SPEC 7.5). The adversarial fixtures — script
# tags, event-handler attributes, javascript:/data: destinations, an image path
# escaping the repository root — are the point of this target.
qt_add_executable(tst_markdownmodel tst_markdownmodel.cpp)
target_link_libraries(tst_markdownmodel PRIVATE
    ch_mobile
    Qt6::Test
    Qt6::Core
    Qt6::Gui
)
add_test(NAME tst_markdownmodel COMMAND tst_markdownmodel)

# Credentials. This target's job is to prove a NEGATIVE: that after a full
# import/authenticate/forget cycle no byte of the key and no byte of the
# passphrase exists anywhere under the app's own storage roots, because SPEC
# 11.2 does not permit the client to store either.
qt_add_executable(tst_mobilekeystore tst_mobilekeystore.cpp)
target_link_libraries(tst_mobilekeystore PRIVATE
    ch_mobile
    ch_ssh
    Qt6::Test
    Qt6::Core
    Qt6::Gui
)
add_test(NAME tst_mobilekeystore COMMAND tst_mobilekeystore)

# The shell as the user meets it: the REAL MobileMain.qml from the real
# CodeHarbor.Mobile module, walked Servers -> Sessions -> Panes -> one pane over
# the same fake codeharbord tst_mobilenav uses. This is the gate that catches
# what a C++-only test cannot — a page that does not load, a nested delegate
# model that shows group headers and no sessions, a kind -> page routing entry
# that names a component that is not there. It also writes frame grabs, so the
# assertions have something a human can look at.
#
# Linking the QML plugin is what registers the module, exactly as the executable
# does; without it loadFromModule() would find nothing.
qt_add_executable(tst_mobileshell tst_mobileshell.cpp)
target_link_libraries(tst_mobileshell PRIVATE
    codeharbor_mobile_qmlplugin
    # Registers CodeHarbor.Mobile.Core, exactly as the executable does.
    ch_mobileplugin
    ch_mobile
    ch_vt
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
)
add_test(NAME tst_mobileshell COMMAND tst_mobileshell)
# Offscreen, and the software renderer: a grabWindow() on a headless box has no
# GPU to render through, and the frames are being inspected for layout, not for
# shader output.
set_tests_properties(tst_mobileshell PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

# Console-mode on Windows for EVERY QTest target here, without exception.
# qt_add_executable marks executables as GUI subsystem, which on Windows
# swallows QTest's stdout and stderr — a failing assertion then reports nothing
# at all through ctest. tst_mobileshell needs it as much as the rest: it draws
# into an offscreen window, but a Windows console subsystem binary can still
# create one, and it is the target whose failures are hardest to guess at.
if(WIN32)
    set_target_properties(
        tst_panelist
        tst_mobilenav
        tst_mobileterminal
        tst_mobileviewers
        tst_markdownmodel
        tst_mobilekeystore
        tst_mobileshell
        PROPERTIES WIN32_EXECUTABLE FALSE)
endif()
