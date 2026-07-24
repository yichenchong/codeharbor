qt_add_library(ch_terminal STATIC
    TerminalController.h
    TerminalController.cpp
)

target_include_directories(ch_terminal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ch_terminal PUBLIC Qt6::Core ch_models ch_ssh)
