qt_add_executable(tst_agentmonitor tst_agentmonitor.cpp)

target_link_libraries(tst_agentmonitor PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
    ch_agent
)

# Repo root so harnessWireWordsMatchRemoteEventsTs can read the authoritative
# HARNESSES list out of remote/src/events.ts and check that the client accepts
# every harness the daemon may name. Same mechanism as src/models/tests.
target_compile_definitions(tst_agentmonitor PRIVATE
    CH_REPO_ROOT="${CMAKE_SOURCE_DIR}"
)

add_test(NAME tst_agentmonitor COMMAND tst_agentmonitor)

# The client half of `tmux.paneActivity` (SPEC 6.6): the poll that keeps a Dev
# Session the user is not looking at reporting its agent status. Qt6::Network
# for the QLocalSocket pair the RPC client is driven over.
qt_add_executable(tst_tmuxactivity tst_tmuxactivity.cpp)

target_link_libraries(tst_tmuxactivity PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
    ch_agent
)

add_test(NAME tst_tmuxactivity COMMAND tst_tmuxactivity)

# LIVE gate (docs/PLAN.md A): the whole agent-awareness chain against a real
# sshd — the real remote hook, the real remote bridge, a real SSH channel — plus
# SPEC 6.6's server-side activity poll for a pane whose PTY is detached.
# Registered unconditionally and labelled `live`; the test QSKIPs when
# CH_LIVE_SSH is unset so the default `ctest` run stays green everywhere.
qt_add_executable(tst_liveagent tst_liveagent.cpp)

target_link_libraries(tst_liveagent PRIVATE
    Qt6::Core
    Qt6::Test
    ch_agent
    ch_models
    ch_ssh
    # ch_remote for the RPC client the tmux.paneActivity poll is driven over, and
    # ch_app for SessionBootstrap::rpcCommand() — the exact `codeharbord rpc
    # --stdio` string the shipped client execs, entry-point ladder included, so
    # the live gate cannot pass against a remote layout the app could not launch.
    ch_remote
    ch_app
)

add_test(NAME tst_liveagent COMMAND tst_liveagent)
set_tests_properties(tst_liveagent PROPERTIES LABELS live)
set_tests_properties(tst_liveagent PROPERTIES
    FIXTURES_REQUIRED codeharbor_live_fixture)

# Console-mode on Windows, for the same reason as src/app/tests: qt_add_executable
# marks executables as GUI subsystem, which on Windows swallows QTest's stdout and
# stderr, so a failing assertion reports nothing at all through ctest.
if(WIN32)
    set_target_properties(
        tst_agentmonitor
        tst_tmuxactivity
        tst_liveagent
        PROPERTIES WIN32_EXECUTABLE FALSE)
endif()
