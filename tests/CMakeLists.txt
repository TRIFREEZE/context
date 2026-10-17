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

# ---------------------------------------------------------------------------
# Host test: loads the BUILT Context.vst3 through JUCE's VST3 hosting (like a DAW).
# ---------------------------------------------------------------------------
option(CONTEXT_BUILD_HOST_TEST "Build the VST3 host-load test" ON)
if(CONTEXT_BUILD_HOST_TEST)
    juce_add_console_app(ContextHostTest PRODUCT_NAME "ContextHostTest" NEEDS_CURL TRUE)
    target_sources(ContextHostTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/host/HostTest.cpp")
    target_compile_definitions(ContextHostTest PRIVATE
        JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_VST=0 JUCE_PLUGINHOST_AU=0 JUCE_PLUGINHOST_LADSPA=0 JUCE_PLUGINHOST_LV2=0
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(ContextHostTest PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_recommended_config_flags)
    add_dependencies(ContextHostTest Context_VST3)

    set(_ctx_host_args "${CMAKE_BINARY_DIR}/Context_artefacts/$<CONFIG>/VST3/${CONTEXT_PRODUCT_NAME}.vst3")
    if(NOT WIN32 AND NOT APPLE)
        list(APPEND _ctx_host_args --no-editor)   # headless Linux CI has no display for plugin views
    endif()
    add_test(NAME vst3_host_load COMMAND ContextHostTest ${_ctx_host_args})
    set_tests_properties(vst3_host_load PROPERTIES TIMEOUT 300
        ENVIRONMENT "CONTEXT_APPDATA_DIR=${CMAKE_BINARY_DIR}/host-test-appdata")
endif()
