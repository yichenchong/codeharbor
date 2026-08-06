# Headless environment shared by every test in this directory. Chromium must be
# told to stay off the GPU and out of its sandbox; Quick must use the software
# renderer because there is no display or GL context in CI.
#
# --single-process is deliberately ABSENT: Chromium aborts ("Single mode supports
# only single profile") as soon as a second QWebEngineProfile exists, and the
# viewer stack creates two by design (sandboxed external + privileged internal,
# SPEC 7.3). A WebEngineView's transient default profile can trip it alone, so it
# is unsafe here even though Main.qml's empty panes happen to survive it.
set(CODEHARBOR_QML_TEST_ENV
    "QT_QPA_PLATFORM=offscreen"
    "QT_QUICK_BACKEND=software"
    "QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --no-sandbox --disable-dev-shm-usage"
)
if(WIN32)
    # Qt 6.9 no longer bundles fonts with the SDK. Use the Windows font
    # directory so QFontDatabase does not turn that deployment detail into a
    # false QML warning.
    list(APPEND CODEHARBOR_QML_TEST_ENV "QT_QPA_FONTDIR=$ENV{WINDIR}/Fonts")
endif()

# Regression gate: instantiates the REAL application QML tree the way main.cpp
# does and fails on any QML error/warning. Without this the unit suite can be
# green while the shipped binary cannot start.
qt_add_executable(tst_qmlload tst_qmlload.cpp)

target_link_libraries(tst_qmlload PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::WebEngineQuick
    Qt6::WebChannel
    # Linking the generated plugin registers the CodeHarbor QML module and its
    # qrc resources, so qrc:/qt/qml/CodeHarbor/Main.qml resolves.
    codeharbor_qmlplugin
    ch_app
    ch_models
    ch_persistence
    ch_remote
    ch_viewers
    ch_editor
    ch_agent
)

target_compile_definitions(tst_qmlload PRIVATE
    CODEHARBOR_VERSION="${PROJECT_VERSION}"
)

add_test(NAME tst_qmlload COMMAND tst_qmlload)
set_tests_properties(tst_qmlload PROPERTIES
    ENVIRONMENT "${CODEHARBOR_QML_TEST_ENV}"
    TIMEOUT 180
)

# Identity gate for the split tree (SPEC 4.5): splitting a region must RE-HOME
# the panes already in it, never rebuild them. Counting panes cannot see the
# difference — a rebuilt terminal even re-attaches to the same tmux session —
# so this drives the REAL region components and compares object identity plus
# the page state only the surviving pane can hold.
qt_add_executable(tst_paneidentity tst_paneidentity.cpp)

target_link_libraries(tst_paneidentity PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::WebEngineQuick
    Qt6::WebChannel
    codeharbor_qmlplugin
    ch_editor
    ch_terminal
    ch_viewers
    ch_remote
    ch_models
    ch_ssh
)

add_test(NAME tst_paneidentity COMMAND tst_paneidentity)
set_tests_properties(tst_paneidentity PROPERTIES
    ENVIRONMENT "${CODEHARBOR_QML_TEST_ENV}"
    TIMEOUT 300
)

# Behavioural gate for the sidebar's drag-and-drop reordering and selection
# affordances (SPEC 4.2). Loads the real qrc SessionsSidebar.qml over a stub
# `app` and a real SessionsModel, and drives genuine mouse/key events.
qt_add_executable(tst_sidebar tst_sidebar.cpp)

target_link_libraries(tst_sidebar PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    codeharbor_qmlplugin
    ch_models
)

add_test(NAME tst_sidebar COMMAND tst_sidebar)
set_tests_properties(tst_sidebar PROPERTIES
    ENVIRONMENT "${CODEHARBOR_QML_TEST_ENV}"
    TIMEOUT 180
)

# Environment probe: proves whether Qt WebEngine can actually load and script a
# page under this box's headless recipe. Labelled `live` because it depends on
# machine capabilities (QtWebEngineProcess, /dev/shm, sandbox permissions), and
# QSKIPs instead of failing when the platform cannot support it.
qt_add_executable(tst_webengine_headless tst_webengine_headless.cpp)

target_link_libraries(tst_webengine_headless PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::WebEngineQuick
)

add_test(NAME tst_webengine_headless COMMAND tst_webengine_headless)
set_tests_properties(tst_webengine_headless PROPERTIES
    LABELS live
    ENVIRONMENT "${CODEHARBOR_QML_TEST_ENV}"
    TIMEOUT 180
)
set_tests_properties(tst_webengine_headless PROPERTIES
    FIXTURES_REQUIRED codeharbor_live_fixture)

# Command palette + global shortcuts (SPEC 15), and the module's colour gate.
#
# Loads the REAL CommandPalette.qml out of the CodeHarbor module's own qrc
# (qrc:/qt/qml/CodeHarbor/CommandPalette.qml), which is why this target links
# codeharbor_qmlplugin: the palette reaches the `Theme` singleton, and a
# singleton only exists where the module is registered. The earlier arrangement
# embedded a copy of the file in this target's own qrc precisely to avoid that
# dependency, but with no module registered `Theme.x` resolved to undefined —
# blank colours, no warning.
#
# CODEHARBOR_QML_SOURCE_DIR points the colour gate at the QML sources it scans.
get_filename_component(_ch_qml_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

qt_add_executable(tst_palette tst_palette.cpp)

target_link_libraries(tst_palette PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    codeharbor_qmlplugin
)

target_compile_definitions(tst_palette PRIVATE
    CODEHARBOR_QML_SOURCE_DIR="${_ch_qml_src_dir}"
)

add_test(NAME tst_palette COMMAND tst_palette)
set_tests_properties(tst_palette PROPERTIES
    ENVIRONMENT "${CODEHARBOR_QML_TEST_ENV}"
    TIMEOUT 120
)

# Cold-start usability gate for the shell surfaces a first-time user lands on,
# and the frame grabber behind the design review (`tst_uxshell -shots` writes
# PNGs of every surface under <build>/src/qml/tests/ux-shots/$CH_UX_PHASE).
# Loads the REAL qrc components: ConnectSheet, SessionsSidebar, CommandPalette,
# TerminalPaneView and ViewerPane, over a stub `app`.
qt_add_executable(tst_uxshell tst_uxshell.cpp)

target_link_libraries(tst_uxshell PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::WebEngineQuick
    Qt6::WebChannel
    codeharbor_qmlplugin
    ch_models
)

# The connection-state tests DERIVE the set of states from the source of truth
# (a scan of src/app/AppController.cpp for setConnectionState literals) instead
# of hand-listing them, so a newly published state cannot ship with no wording
# in the sidebar footer. Same mechanism as src/models/tests.
target_compile_definitions(tst_uxshell PRIVATE
    CH_REPO_ROOT="${CMAKE_SOURCE_DIR}"
)

add_test(NAME tst_uxshell COMMAND tst_uxshell)
set_tests_properties(tst_uxshell PROPERTIES
    ENVIRONMENT "${CODEHARBOR_QML_TEST_ENV}"
    TIMEOUT 180
)


# Focused regression gate for the shared scrollbar and the ViewerPane/WebEngine
# cursor handoff. The latter uses a local data: page, not the unavailable live
# SSH fixture, so it stays in the ordinary headless suite.
qt_add_executable(tst_ui_polish tst_ui_polish.cpp)

target_link_libraries(tst_ui_polish PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::WebEngineQuick
    codeharbor_qmlplugin
)

add_test(NAME tst_ui_polish COMMAND tst_ui_polish)
set_tests_properties(tst_ui_polish PROPERTIES
    ENVIRONMENT "${CODEHARBOR_QML_TEST_ENV}"
    TIMEOUT 180
)

# Log pane regression: a remote diagnostic emitted while the sheet is closed
# remains in the one shared buffer and appears when the sheet opens.
qt_add_executable(tst_logview tst_logview.cpp)
target_link_libraries(tst_logview PRIVATE
    ch_app
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    codeharbor_qmlplugin
)
add_test(NAME tst_logview COMMAND tst_logview)
set_tests_properties(tst_logview PROPERTIES
    ENVIRONMENT "${CODEHARBOR_QML_TEST_ENV}"
    TIMEOUT 120
)

# Open-as explorer regression: the real directory menu only offers handlers
# claimed by the registry, and the existing viewer split path keeps the first
# pane alive while a second pane receives the target.
qt_add_executable(tst_openas tst_openas.cpp)
target_link_libraries(tst_openas PRIVATE
    Qt6::Test
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::WebEngineQuick
    codeharbor_qmlplugin
    ch_viewers
)
add_test(NAME tst_openas COMMAND tst_openas)
set_tests_properties(tst_openas PROPERTIES
    ENVIRONMENT "${CODEHARBOR_QML_TEST_ENV}"
    TIMEOUT 180
)

if(WIN32)
    # Keep QTest diagnostics visible on Windows. qt_add_executable selects the
    # GUI subsystem, and a GUI-subsystem process has no console attached: a
    # failing test exits with code 1 while its assertion text and test-case name
    # go nowhere at all. Every target in this directory is a console test, so
    # every one of them needs this — it used to be set on tst_qmlload alone,
    # which made exactly one of nine failures diagnosable from a release log.
    set_target_properties(
        tst_qmlload
        tst_paneidentity
        tst_sidebar
        tst_webengine_headless
        tst_palette
        tst_uxshell
        tst_ui_polish
        tst_logview
        tst_openas
        PROPERTIES WIN32_EXECUTABLE FALSE)
endif()
