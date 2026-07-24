qt_add_executable(tst_viewers tst_viewers.cpp)

target_link_libraries(tst_viewers PRIVATE
    ch_viewers
    Qt6::Test
)

add_test(NAME tst_viewers COMMAND tst_viewers)
