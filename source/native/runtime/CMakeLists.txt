# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
# runtime: the NR core shared by both tracks. Allowed dependencies: core, runtime.
# Original-model session and Evaluate pipeline, colour codec/handoff, depth
# codec, exposure meter, completion timeline, frame retirement, live submission
# and native binding preservation. The live bridge DLL (overglaze_nvngx.dll) is
# assembled from these sources: the research variant in native/research, the
# controller variant here.
set_property(DIRECTORY PROPERTY LABELS "runtime;controller")

add_library(lab_runtime_includes INTERFACE)
target_include_directories(lab_runtime_includes INTERFACE include host)
target_link_libraries(lab_runtime_includes INTERFACE lab_core)

add_library(lab_native_resume STATIC src/native_resume.cpp src/resume_slots.cpp src/live_submission.cpp src/insertion_bindings.cpp src/insertion_slots.cpp)
target_link_libraries(lab_native_resume PUBLIC lab_runtime_includes lab_core minhook d3d12)
add_library(lab_color_codec STATIC src/color_codec.cpp src/color_handoff.cpp)
target_link_libraries(lab_color_codec PUBLIC lab_runtime_includes d3d12 d3dcompiler)
add_library(lab_prepost_ratio_codec STATIC src/prepost_ratio_codec.cpp src/exposure_meter.cpp)
target_link_libraries(lab_prepost_ratio_codec PUBLIC lab_runtime_includes d3d12 d3dcompiler)

# The runtime half of the live bridge client: identity gate, the 14 runtime
# exports, the NR mailbox and the nr_runtime publication. Both hosts use it; the
# research subclass adds the collectors through its virtual hooks.
add_library(lab_live_runtime_client STATIC src/live_runtime_client.cpp)
target_link_libraries(lab_live_runtime_client PUBLIC lab_runtime_includes lab_core)
# Command-signature creation metadata (runtime/host/indirect_layout.hpp), the
# early observer whose private data binding preservation reads at ExecuteIndirect.
# Shared: the controller host installs it on its early path too (007 First
# Light needs it), the research host as well.
add_library(lab_indirect_observer STATIC src/indirect_observer.cpp src/indirect_observer_slots.cpp)
target_link_libraries(lab_indirect_observer PUBLIC lab_runtime_includes lab_core minhook d3d12)

add_executable(lab_binding_heap_state_tests tests/binding_heap_state_tests.cpp)
target_link_libraries(lab_binding_heap_state_tests PRIVATE lab_native_resume)
add_test(NAME native_binding_heap_state COMMAND lab_binding_heap_state_tests)
add_executable(lab_live_submission_tests tests/live_submission_tests.cpp)
target_link_libraries(lab_live_submission_tests PRIVATE lab_native_resume d3d12 dxgi)
foreach(case normal wrong-queue duplicate reset reuse cross-thread probe-discard nr-discard)
  add_test(NAME live_submission_${case} COMMAND lab_live_submission_tests ${case})
  set_tests_properties(live_submission_${case} PROPERTIES TIMEOUT 15 LABELS gpu)
endforeach()
add_executable(lab_native_resume_tests tests/native_resume_tests.cpp)
target_link_libraries(lab_native_resume_tests PRIVATE lab_native_resume dxgi d3dcompiler)
add_test(NAME native_resume_hooks COMMAND lab_native_resume_tests)
add_test(NAME native_resume_wrong_queue COMMAND lab_native_resume_tests wrong-queue)
add_test(NAME native_resume_duplicate COMMAND lab_native_resume_tests duplicate)
set_tests_properties(native_resume_hooks native_resume_wrong_queue native_resume_duplicate PROPERTIES TIMEOUT 15 LABELS gpu)
add_test(NAME native_completion_only COMMAND lab_native_resume_tests completion-normal)
add_test(NAME native_completion_only_wrong_queue COMMAND lab_native_resume_tests completion-wrong-queue)
add_test(NAME native_completion_only_duplicate COMMAND lab_native_resume_tests completion-duplicate)
set_tests_properties(native_completion_only native_completion_only_wrong_queue native_completion_only_duplicate PROPERTIES TIMEOUT 15 LABELS gpu)
add_executable(lab_completion_timeline_tests tests/completion_timeline_tests.cpp)
target_link_libraries(lab_completion_timeline_tests PRIVATE lab_runtime_includes lab_core d3d12 dxgi)
add_test(NAME native_completion_timeline COMMAND lab_completion_timeline_tests)
set_tests_properties(native_completion_timeline PROPERTIES TIMEOUT 15 LABELS gpu)
add_executable(lab_compute_bindings_tests tests/compute_bindings_tests.cpp)
target_link_libraries(lab_compute_bindings_tests PRIVATE lab_runtime_includes)
add_test(NAME native_compute_bindings COMMAND lab_compute_bindings_tests)
set_tests_properties(native_compute_bindings PROPERTIES TIMEOUT 10)
add_executable(lab_exposure_control_tests tests/exposure_control_tests.cpp)
target_link_libraries(lab_exposure_control_tests PRIVATE lab_runtime_includes lab_core)
add_test(NAME native_exposure_control COMMAND lab_exposure_control_tests)
add_executable(lab_exposure_meter_harness tests/exposure_meter_harness.cpp)
target_link_libraries(lab_exposure_meter_harness PRIVATE lab_prepost_ratio_codec lab_core dxgi)
add_test(NAME native_exposure_meter COMMAND lab_exposure_meter_harness)
set_tests_properties(native_exposure_meter PROPERTIES LABELS gpu SKIP_RETURN_CODE 77)
add_executable(lab_nr_color_profile_tests tests/nr_color_profile_tests.cpp)
target_link_libraries(lab_nr_color_profile_tests PRIVATE lab_runtime_includes lab_core)
add_test(NAME native_nr_color_profile_boundary COMMAND lab_nr_color_profile_tests)
add_executable(lab_prepost_guard_tests tests/prepost_guard_tests.cpp)
target_link_libraries(lab_prepost_guard_tests PRIVATE lab_prepost_ratio_codec lab_color_codec lab_core dxgi)
add_test(NAME native_prepost_invalid_rgb_guard COMMAND lab_prepost_guard_tests)
set_tests_properties(native_prepost_invalid_rgb_guard PROPERTIES TIMEOUT 20 LABELS gpu SKIP_RETURN_CODE 77)
add_executable(lab_hdr_fixture_tests tests/hdr_fixture_tests.cpp)
target_link_libraries(lab_hdr_fixture_tests PRIVATE lab_runtime_includes)
add_test(NAME native_hdr_numeric_fixture COMMAND lab_hdr_fixture_tests)
add_executable(lab_embedded_control_tests tests/embedded_control_tests.cpp)
target_link_libraries(lab_embedded_control_tests PRIVATE lab_runtime_includes lab_core)
add_test(NAME native_embedded_control COMMAND lab_embedded_control_tests)
add_executable(lab_nr_compute_only_tests tests/nr_compute_only_tests.cpp)
target_link_libraries(lab_nr_compute_only_tests PRIVATE lab_runtime_includes lab_core)
add_test(NAME native_nr_compute_only COMMAND lab_nr_compute_only_tests)
# Two-plane depth/stencil -> single-plane R32_FLOAT, per pixel, on WARP. Handing
# NR the two-plane resource itself crashes the driver.
add_executable(lab_depth_codec_tests tests/depth_codec_tests.cpp src/depth_codec.cpp)
target_link_libraries(lab_depth_codec_tests PRIVATE lab_runtime_includes lab_core d3d12 dxgi d3dcompiler)
add_test(NAME native_depth_stencil_plane0 COMMAND lab_depth_codec_tests)
# Frame regions (Live ABI23, Hellblade 2): motion resampled onto the guide grid,
# depth and colour cropped to their regions, padding never written. On WARP.
add_executable(lab_region_crop_tests tests/region_crop_tests.cpp src/depth_codec.cpp src/motion_codec.cpp)
target_link_libraries(lab_region_crop_tests PRIVATE lab_runtime_includes lab_core lab_color_codec d3d12 dxgi d3dcompiler)
add_test(NAME native_region_crop COMMAND lab_region_crop_tests)
if(LAB_BUILD_NR_PROBE)
  add_executable(lab_nr_session_tests tests/nr_session_tests.cpp)
  target_link_libraries(lab_nr_session_tests PRIVATE lab_runtime_includes lab_ngx_headers)
  add_test(NAME native_nr_session_lifecycle COMMAND lab_nr_session_tests)

  # Controller variant of the live bridge: the shared NR runtime core with the
  # empty extension seam, so its link closure holds no capture/probe object at
  # all. Same file name as the research bridge, in its own subdirectory.
  add_library(lab_nr_bridge_controller SHARED src/nr_live.cpp src/nr_live_extension_none.cpp src/depth_codec.cpp src/motion_codec.cpp)
  target_link_libraries(lab_nr_bridge_controller PRIVATE lab_runtime_includes lab_ngx_headers lab_core
    lab_color_codec lab_prepost_ratio_codec lab_native_resume d3d12 dxgi d3dcompiler)
  set_target_properties(lab_nr_bridge_controller PROPERTIES PREFIX "" OUTPUT_NAME "overglaze_nvngx"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/controller"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/controller"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/controller")

  # Layout of the runtime ABI, and the export table of each bridge variant read
  # from the file on disk (no bridge is loaded). The research variant is checked
  # from native/research, where that target exists.
  add_executable(lab_live_abi_tests tests/live_abi_tests.cpp)
  target_link_libraries(lab_live_abi_tests PRIVATE lab_runtime_includes lab_core)
  if(EXISTS "${LAB_NATIVE}/research/include")
    target_include_directories(lab_live_abi_tests PRIVATE "${LAB_NATIVE}/research/include")
  endif()
  add_test(NAME live_abi_layout COMMAND lab_live_abi_tests)
  add_test(NAME controller_bridge_exports COMMAND lab_live_abi_tests controller $<TARGET_FILE:lab_nr_bridge_controller>)
  # Two negative paths: a required export that is missing must be
  # named in the fault, and a research host handed the controller bridge must
  # simply report research_available=false. The mock is deliberately NOT called
  # overglaze_nvngx.dll, so no host path derivation can ever pick it up.
  add_library(lab_mock_live_bridge SHARED tests/mock_live_bridge.cpp)
  set_target_properties(lab_mock_live_bridge PROPERTIES PREFIX "")
  add_executable(lab_live_runtime_client_tests tests/live_runtime_client_tests.cpp)
  target_link_libraries(lab_live_runtime_client_tests PRIVATE lab_runtime_includes lab_core)
  add_test(NAME live_runtime_client_exports COMMAND lab_live_runtime_client_tests
           $<TARGET_FILE:lab_mock_live_bridge> $<TARGET_FILE:lab_nr_bridge_controller>)
  set_tests_properties(live_runtime_client_exports PROPERTIES TIMEOUT 30)
endif()
