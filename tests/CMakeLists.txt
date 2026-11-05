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
        NAME tst_live_fixture_generation
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/live_fixture_generation.sh
                ${PROJECT_SOURCE_DIR})
    set_tests_properties(tst_live_fixture_generation PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 60)
    add_test(
        NAME tst_live_fixture_socket_isolation
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/live_fixture_socket_isolation.sh
                ${PROJECT_SOURCE_DIR})
    set_tests_properties(tst_live_fixture_socket_isolation PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 60)
    add_test(
        NAME tst_vcpkg_patch_validation
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_patch_validation.sh
                ${PROJECT_SOURCE_DIR})
    set_tests_properties(tst_vcpkg_patch_validation PROPERTIES TIMEOUT 30)
    if(CODEHARBOR_BUILD_DESKTOP)
        add_test(
            NAME tst_web_bundle_rebuild
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/web_bundle_rebuild.sh
                    ${PROJECT_SOURCE_DIR} "${Qt6_DIR}")
        set_tests_properties(tst_web_bundle_rebuild PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 120)
    endif()
    if(CODEHARBOR_BUILD_MOBILE)
        add_test(
            NAME tst_android_navigation_policy
            COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/android_navigation_policy.sh
                    ${PROJECT_SOURCE_DIR})
        set_tests_properties(tst_android_navigation_policy PROPERTIES
            SKIP_RETURN_CODE 77
            TIMEOUT 60)
    endif()
endif()
