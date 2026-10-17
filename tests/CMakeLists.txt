juce_add_console_app(ContextTests PRODUCT_NAME "ContextTests" NEEDS_CURL TRUE)

file(GLOB CONTEXT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp")
target_sources(ContextTests PRIVATE ${CONTEXT_TEST_SOURCES})
# The test runner pumps the message loop while waiting for async work.
target_compile_definitions(ContextTests PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(ContextTests PRIVATE
    context_shared
    juce::juce_recommended_config_flags)

add_test(NAME unit_tests COMMAND ContextTests --unit)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME bridge_integration
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/integration/run_bridge_integration.py"
                     --tests-exe "$<TARGET_FILE:ContextTests>"
                     --bridge "${CMAKE_SOURCE_DIR}/tools/reference_bridge/reference_bridge.py")
    set_tests_properties(bridge_integration PROPERTIES TIMEOUT 600)

    add_test(NAME license_integration
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/integration/run_license_integration.py"
                     --tests-exe "$<TARGET_FILE:ContextTests>"
                     --backend "${CMAKE_SOURCE_DIR}/backend")
    # Exit code 77 = backend Python packages not installed -> reported as skipped, not failed.
    set_tests_properties(license_integration PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
else()
    message(STATUS "Python 3 not found: bridge integration test disabled")
endif()
