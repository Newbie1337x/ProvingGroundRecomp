cmake_minimum_required(VERSION 3.25)
project(ProvingGroundRecomp VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(WIN32)
    enable_language(RC)
endif()

option(PG_BUILD_GAME "Build the game from the generated code (off: only the codegen tool)" ON)

# The game needs ReXGlue v0.10.0 from source with patches/rexglue-sdk.patch applied; the build
# scripts fetch and patch it into thirdparty/rexglue-sdk.
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk" CACHE PATH
    "Path to the patched rexglue-sdk source tree")
if(NOT EXISTS "${REXSDK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "ReXGlue SDK source not found at ${REXSDK_DIR}. Run build.ps1 (Windows) or build.sh "
        "(Linux), which fetch and patch it, or set REXSDK_DIR to a patched checkout.")
endif()
add_subdirectory("${REXSDK_DIR}" rexglue-sdk)
file(STRINGS "${CMAKE_CURRENT_BINARY_DIR}/rexglue-sdk/include/rex/version.h"
    _sdk_version_define REGEX "^#define REXGLUE_VERSION_STRING ")
string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" REXGLUE_VERSION_STRING "${_sdk_version_define}")
if(NOT REXGLUE_VERSION_STRING VERSION_EQUAL "0.10.0")
    message(FATAL_ERROR "This project targets ReXGlue 0.10.0; found ${REXGLUE_VERSION_STRING}")
endif()

add_custom_target(pg_codegen
    COMMAND $<TARGET_FILE:rex::rexglue> codegen "${CMAKE_CURRENT_SOURCE_DIR}/pg_config.toml"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generating Proving Ground code from your default.xex"
    VERBATIM
)

if(NOT PG_BUILD_GAME)
    message(STATUS "Game disabled: build the rexglue target, then run codegen")
    return()
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xex")
    message(FATAL_ERROR
        "Missing assets/default.xex. Extract your Xbox 360 copy of the game into assets first.")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/sources.cmake" OR
   NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/pg_init.h")
    message(FATAL_ERROR
        "Generated code is missing. Run 'rexglue codegen pg_config.toml', then configure again "
        "(the build scripts do this automatically).")
endif()

include(generated/sources.cmake)
add_library(pg_recomp OBJECT ${GENERATED_SOURCES})
target_include_directories(pg_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/generated")
target_link_libraries(pg_recomp PRIVATE rex::runtime)
rexglue_apply_target_settings(pg_recomp)
target_precompile_headers(pg_recomp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/generated/pg_pch.h")
target_compile_options(pg_recomp PRIVATE
    $<$<AND:$<CONFIG:RelWithDebInfo>,$<CXX_COMPILER_ID:Clang,AppleClang>>:-gline-tables-only>)
# Guest SEH handlers need matching asynchronous EH settings in the PCH and TUs.
if(WIN32 AND MSVC)
    target_compile_options(pg_recomp PRIVATE /EHa)
elseif(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(pg_recomp PRIVATE -fasync-exceptions)
endif()

set(PG_SOURCES
    src/main.cpp
    src/pg_buttons.cpp
    src/pg_dev.cpp
    src/pg_folder_picker.cpp
    src/pg_frame_pacing.cpp
    src/pg_input_script.cpp
    src/pg_sampler.cpp
    src/pg_timeline_hooks.cpp
    src/pg_ui.cpp
)
if(WIN32)
    add_executable(pg WIN32 ${PG_SOURCES} res/pg.rc)
else()
    add_executable(pg ${PG_SOURCES})
endif()
set_target_properties(pg PROPERTIES OUTPUT_NAME ProvingGround)
# The SDK's runtime does not export the ImGui include directory to host applications.
target_include_directories(pg PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${REXSDK_DIR}/thirdparty/imgui")
target_link_libraries(pg PRIVATE pg_recomp rex::runtime SDL3::SDL3)
if(WIN32)
    target_link_libraries(pg PRIVATE dbghelp winmm psapi ole32 uuid shell32)
endif()
# Xenos is loaded dynamically; linking rex::runtime alone does not stage it.
rexglue_configure_target(pg GPU_PLUGINS xenos)
