# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)
# Ask MSVC for English diagnostics when that language pack is installed.
set(ENV{VSLANG} 1033)
project(overglaze VERSION 0.2.0 LANGUAGES C CXX)
enable_language(ASM_MASM)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Overglaze requires 64-bit Windows")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Tracks. Every native source lives in exactly one track directory:
# native/{core,runtime,provider,viewer,controller} (and, in a tree that has it,
# native/research). Ownership, allowed dependencies and the controller link ban
# are declared in native/OWNERSHIP.json and enforced by
# tests/controller/test_track_boundaries.py. LAB_TRACK=controller never
# configures native/research, and is the only choice in a tree without it.
# ---------------------------------------------------------------------------
set(LAB_NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/native")
if(EXISTS "${LAB_NATIVE}/research/CMakeLists.txt")
  set(LAB_TRACK_DEFAULT "all")
else()
  set(LAB_TRACK_DEFAULT "controller")
endif()
set(LAB_TRACK "${LAB_TRACK_DEFAULT}" CACHE STRING "controller: core+runtime+provider+viewer+controller only; research or all: also native/research")
set_property(CACHE LAB_TRACK PROPERTY STRINGS controller research all)
if(NOT LAB_TRACK MATCHES "^(controller|research|all)$")
  message(FATAL_ERROR "LAB_TRACK must be controller, research or all (got '${LAB_TRACK}')")
endif()
if(NOT LAB_TRACK STREQUAL "controller" AND NOT EXISTS "${LAB_NATIVE}/research/CMakeLists.txt")
  message(FATAL_ERROR "LAB_TRACK=${LAB_TRACK} needs native/research, which this tree does not have; use LAB_TRACK=controller")
endif()
option(LAB_BUILD_NR_PROBE "Build the NR bridge and NR fixtures using local NVIDIA headers" ON)
option(LAB_BUILD_NR_CONTROL_RESEARCH "Build opt-in offline NR controls research executables (not a game plugin)" OFF)
if(LAB_TRACK STREQUAL "controller" AND NOT LAB_BUILD_NR_PROBE)
  message(FATAL_ERROR "The controller track needs the NR bridge: configure with LAB_BUILD_NR_PROBE=ON")
endif()

# ---------------------------------------------------------------------------
# Dependencies and tools. Nothing here names a machine: LAB_DEPS defaults to
# %OVERGLAZE_DEPS% (Enter-Environment.ps1 sets it for a toolchain root that has
# one), else to <checkout>/deps, where the Initialize-*.ps1 scripts put them.
# ---------------------------------------------------------------------------
if(DEFINED ENV{OVERGLAZE_DEPS} AND NOT "$ENV{OVERGLAZE_DEPS}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{OVERGLAZE_DEPS}" LAB_DEPS_DEFAULT)
else()
  get_filename_component(LAB_DEPS_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../deps" ABSOLUTE)
endif()
set(LAB_DEPS "${LAB_DEPS_DEFAULT}" CACHE PATH "Pinned dependencies; run the Initialize-*.ps1 scripts")
set(IMGUI_DIR "${LAB_DEPS}/imgui-1.92.5-docking")
foreach(header "${IMGUI_DIR}/imgui.h" "${LAB_DEPS}/json/single_include/nlohmann/json.hpp" "${LAB_DEPS}/minhook/include/MinHook.h")
  if(NOT EXISTS "${header}")
    message(FATAL_ERROR "Missing dependency: ${header} (run Initialize-LabDependencies.ps1, or set LAB_DEPS)")
  endif()
endforeach()
# Python runs the track-boundary test and, optionally, the compiler output
# adapter. %OVERGLAZE_PYTHON% or the first Python 3 on PATH.
if(DEFINED ENV{OVERGLAZE_PYTHON} AND NOT "$ENV{OVERGLAZE_PYTHON}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{OVERGLAZE_PYTHON}" LAB_BUILD_PYTHON_DEFAULT)
else()
  find_program(LAB_PYTHON_FOUND NAMES python3 python)
  set(LAB_BUILD_PYTHON_DEFAULT "${LAB_PYTHON_FOUND}")
endif()
set(LAB_BUILD_PYTHON "${LAB_BUILD_PYTHON_DEFAULT}" CACHE FILEPATH "Python 3 used by the track-boundary test and the compiler output adapter")
if(LAB_BUILD_PYTHON AND NOT EXISTS "${LAB_BUILD_PYTHON}")
  message(FATAL_ERROR "LAB_BUILD_PYTHON does not exist: ${LAB_BUILD_PYTHON}")
endif()
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
  # A localized MSVC (e.g. the Chinese language pack only) prints the
  # /showIncludes prefix and paths in the system code page, which Ninja then
  # misreads as zero header dependencies. The adapter rewrites that output to
  # UTF-8 with the English prefix and leaves English output unchanged. Default
  # ON when Python is available; OFF builds without it.
  if(LAB_BUILD_PYTHON)
    set(LAB_MSVC_OUTPUT_ADAPTER_DEFAULT ON)
  else()
    set(LAB_MSVC_OUTPUT_ADAPTER_DEFAULT OFF)
  endif()
  option(LAB_MSVC_OUTPUT_ADAPTER "Route MSVC output through python/msvc_launcher.py (needed for a non-English MSVC)" ${LAB_MSVC_OUTPUT_ADAPTER_DEFAULT})
  if(LAB_MSVC_OUTPUT_ADAPTER)
    if(NOT LAB_BUILD_PYTHON)
      message(FATAL_ERROR "LAB_MSVC_OUTPUT_ADAPTER needs Python: set LAB_BUILD_PYTHON or turn the adapter off")
    endif()
    set(CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file:")
    set(CMAKE_C_COMPILER_LAUNCHER "${LAB_BUILD_PYTHON};-B;${CMAKE_CURRENT_SOURCE_DIR}/python/msvc_launcher.py")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_C_COMPILER_LAUNCHER}")
  endif()
  add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/W4> $<$<COMPILE_LANGUAGE:C,CXX>:/permissive-> $<$<COMPILE_LANGUAGE:C,CXX>:/EHsc> $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>)
endif()

# Keep the flat build layout the scripts, fixtures and publish maps rely on
# (<build>/<target>.exe, standalone/dxgi.dll, controller/...): targets defined in
# the track subdirectories still land in the build root unless they say otherwise.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# The out-of-game programs find their program root from where they run:
# <root>\app\ (a release) or <root>\data\_build*\ (a build directory inside a
# program root). A build directory anywhere else gets an overglaze-root.json
# beside its executables that names a development root inside the build tree,
# so the programs, and the tests that drive them, work from any checkout and
# write nothing outside the build directory. Delete the file to fall back to the
# layout rules.
get_filename_component(LAB_BUILD_NAME "${CMAKE_BINARY_DIR}" NAME)
get_filename_component(LAB_BUILD_PARENT "${CMAKE_BINARY_DIR}" DIRECTORY)
get_filename_component(LAB_BUILD_PARENT_NAME "${LAB_BUILD_PARENT}" NAME)
string(TOLOWER "${LAB_BUILD_NAME}" LAB_BUILD_NAME_LOWER)
string(TOLOWER "${LAB_BUILD_PARENT_NAME}" LAB_BUILD_PARENT_NAME_LOWER)
if(LAB_BUILD_NAME_LOWER MATCHES "^_build" AND LAB_BUILD_PARENT_NAME_LOWER STREQUAL "data")
  set(LAB_PROGRAM_ROOT "${LAB_BUILD_PARENT}/..")
  get_filename_component(LAB_PROGRAM_ROOT "${LAB_PROGRAM_ROOT}" ABSOLUTE)
else()
  set(LAB_PROGRAM_ROOT "${CMAKE_BINARY_DIR}/dev-root")
  file(MAKE_DIRECTORY "${LAB_PROGRAM_ROOT}/data")
  file(TO_NATIVE_PATH "${LAB_PROGRAM_ROOT}" LAB_PROGRAM_ROOT_NATIVE)
  string(REPLACE "\\" "\\\\" LAB_PROGRAM_ROOT_JSON "${LAB_PROGRAM_ROOT_NATIVE}")
  file(WRITE "${CMAKE_BINARY_DIR}/overglaze-root.json"
    "{\"schema\":\"overglaze-root-override-v1\",\"lab_root\":\"${LAB_PROGRAM_ROOT_JSON}\",\"note\":\"Written by CMake for a build directory outside <root>\\\\data\\\\_build*; delete to use the layout rules.\"}\n")
endif()

add_subdirectory("${LAB_DEPS}/minhook" "${CMAKE_BINARY_DIR}/minhook" EXCLUDE_FROM_ALL)

set(LAB_STREAMLINE_HEADERS "${LAB_DEPS}/streamline-headers-fbe73ba0dc817da8db877c62038d415816209139" CACHE PATH "Pinned public Streamline reference headers; no DLLs")
set(LAB_SL_HEADER_PINS
  "sl_core_api.h|7a2a36ae27c09991b0bccdb28b1a34e24aa688fbead21dfaaa41e97513abf83a"
  "sl_core_types.h|29d0184a91dc24aebc0f1b09217de7b12b6d8186fd8c8c2d021e6e38e0aadf9d"
  "sl_struct.h|fa28dacbb8bc0574d6dd2403ec8b01b2d811facd7884251f3834b6e9dbf4d73f"
  "sl_consts.h|69a9f35aeecd7fbf68a997872ba2e29f27af7da4675f76b94628393d14474abf"
  "sl_version.h|dc9bf43ae1003d4d94a18e901043724a25dce73ff64ebc5b1d015dd84ef0e4a2"
  "sl_result.h|cfd0a3bad8886a10e311d3f93a52b18e55b721b020a059b00fb0584a4e843c11"
  "sl_appidentity.h|388d5c18c213e85e87dcc057fb8223b02dee9572675ed55337a4afe9b8d93531"
  "sl_device_wrappers.h|55f71095da4da57659752506d120dfb9ccaa360e313cbce6086a6ac4c1475967")
foreach(pin IN LISTS LAB_SL_HEADER_PINS)
  string(REPLACE "|" ";" parts "${pin}")
  list(GET parts 0 header)
  list(GET parts 1 expected)
  file(SHA256 "${LAB_STREAMLINE_HEADERS}/${header}" actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "Streamline public header changed: ${header}; review before compiling (Initialize-StreamlineHeaders.ps1 fetches the pinned commit)")
  endif()
endforeach()
add_library(lab_streamline_headers INTERFACE)
target_include_directories(lab_streamline_headers SYSTEM INTERFACE "${LAB_STREAMLINE_HEADERS}")

if(LAB_BUILD_NR_PROBE)
  if(NOT EXISTS "${LAB_DEPS}/ngx-headers/include/nvsdk_ngx.h")
    message(FATAL_ERROR "Missing the NVIDIA NGX SDK headers: run Initialize-NgxHeaders.ps1 (it asks you to accept NVIDIA's licence), or configure LAB_BUILD_NR_PROBE=OFF")
  endif()
  add_library(lab_ngx_headers INTERFACE)
  target_include_directories(lab_ngx_headers SYSTEM INTERFACE "${LAB_DEPS}/ngx-headers/include")
endif()

add_library(lab_imgui STATIC "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_draw.cpp"
  "${IMGUI_DIR}/imgui_tables.cpp" "${IMGUI_DIR}/imgui_widgets.cpp"
  "${IMGUI_DIR}/backends/imgui_impl_win32.cpp" "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp")
target_include_directories(lab_imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_link_libraries(lab_imgui PUBLIC d3d11 dxgi dwmapi)
target_sources(lab_imgui PRIVATE "${IMGUI_DIR}/backends/imgui_impl_dx12.cpp")

# ---------------------------------------------------------------------------
# Research-only dependencies: zlib, the reviewed ReShade headers and the NVAPI
# headers gate configure only when native/research is configured. The
# controller track uses none of them.
# ---------------------------------------------------------------------------
if(NOT LAB_TRACK STREQUAL "controller")
  if(NOT EXISTS "${LAB_DEPS}/reshade/include/reshade.hpp")
    message(FATAL_ERROR "Missing dependency: ${LAB_DEPS}/reshade/include/reshade.hpp")
  endif()
  if(NOT LAB_BUILD_PYTHON)
    message(FATAL_ERROR "The research track needs Python (LAB_BUILD_PYTHON)")
  endif()
  file(SHA256 "${LAB_DEPS}/zlib-1.3.2.tar.gz" LAB_ZLIB_ARCHIVE_SHA)
  if(NOT LAB_ZLIB_ARCHIVE_SHA STREQUAL "bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16")
    message(FATAL_ERROR "Run Initialize-Zlib.ps1; official zlib archive identity mismatch")
  endif()
  execute_process(COMMAND "${LAB_BUILD_PYTHON}" -B "${CMAKE_CURRENT_SOURCE_DIR}/python/verify_zlib.py"
    "${LAB_DEPS}/zlib-1.3.2.tar.gz" "${LAB_DEPS}/zlib-1.3.2"
    RESULT_VARIABLE LAB_ZLIB_VERIFY_RESULT TIMEOUT 20)
  if(NOT LAB_ZLIB_VERIFY_RESULT EQUAL 0)
    message(FATAL_ERROR "Extracted zlib source differs from pinned archive")
  endif()
  file(GLOB_RECURSE LAB_ZLIB_SOURCE_FILES CONFIGURE_DEPENDS "${LAB_DEPS}/zlib-1.3.2/*")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LAB_ZLIB_SOURCE_FILES}
    "${LAB_DEPS}/zlib-1.3.2.tar.gz" "${CMAKE_CURRENT_SOURCE_DIR}/python/verify_zlib.py")
  set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory("${LAB_DEPS}/zlib-1.3.2" "${CMAKE_BINARY_DIR}/zlib" EXCLUDE_FROM_ALL)

  # Reviewed ReShade v6.8.0 headers (public unwrap interface; research add-ons and fixtures).
  foreach(spec "com_ptr.hpp|97e721fbf762cd5c7a16043fe98dbd60b243fac43cb7df88a5ccf2281f6e491d" "com_utils.hpp|1d9b363343f5ab0e5e4672325b95c7e3782fecb30722dbd72ecdb3fc5728dff0")
    string(REPLACE "|" ";" parts "${spec}")
    list(GET parts 0 header)
    list(GET parts 1 expected)
    file(SHA256 "${LAB_DEPS}/reshade/source/${header}" actual)
    if(NOT actual STREQUAL expected)
      message(FATAL_ERROR "Reviewed ReShade v6.8.0 header changed: ${header}")
    endif()
  endforeach()
  add_library(lab_reshade_headers INTERFACE)
  target_include_directories(lab_reshade_headers INTERFACE "${LAB_DEPS}/reshade/include")
  target_include_directories(lab_reshade_headers SYSTEM INTERFACE "${LAB_DEPS}/reshade/source")

  # NVAPI headers shipped with Nsight Graphics: %OVERGLAZE_NVAPI_HEADERS%, else
  # under the toolchain root Enter-Environment.ps1 reports.
  if(DEFINED ENV{OVERGLAZE_NVAPI_HEADERS} AND NOT "$ENV{OVERGLAZE_NVAPI_HEADERS}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{OVERGLAZE_NVAPI_HEADERS}" LAB_NVAPI_HEADERS_DEFAULT)
  elseif(DEFINED ENV{OVERGLAZE_TOOLCHAIN_ROOT} AND NOT "$ENV{OVERGLAZE_TOOLCHAIN_ROOT}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{OVERGLAZE_TOOLCHAIN_ROOT}/NsightGraphics2026.2/target/Resources/nvapi" LAB_NVAPI_HEADERS_DEFAULT)
  else()
    set(LAB_NVAPI_HEADERS_DEFAULT "")
  endif()
  set(LAB_NVAPI_HEADERS "${LAB_NVAPI_HEADERS_DEFAULT}" CACHE PATH "Pinned NVAPI headers (Nsight Graphics 2026.2)")
  file(SHA256 "${LAB_NVAPI_HEADERS}/nvapi.h" LAB_NVAPI_SHA)
  file(SHA256 "${LAB_NVAPI_HEADERS}/nvapi_interface.h" LAB_NVAPI_INTERFACE_SHA)
  if(NOT LAB_NVAPI_SHA STREQUAL "b42060d558551af0e3d6ba2558fd9c0915f04f3ed2ea405d053eb099b40d08eb" OR
     NOT LAB_NVAPI_INTERFACE_SHA STREQUAL "63d27716114c5c0e6d51cc9a70a26e7a4e4b95c73905154d9b5a39ae3175770f")
    message(FATAL_ERROR "NVAPI header/interface baseline changed; review before compiling hooks")
  endif()
  add_library(lab_nvapi_headers INTERFACE)
  target_include_directories(lab_nvapi_headers SYSTEM INTERFACE "${LAB_NVAPI_HEADERS}")

  configure_file(configs/games.json games.json COPYONLY)
endif()

enable_testing()
# Order matters only for $<TARGET_FILE:...> references and the research track's
# use of controller sources; dependencies otherwise resolve at generate time.
add_subdirectory(native/core)
add_subdirectory(native/runtime)
add_subdirectory(native/provider)
add_subdirectory(native/viewer)
add_subdirectory(native/controller)
if(NOT LAB_TRACK STREQUAL "controller")
  add_subdirectory(native/research)
endif()
# Static dependency ban: file ownership, include graph and link graph against
# native/OWNERSHIP.json. Runs in every build directory; a tree without
# native/research checks the five public tracks only.
if(LAB_BUILD_PYTHON)
  add_test(NAME track_boundaries COMMAND "${LAB_BUILD_PYTHON}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/controller/test_track_boundaries.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(track_boundaries PROPERTIES LABELS "controller;research" TIMEOUT 60)
else()
  message(WARNING "No Python found: the track_boundaries test is not registered")
endif()
