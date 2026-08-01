qt_add_executable(tst_models tst_models.cpp)
target_link_libraries(tst_models PRIVATE Qt6::Test ch_models)

# Repo root so agentStateWireWordsMatchRemoteEventsTs can read the authoritative
# AGENT_STATES list out of remote/src/events.ts and compare it against the
# ch::AgentState enumeration. Same mechanism as src/persistence/tests.
target_compile_definitions(tst_models PRIVATE
    CH_REPO_ROOT="${CMAKE_SOURCE_DIR}"
)

add_test(NAME tst_models COMMAND tst_models)

# Console-mode on Windows, for the same reason as src/app/tests: qt_add_executable
# marks executables as GUI subsystem, which on Windows swallows QTest's stdout and
# stderr, so a failing assertion reports nothing at all through ctest.
if(WIN32)
    set_target_properties(tst_models PROPERTIES WIN32_EXECUTABLE FALSE)
endif()
