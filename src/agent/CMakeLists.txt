qt_add_library(ch_agent STATIC
    AgentEvent.h
    AgentStatusMonitor.h
    AgentStatusMonitor.cpp
    TmuxActivityPoller.h
    TmuxActivityPoller.cpp
)

target_include_directories(ch_agent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# ch_remote for the tmux.paneActivity poller's RPC peer. PUBLIC because
# TmuxActivityPoller.h names ch::CodeharbordClient in its interface, so anything
# that constructs one needs the header too.
target_link_libraries(ch_agent PUBLIC Qt6::Core ch_models ch_remote)

if(CODEHARBOR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
