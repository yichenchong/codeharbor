qt_add_executable(tst_models tst_models.cpp)
target_link_libraries(tst_models PRIVATE Qt6::Test ch_models)
add_test(NAME tst_models COMMAND tst_models)
