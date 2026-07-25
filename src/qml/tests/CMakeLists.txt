# Headless environment shared by every test in this directory. Chromium must be
# told to stay off the GPU and out of its sandbox; Quick must use the software
# renderer because there is no display or GL context in CI.
#
# --single-process is deliberately ABSENT: Chromium aborts ("Single mode supports
# only single profile") as soon as a second QWebEngineProfile exists, and the
# viewer stack creates two by design (sandboxed external + privileged internal,
# SPEC 7.2). A WebEngineView's transient default profile can trip it alone, so it
# is unsafe here even though Main.qml's empty panes happen to survive it.
set(CODEHARBOR_QML_TEST_ENV
    "QT_QPA_PLATFORM=offscreen"
    "QT_QUICK_BACKEND=software"
    "QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --no-sandbox --disable-dev-shm-usage"
)

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
