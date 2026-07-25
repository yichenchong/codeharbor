qt_add_executable(tst_editorcontroller tst_editorcontroller.cpp)

target_link_libraries(tst_editorcontroller PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
    ch_editor
)

add_test(NAME tst_editorcontroller COMMAND tst_editorcontroller)
