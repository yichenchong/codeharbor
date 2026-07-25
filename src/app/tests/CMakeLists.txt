qt_add_executable(tst_appcontroller tst_appcontroller.cpp)
target_link_libraries(tst_appcontroller PRIVATE
    ch_app
    Qt6::Test
    Qt6::Core
    Qt6::Gui
)
add_test(NAME tst_appcontroller COMMAND tst_appcontroller)

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
