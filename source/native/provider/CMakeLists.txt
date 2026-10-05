# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
# provider: the Streamline public-boundary frame provider shared by both hosts.
# Allowed dependencies: core, runtime, provider.
# lab_provider is the pure provider library: no research
# header is reachable from provider/include and it links nothing of research, so
# a controller target may link it. The research track gets the same objects by
# linking it from lab_observer rather than recompiling the sources.
set_property(DIRECTORY PROPERTY LABELS "provider;controller")

add_library(lab_provider_includes INTERFACE)
target_include_directories(lab_provider_includes INTERFACE include)
target_link_libraries(lab_provider_includes INTERFACE lab_runtime_includes lab_streamline_headers)

add_library(lab_provider STATIC src/streamline.cpp src/sl_boundary.cpp src/sl_bindings.cpp
  src/sl_resource_lease.cpp src/rr_options.cpp src/workbench_adapter.cpp
  src/sl_host_contract.cpp src/sl_frame_provider.cpp src/sl_volatile_copy.cpp src/latency_markers.cpp)
target_link_libraries(lab_provider PUBLIC lab_provider_includes lab_core minhook d3d12 dxgi)

add_executable(lab_depth_projection_tests tests/depth_projection_tests.cpp)
target_link_libraries(lab_depth_projection_tests PRIVATE lab_provider_includes lab_core)
add_test(NAME native_depth_projection COMMAND lab_depth_projection_tests)

# The SL boundary -> NR frame translation, shared by both tracks. WARP only:
# real resource descriptors, no queue, no NR, no game.
add_executable(lab_sl_frame_provider_tests tests/sl_frame_provider_tests.cpp)
target_link_libraries(lab_sl_frame_provider_tests PRIVATE lab_provider)
add_test(NAME native_sl_frame_provider COMMAND lab_sl_frame_provider_tests)
set_tests_properties(native_sl_frame_provider PROPERTIES TIMEOUT 60)

# The public export-table resolution and tag-ABI selection (P1). No CommandLog,
# no Streamline module: kernel32 stands in as a real signed image.
add_executable(lab_public_export_tests tests/public_export_tests.cpp)
target_link_libraries(lab_public_export_tests PRIVATE lab_provider)
add_test(NAME native_public_export_resolution COMMAND lab_public_export_tests)

# Provider tests, home from native/research now that lab_provider links no research object.
add_executable(lab_workbench_adapter_tests tests/workbench_adapter_tests.cpp)
target_link_libraries(lab_workbench_adapter_tests PRIVATE lab_provider)
target_link_options(lab_workbench_adapter_tests PRIVATE /OPT:NOICF)
add_test(NAME native_workbench_adapter_admission COMMAND lab_workbench_adapter_tests)
add_executable(lab_public_admission_tests tests/public_admission_tests.cpp)
target_link_libraries(lab_public_admission_tests PRIVATE lab_provider d3d12 dxgi)
target_link_options(lab_public_admission_tests PRIVATE /OPT:NOICF)
add_test(NAME native_public_admission COMMAND lab_public_admission_tests)
# Research configuration (RR requires its private evidence) plus an SR target
# admitted on the public proof. Own process: the hooks are global.
add_test(NAME native_public_admission_per_feature COMMAND lab_public_admission_tests per-feature)
add_executable(lab_rr_options_tests tests/rr_options_tests.cpp)
target_link_libraries(lab_rr_options_tests PRIVATE lab_provider)
add_test(NAME native_rr_options_requests COMMAND lab_rr_options_tests)
set_tests_properties(native_rr_options_requests PROPERTIES TIMEOUT 15)
add_executable(lab_sl_boundary_tests tests/sl_boundary_tests.cpp)
target_link_libraries(lab_sl_boundary_tests PRIVATE lab_provider)
target_link_options(lab_sl_boundary_tests PRIVATE /OPT:NOICF)
add_test(NAME native_sl_bounded_boundary COMMAND lab_sl_boundary_tests)
add_executable(lab_sl_concurrency_tests tests/sl_concurrency_tests.cpp)
target_link_libraries(lab_sl_concurrency_tests PRIVATE lab_provider)
target_link_options(lab_sl_concurrency_tests PRIVATE /OPT:NOICF)
add_test(NAME native_sl_concurrency_scope COMMAND lab_sl_concurrency_tests)
add_executable(lab_sl_bindings_tests tests/sl_bindings_tests.cpp)
target_link_libraries(lab_sl_bindings_tests PRIVATE lab_provider)
add_test(NAME native_sl_exact_bindings COMMAND lab_sl_bindings_tests)
set_tests_properties(native_sl_exact_bindings PROPERTIES TIMEOUT 15)
add_executable(lab_frame_tag_hook_tests tests/frame_tag_hook_tests.cpp)
target_link_libraries(lab_frame_tag_hook_tests PRIVATE lab_provider)
target_link_options(lab_frame_tag_hook_tests PRIVATE /OPT:NOICF)
add_test(NAME native_frame_tag_hook COMMAND lab_frame_tag_hook_tests)
add_executable(lab_sl_resource_lease_tests tests/sl_resource_lease_tests.cpp)
target_link_libraries(lab_sl_resource_lease_tests PRIVATE lab_provider)
add_test(NAME native_sl_resource_entry_lease COMMAND lab_sl_resource_lease_tests)
set_tests_properties(native_sl_resource_entry_lease PROPERTIES TIMEOUT 15 LABELS gpu)
add_executable(lab_sl_route_tests tests/sl_route_tests.cpp)
target_link_libraries(lab_sl_route_tests PRIVATE lab_provider lab_ngx_headers)
add_test(NAME native_sl_nr_guide_contract COMMAND lab_sl_route_tests)
# Cyberpunk 2077 with DLSS-G on: eOnlyValidNow depth/motion copied at
# their slSetTag call on the game's list, through the real adapter and hooks.
# WARP: the copy really executes and is read back bit-exact; no NR, no game.
add_executable(lab_volatile_copy_tests tests/volatile_copy_tests.cpp)
target_link_libraries(lab_volatile_copy_tests PRIVATE lab_provider d3d12 dxgi)
target_link_options(lab_volatile_copy_tests PRIVATE /OPT:NOICF)
add_test(NAME native_only_valid_now_copies COMMAND lab_volatile_copy_tests)
set_tests_properties(native_only_valid_now_copies PROPERTIES TIMEOUT 60)
add_executable(lab_dual_tag_hook_tests tests/dual_tag_hook_tests.cpp)
target_link_libraries(lab_dual_tag_hook_tests PRIVATE lab_provider)
target_link_options(lab_dual_tag_hook_tests PRIVATE /OPT:NOICF)
add_test(NAME native_dual_tag_hook COMMAND lab_dual_tag_hook_tests)
# Cyberpunk 2077 with DLSS-G on: the game's own latency markers. Observe-only
# hooks on slPCLSetMarker /
# slReflexSetMarker resolved through the public slGetFeatureFunction, a lock-free
# marker ring and token table; the binding layer reads them at every Present.
# CPU only: synthetic marker functions in the test image are hooked for real.
add_executable(lab_latency_markers_tests tests/latency_markers_tests.cpp)
target_link_libraries(lab_latency_markers_tests PRIVATE lab_provider)
target_link_options(lab_latency_markers_tests PRIVATE /OPT:NOICF)
add_test(NAME native_chain_latency_markers COMMAND lab_latency_markers_tests)
set_tests_properties(native_chain_latency_markers PROPERTIES TIMEOUT 30)
