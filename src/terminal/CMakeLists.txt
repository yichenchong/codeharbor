qt_add_library(ch_terminal STATIC
    TerminalController.h
    TerminalController.cpp
    TerminalBridge.h
    TerminalBridge.cpp
    TerminalFactory.h
    TerminalFactory.cpp
)

target_include_directories(ch_terminal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# ch_persistence: TerminalFactory resolves a pane's tmux target through the
# workspace repository — the SERVER owns a terminal's identity (SPEC 5.2).
target_link_libraries(ch_terminal PUBLIC Qt6::Core ch_models ch_persistence ch_ssh)

if(CODEHARBOR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
