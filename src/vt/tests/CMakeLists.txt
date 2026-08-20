# Behavioural gate for the native VT engine: the parser state machine, the screen
# model, and key/paste encoding. Headless by construction - QTEST_GUILESS_MAIN,
# no QGuiApplication, no SSH, no device - so it runs identically on every CI
# platform and inside the mobile-only build.
qt_add_executable(tst_vt tst_vt.cpp)
target_link_libraries(tst_vt PRIVATE Qt6::Test ch_vt)
add_test(NAME tst_vt COMMAND tst_vt)

# Console-mode on Windows, for the same reason as src/models/tests:
# qt_add_executable marks executables as GUI subsystem, which on Windows swallows
# QTest's stdout and stderr, so a failing assertion reports nothing at all
# through ctest.
if(WIN32)
    set_target_properties(tst_vt PROPERTIES WIN32_EXECUTABLE FALSE)
endif()
