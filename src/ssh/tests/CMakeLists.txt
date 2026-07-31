qt_add_executable(tst_knownhosts tst_knownhosts.cpp)

target_link_libraries(tst_knownhosts PRIVATE
    Qt6::Core
    Qt6::Test
    ch_ssh
)

add_test(NAME tst_knownhosts COMMAND tst_knownhosts)

# Multi-step authentication decisions (SPEC 12.1), driven as pure functions so
# every order a server can require its methods in is covered without a network.
qt_add_executable(tst_sshauth tst_sshauth.cpp)

target_link_libraries(tst_sshauth PRIVATE
    Qt6::Core
    Qt6::Test
    ch_ssh
)

add_test(NAME tst_sshauth COMMAND tst_sshauth)

# The server-free half of SshChannelDevice: its QIODevice contract with no
# channel behind it, and how a refused start is reported.
qt_add_executable(tst_sshchanneldevice tst_sshchanneldevice.cpp)

target_link_libraries(tst_sshchanneldevice PRIVATE
    Qt6::Core
    Qt6::Test
    ch_ssh
)

add_test(NAME tst_sshchanneldevice COMMAND tst_sshchanneldevice)

# LIVE gate (docs/PLAN.md S): exercises the real SSH spine against a real sshd.
# Registered unconditionally and labelled `live`; the test QSKIPs when
# CH_LIVE_SSH is unset so the default `ctest` run stays green everywhere.
qt_add_executable(tst_livessh tst_livessh.cpp)

target_link_libraries(tst_livessh PRIVATE
    Qt6::Core
    Qt6::Test
    ch_ssh
    ch_remote
)

add_test(NAME tst_livessh COMMAND tst_livessh)
set_tests_properties(tst_livessh PROPERTIES LABELS live)
