qt_add_executable(tst_agentmonitor tst_agentmonitor.cpp)

target_link_libraries(tst_agentmonitor PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
    ch_agent
)

add_test(NAME tst_agentmonitor COMMAND tst_agentmonitor)
