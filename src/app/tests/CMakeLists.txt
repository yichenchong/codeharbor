qt_add_executable(tst_appcontroller tst_appcontroller.cpp)
target_link_libraries(tst_appcontroller PRIVATE
    ch_app
    Qt6::Test
    Qt6::Core
    Qt6::Gui
)
add_test(NAME tst_appcontroller COMMAND tst_appcontroller)
