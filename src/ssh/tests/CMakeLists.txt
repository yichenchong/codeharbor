qt_add_executable(tst_knownhosts tst_knownhosts.cpp)

target_link_libraries(tst_knownhosts PRIVATE
    Qt6::Core
    Qt6::Test
    ch_ssh
)

add_test(NAME tst_knownhosts COMMAND tst_knownhosts)
