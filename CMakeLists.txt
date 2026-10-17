cmake_minimum_required(VERSION 3.22)

# ---------------------------------------------------------------------------
# Product identity — change these to rename the product globally.
# (Renaming changes the plugin ID, so DAWs treat it as a new plugin.)
# ---------------------------------------------------------------------------
set(CONTEXT_PRODUCT_NAME   "Context"      CACHE STRING "Product / plugin name")
set(CONTEXT_COMPANY_NAME   "TRIFREEZE"    CACHE STRING "Company name shown in hosts")
set(CONTEXT_BUNDLE_ID      "com.trifreeze.context" CACHE STRING "Bundle identifier")
set(CONTEXT_MANUFACTURER_CODE "Trfz"      CACHE STRING "4-char manufacturer code (one uppercase letter min)")
set(CONTEXT_PLUGIN_CODE    "Cntx"         CACHE STRING "4-char unique plugin code")

# Licensing server URL compiled in as the default (users can change it in Settings).
set(CONTEXT_LICENSE_SERVER_URL "http://127.0.0.1:8787" CACHE STRING "Default licensing backend URL")

option(CONTEXT_BUILD_TESTS     "Build the unit test executable" ON)
option(CONTEXT_BUILD_STANDALONE "Build the Standalone app (debugging aid)" ON)
option(CONTEXT_DEVELOPER_BUILD "Developer build: mock provider + verbose logging by default" OFF)

# The MSVC runtime is linked statically so users never need the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(Context VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ---------------------------------------------------------------------------
# JUCE — pinned to an exact commit, recorded in dependencies/JUCE_COMMIT
# (tag in dependencies/JUCE_VERSION). Never "latest".
# CI and scripts/fetch-juce.* do a fast shallow fetch of that commit and pass
#   -DFETCHCONTENT_SOURCE_DIR_JUCE=<folder>
# Without that option CMake clones JUCE itself (slower, same commit).
# ---------------------------------------------------------------------------
file(STRINGS "${CMAKE_SOURCE_DIR}/dependencies/JUCE_COMMIT" CONTEXT_JUCE_COMMIT LIMIT_COUNT 1)
file(STRINGS "${CMAKE_SOURCE_DIR}/dependencies/JUCE_VERSION" CONTEXT_JUCE_VERSION LIMIT_COUNT 1)
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${CONTEXT_JUCE_COMMIT}
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(JUCE)

# Guard against building with the wrong JUCE (e.g. a stale local folder).
file(STRINGS "${juce_SOURCE_DIR}/CMakeLists.txt" _ctx_juce_project_line REGEX "^project\\(JUCE VERSION")
if(NOT _ctx_juce_project_line MATCHES "VERSION ${CONTEXT_JUCE_VERSION}")
    message(FATAL_ERROR "Expected JUCE ${CONTEXT_JUCE_VERSION} but found: ${_ctx_juce_project_line} in ${juce_SOURCE_DIR}")
endif()

# ---------------------------------------------------------------------------
# Build metadata (version, git commit) for the About screen.
# ---------------------------------------------------------------------------
set(CONTEXT_GIT_COMMIT "unknown")
if(DEFINED ENV{GITHUB_SHA})
    string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 CONTEXT_GIT_COMMIT)
else()
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
                        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                        OUTPUT_VARIABLE _ctx_commit OUTPUT_STRIP_TRAILING_WHITESPACE
                        RESULT_VARIABLE _ctx_git_result ERROR_QUIET)
        if(_ctx_git_result EQUAL 0 AND _ctx_commit)
            set(CONTEXT_GIT_COMMIT "${_ctx_commit}")
        endif()
    endif()
endif()

# Public key for license-token verification (public information, safe to commit).
set(CONTEXT_PUBLIC_KEY_FILE "${CMAKE_SOURCE_DIR}/licensing/license_public_key.txt")
set(CONTEXT_LICENSE_PUBLIC_KEY "")
if(EXISTS "${CONTEXT_PUBLIC_KEY_FILE}")
    file(STRINGS "${CONTEXT_PUBLIC_KEY_FILE}" _ctx_key_lines REGEX "^[A-Za-z0-9+/=]+$")
    list(LENGTH _ctx_key_lines _ctx_key_count)
    if(_ctx_key_count GREATER 0)
        list(GET _ctx_key_lines 0 CONTEXT_LICENSE_PUBLIC_KEY)
    endif()
endif()
if(CONTEXT_LICENSE_PUBLIC_KEY STREQUAL "")
    message(WARNING "No license public key in ${CONTEXT_PUBLIC_KEY_FILE}: license activation will be unavailable in this build.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CONTEXT_PUBLIC_KEY_FILE}")

configure_file(plugin/Source/Core/BuildInfo.h.in "${CMAKE_BINARY_DIR}/generated/BuildInfo.h" @ONLY)

# ---------------------------------------------------------------------------
# Vendored Ed25519 verification (Monocypher, BSD-2 / CC0).
# ---------------------------------------------------------------------------
add_library(context_monocypher STATIC
    third_party/monocypher/monocypher.c
    third_party/monocypher/monocypher-ed25519.c)
target_include_directories(context_monocypher PUBLIC third_party/monocypher)
set_target_properties(context_monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "third_party")

# ---------------------------------------------------------------------------
# Shared source set: compiled into the plugin AND the tests.
# ---------------------------------------------------------------------------
include(plugin/Sources.cmake)

add_library(context_shared INTERFACE)
target_sources(context_shared INTERFACE ${CONTEXT_SHARED_SOURCES})
target_include_directories(context_shared INTERFACE
    "${CMAKE_SOURCE_DIR}/plugin/Source"
    "${CMAKE_BINARY_DIR}/generated")
target_compile_definitions(context_shared INTERFACE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    CONTEXT_DEVELOPER_BUILD=$<BOOL:${CONTEXT_DEVELOPER_BUILD}>
    CONTEXT_BUILD_CONFIG="$<CONFIG>")
target_link_libraries(context_shared INTERFACE
    context_monocypher
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_cryptography)
if(MSVC)
    # Source files are UTF-8; keep warnings visible but never fail CI on JUCE warnings.
    target_compile_options(context_shared INTERFACE /utf-8 /bigobj /W3)
else()
    target_compile_options(context_shared INTERFACE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# The plugin.
# ---------------------------------------------------------------------------
set(_ctx_formats VST3)
if(CONTEXT_BUILD_STANDALONE)
    list(APPEND _ctx_formats Standalone)
endif()

juce_add_plugin(Context
    PRODUCT_NAME              "${CONTEXT_PRODUCT_NAME}"
    COMPANY_NAME              "${CONTEXT_COMPANY_NAME}"
    BUNDLE_ID                 "${CONTEXT_BUNDLE_ID}"
    PLUGIN_MANUFACTURER_CODE  "${CONTEXT_MANUFACTURER_CODE}"
    PLUGIN_CODE               "${CONTEXT_PLUGIN_CODE}"
    VERSION                   "${PROJECT_VERSION}"
    FORMATS                   ${_ctx_formats}
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          FALSE
    NEEDS_MIDI_OUTPUT         FALSE
    IS_MIDI_EFFECT            FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    NEEDS_CURL                TRUE
    COPY_PLUGIN_AFTER_BUILD   FALSE
    VST3_CATEGORIES           Fx Analyzer
    DESCRIPTION               "AI sound generation inside your DAW")

# Only the plugin entry point lives outside the shared source set, so the test
# executable can instantiate the real processor and editor.
target_sources(Context PRIVATE plugin/Source/PluginEntry.cpp)
target_link_libraries(Context PRIVATE
    context_shared
    juce::juce_recommended_config_flags)
# Plugins must never run modal loops inside a host.
target_compile_definitions(Context PUBLIC JUCE_MODAL_LOOPS_PERMITTED=0)

# ---------------------------------------------------------------------------
# Tests.
# ---------------------------------------------------------------------------
if(CONTEXT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
