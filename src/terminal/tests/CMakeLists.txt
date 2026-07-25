qt_add_executable(tst_terminalcontroller tst_terminalcontroller.cpp)
target_link_libraries(tst_terminalcontroller PRIVATE Qt6::Test ch_terminal)
add_test(NAME tst_terminalcontroller COMMAND tst_terminalcontroller)

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
