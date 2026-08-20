qt_add_library(ch_editor STATIC
    EditorController.h
    EditorController.cpp
    EditorFactory.h
    EditorFactory.cpp
)

target_include_directories(ch_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ch_editor PUBLIC Qt6::Core ch_remote ch_models)

# CODEHARBOR_BUILD_DESKTOP as well as CODEHARBOR_BUILD_TESTS: this directory's
# test targets link ch_viewers and codeharbor_qmlplugin BY NAME - they drive the
# real Chromium panes - and neither target exists in a mobile configure. Gating
# the whole directory is what makes `-DCODEHARBOR_BUILD_DESKTOP=OFF` a
# configuration that can still be tested, instead of one that fails at generate
# time. A desktop build is unaffected: both variables default ON.
if(CODEHARBOR_BUILD_TESTS AND CODEHARBOR_BUILD_DESKTOP)
    add_subdirectory(tests)
endif()
