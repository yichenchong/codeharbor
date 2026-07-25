qt_add_executable(tst_agentmonitor tst_agentmonitor.cpp)

target_link_libraries(tst_agentmonitor PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
    ch_agent
)

add_test(NAME tst_agentmonitor COMMAND tst_agentmonitor)

# LIVE gate (docs/PLAN.md A): the whole agent-awareness chain against a real
# sshd — the real remote hook, the real remote bridge, a real SSH channel.
# Registered unconditionally and labelled `live`; the test QSKIPs when
# CH_LIVE_SSH is unset so the default `ctest` run stays green everywhere.
qt_add_executable(tst_liveagent tst_liveagent.cpp)

target_link_libraries(tst_liveagent PRIVATE
    Qt6::Core
    Qt6::Test
    ch_agent
    ch_models
    ch_ssh
)

add_test(NAME tst_liveagent COMMAND tst_liveagent)
set_tests_properties(tst_liveagent PROPERTIES LABELS live)
