qt_add_library(ch_remote STATIC
    CodeharbordClient.h
    CodeharbordClient.cpp
    RpcTypes.h
)

target_include_directories(ch_remote PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ch_remote PUBLIC Qt6::Core)

if(CODEHARBOR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
