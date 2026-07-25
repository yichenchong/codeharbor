qt_add_library(ch_agent STATIC
    AgentEvent.h
    AgentStatusMonitor.h
    AgentStatusMonitor.cpp
)

target_include_directories(ch_agent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ch_agent PUBLIC Qt6::Core ch_models)

if(CODEHARBOR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
