# The live fixture setup test is a CTest fixture, not a live-labelled target:
# CTest pulls it in automatically for every test that requires this fixture.
# The script exits quietly when CH_LIVE_SSH is unset, so the portable suite does
# not need an SSH server.
if(UNIX)
    add_test(
        NAME tst_live_fixture_isolation
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/live/check-fixture-isolation.sh)
    set_tests_properties(tst_live_fixture_isolation PROPERTIES
        FIXTURES_SETUP codeharbor_live_fixture
        TIMEOUT 30)
    add_test(
        NAME tst_live_fixture_isolation_paths
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/live_fixture_isolation.sh
                ${PROJECT_SOURCE_DIR})
    set_tests_properties(tst_live_fixture_isolation_paths PROPERTIES TIMEOUT 30)
    add_test(
        NAME tst_vcpkg_patch_validation
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_patch_validation.sh
                ${PROJECT_SOURCE_DIR})
    set_tests_properties(tst_vcpkg_patch_validation PROPERTIES TIMEOUT 30)
endif()
