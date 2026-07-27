qt_add_library(ch_editor STATIC
    EditorController.h
    EditorController.cpp
    EditorFactory.h
    EditorFactory.cpp
)

target_include_directories(ch_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ch_editor PUBLIC Qt6::Core ch_remote ch_models)

if(CODEHARBOR_BUILD_TESTS)
    add_subdirectory(tests)
endif()
