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

# SshConnectionPool's server-free halves: what its destructor is allowed to
# announce, and the known-hosts policy it applies to a key the handshake has
# already read off the wire — including the user declining an unknown one.
qt_add_executable(tst_sshconnectionpool tst_sshconnectionpool.cpp)

target_link_libraries(tst_sshconnectionpool PRIVATE
    Qt6::Core
    Qt6::Test
    ch_ssh
)

add_test(NAME tst_sshconnectionpool COMMAND tst_sshconnectionpool)

# The server-free half of SshChannelDevice: its QIODevice contract with no
# channel behind it, and how a refused start is reported.
qt_add_executable(tst_sshchanneldevice tst_sshchanneldevice.cpp)

target_link_libraries(tst_sshchanneldevice PRIVATE
    Qt6::Core
    Qt6::Test
    ch_ssh
)

add_test(NAME tst_sshchanneldevice COMMAND tst_sshchanneldevice)

# libssh's diagnostic logging is process-global. SshLogRouter owns that state on
# behalf of every pool; this covers the half of it that needs no SSH server:
# install/restore refcounting, out-of-order release, per-thread attribution, and
# the transcript cap.
qt_add_executable(tst_sshlogrouter tst_sshlogrouter.cpp)

target_link_libraries(tst_sshlogrouter PRIVATE
    Qt6::Core
    Qt6::Test
    ch_ssh
)

add_test(NAME tst_sshlogrouter COMMAND tst_sshlogrouter)

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

# Console-mode on Windows, for the same reason as src/app/tests: qt_add_executable
# marks executables as GUI subsystem, which on Windows swallows QTest's stdout and
# stderr, so a failing assertion reports nothing at all through ctest.
if(WIN32)
    set_target_properties(
        tst_knownhosts
        tst_sshauth
        tst_sshconnectionpool
        tst_sshchanneldevice
        tst_sshlogrouter
        tst_livessh
        PROPERTIES WIN32_EXECUTABLE FALSE)
endif()
