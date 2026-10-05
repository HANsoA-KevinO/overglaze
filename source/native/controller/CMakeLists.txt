# SPDX-FileCopyrightText: 2026 HANsoA-KevinO
# SPDX-License-Identifier: MIT
# controller: the NR Controller product surface. Allowed dependencies:
# core, runtime, provider, viewer, controller. Never a research library
# (see OWNERSHIP.json controller_ban; enforced by test_track_boundaries.py).
# In-game panel + input isolation, game manager + CLI, the desktop viewer
# (capture browsing + games page), the DXGI proxy thunks and the controller
# host DLL. overglaze_console is a dev-only protocol UI.
set_property(DIRECTORY PROPERTY LABELS "controller")

add_library(lab_controller_includes INTERFACE)
target_include_directories(lab_controller_includes INTERFACE include)
target_link_libraries(lab_controller_includes INTERFACE lab_provider_includes lab_viewer_includes)

# The controller variant of the in-game panel: same source as research's
# lab_overlay_research, without LAB_OVERLAY_RESEARCH, so the raw-capture UI and
# the capture_* status fields are compiled out. Compare split and compute-only
# stay. Never link both libraries into one binary (see OWNERSHIP controller_ban).
# Compiled once, linked by object reference into every consumer: the two panel
# variants and the input-block test. This is the ODR control; it is meaningful
# only because input_block.cpp uses no target-private macro or include path.
add_library(lab_input_block_objects OBJECT src/input_block.cpp)
target_link_libraries(lab_input_block_objects PUBLIC lab_controller_includes minhook)
add_library(lab_overlay STATIC src/overlay.cpp)
target_link_libraries(lab_overlay PUBLIC lab_controller_includes lab_core lab_ui_preferences lab_imgui minhook d3d12 d3dcompiler lab_input_block_objects)
add_executable(lab_input_block_tests tests/input_block_tests.cpp)
target_link_libraries(lab_input_block_tests PRIVATE lab_controller_includes minhook user32 lab_input_block_objects)
add_test(NAME native_input_block_cursor_clip COMMAND lab_input_block_tests)
set_tests_properties(native_input_block_cursor_clip PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
add_executable(lab_overlay_harness tests/overlay_harness.cpp)
target_link_libraries(lab_overlay_harness PRIVATE lab_overlay)
add_test(NAME native_overlay_synthetic COMMAND lab_overlay_harness --test)
set_tests_properties(native_overlay_synthetic PROPERTIES TIMEOUT 40)
add_executable(lab_overlay_demo WIN32 tests/overlay_harness.cpp)
target_compile_definitions(lab_overlay_demo PRIVATE LAB_OVERLAY_DEMO=1)
target_link_libraries(lab_overlay_demo PRIVATE lab_overlay dxgi)

add_executable(overglazectl src/ctl.cpp)
target_link_libraries(overglazectl PRIVATE lab_core)
add_executable(overglaze_host src/host.cpp)
target_link_libraries(overglaze_host PRIVATE lab_core)

# Where the Lab lives, for the programs that run OUTSIDE a game (viewer, CLI,
# launcher, manager): <root>\app\*.exe, a data\_build* directory, or an explicit
# overglaze-root.json beside the program. Its own library so
# that nothing in-game -- host, bridge, proxy -- and not the installation checker
# can ever link it; their hashes must not move for this.
add_library(lab_root_locator STATIC src/root_locator.cpp)
target_link_libraries(lab_root_locator PUBLIC lab_controller_includes lab_core)
add_executable(lab_root_locator_tests tests/root_locator_tests.cpp)
target_link_libraries(lab_root_locator_tests PRIVATE lab_root_locator)
add_test(NAME native_root_locator COMMAND lab_root_locator_tests)
set_tests_properties(native_root_locator PROPERTIES TIMEOUT 30)

# The game manager (out of game). game_reasons.cpp is the one code -> words
# table and game_presentation.cpp the backend-state -> three-axes mapping;
# neither is linked into anything that runs in a game.
add_library(lab_game_manager STATIC src/game_manager.cpp src/attach.cpp src/game_reasons.cpp src/game_presentation.cpp)
target_link_libraries(lab_game_manager PUBLIC lab_controller_includes lab_core lab_root_locator)
# The state mapping and the code table, pure: every backend state, every code.
add_executable(lab_game_status_tests tests/game_status_tests.cpp)
target_link_libraries(lab_game_status_tests PRIVATE lab_game_manager)
add_test(NAME native_game_status COMMAND lab_game_status_tests)
set_tests_properties(native_game_status PROPERTIES TIMEOUT 30)
add_executable(overglaze_games src/games_cli.cpp)
target_link_libraries(overglaze_games PRIVATE lab_game_manager)
# The Steam launch-option launcher for games that refuse any Lab file in their own
# directory (RE9): starts the game unchanged, injects on the player's first Insert.
add_executable(overglaze_launch WIN32 src/launcher.cpp)
target_link_libraries(overglaze_launch PRIVATE lab_game_manager)
add_executable(lab_launch_command_tests tests/launch_command_tests.cpp)
target_link_libraries(lab_launch_command_tests PRIVATE lab_controller_includes shell32)
add_test(NAME native_launch_command COMMAND lab_launch_command_tests)
add_executable(lab_game_manager_tests tests/game_manager_tests.cpp)
target_link_libraries(lab_game_manager_tests PRIVATE lab_game_manager)
add_test(NAME native_game_manager COMMAND lab_game_manager_tests)
set_tests_properties(native_game_manager PROPERTIES TIMEOUT 45)
add_executable(lab_game_manager_ui_tests tests/game_manager_ui_tests.cpp src/game_manager_ui.cpp)
target_link_libraries(lab_game_manager_ui_tests PRIVATE lab_game_manager lab_imgui shell32)
add_test(NAME native_game_manager_ui COMMAND lab_game_manager_ui_tests)
set_tests_properties(native_game_manager_ui PROPERTIES TIMEOUT 45)

add_executable(overglaze_viewer WIN32 src/viewer.cpp src/game_manager_ui.cpp)
if(LAB_BUILD_PYTHON)
  set(OVERGLAZE_ICON_PATH "${CMAKE_CURRENT_BINARY_DIR}/overglaze.ico")
  set(OVERGLAZE_RESOURCE_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/resources/resource.h")
  add_custom_command(OUTPUT "${OVERGLAZE_ICON_PATH}"
    COMMAND "${LAB_BUILD_PYTHON}" -B "${PROJECT_SOURCE_DIR}/python/generate_brand_icon.py"
      --source "${PROJECT_SOURCE_DIR}/../assets/branding/overglaze.svg" --output "${OVERGLAZE_ICON_PATH}"
    DEPENDS "${PROJECT_SOURCE_DIR}/python/generate_brand_icon.py" "${PROJECT_SOURCE_DIR}/../assets/branding/overglaze.svg"
    VERBATIM)
  configure_file(resources/application.rc.in "${CMAKE_CURRENT_BINARY_DIR}/application.rc" @ONLY)
  set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/application.rc" PROPERTIES OBJECT_DEPENDS "${OVERGLAZE_ICON_PATH}")
  target_sources(overglaze_viewer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/application.rc" "${OVERGLAZE_ICON_PATH}")
else()
  message(FATAL_ERROR "Python is required to generate the Overglaze application icon")
endif()
target_link_libraries(overglaze_viewer PRIVATE lab_controller_includes lab_game_manager lab_capture_library lab_preview_export lab_ui_preferences lab_imgui d3dcompiler dwmapi shell32)
add_test(NAME native_capture_viewer_smoke COMMAND overglaze_viewer --smoke-test)
set_tests_properties(native_capture_viewer_smoke PROPERTIES TIMEOUT 40)
add_test(NAME native_capture_viewer_controls COMMAND lab_capture_library_tests --viewer $<TARGET_FILE:overglaze_viewer>)
set_tests_properties(native_capture_viewer_controls PROPERTIES TIMEOUT 40)
add_test(NAME native_color_contract_viewer_controls COMMAND lab_color_contract_tests --viewer $<TARGET_FILE:overglaze_viewer>)
set_tests_properties(native_color_contract_viewer_controls PROPERTIES TIMEOUT 40)

add_executable(overglaze_console WIN32 src/console.cpp src/console_ui.cpp)
target_link_libraries(overglaze_console PRIVATE lab_controller_includes lab_core lab_pair_preview lab_root_locator lab_imgui windowscodecs shell32)
add_test(NAME console_smoke COMMAND overglaze_console --smoke-test)
add_test(NAME console_automatic_controls COMMAND overglaze_console --ui-test-automatic)
add_test(NAME console_interactions COMMAND overglaze_console --ui-test)
add_test(NAME console_narrow COMMAND overglaze_console --smoke-test --page 1 --size 1180 780)
add_test(NAME console_narrow_interactions COMMAND overglaze_console --ui-test --size 1180 780)
add_test(NAME console_hidpi COMMAND overglaze_console --smoke-test --size 2160 1410 --test-scale 1.5)
add_test(NAME console_hidpi_interactions COMMAND overglaze_console --ui-test --size 2160 1410 --test-scale 1.5)
add_test(NAME console_synthetic_ipc COMMAND overglaze_console --ui-test-synthetic)
add_test(NAME console_observer_readonly COMMAND overglaze_console --ui-test-observer)
add_test(NAME console_manual_capture COMMAND overglaze_console --ui-test-capture)
add_test(NAME console_diagnostics COMMAND overglaze_console --smoke-test --page 3)
add_test(NAME console_observation COMMAND overglaze_console --smoke-test --page 2)
add_test(NAME console_nr_preparation COMMAND overglaze_console --ui-test-nr-preparation)
add_test(NAME console_nr_preparation_narrow COMMAND overglaze_console --ui-test-nr-preparation --size 1180 780)
add_test(NAME console_nr_preparation_hidpi COMMAND overglaze_console --ui-test-nr-preparation --size 2160 1410 --test-scale 1.5)
add_test(NAME console_workbench COMMAND overglaze_console --ui-test-workbench)
add_test(NAME console_workbench_narrow COMMAND overglaze_console --ui-test-workbench --size 1180 780)
add_test(NAME console_workbench_hidpi COMMAND overglaze_console --ui-test-workbench --size 2160 1410 --test-scale 1.5)

add_executable(lab_startup_tests tests/startup_tests.cpp)
target_link_libraries(lab_startup_tests PRIVATE lab_controller_includes lab_core shell32)
add_test(NAME native_startup_failure_and_paths COMMAND lab_startup_tests)
add_executable(lab_display_shader_tests tests/display_shader_tests.cpp)
target_link_libraries(lab_display_shader_tests PRIVATE lab_controller_includes lab_core d3d11 d3dcompiler)
add_test(NAME native_display_shader_colors COMMAND lab_display_shader_tests)
set_tests_properties(native_display_shader_colors PROPERTIES TIMEOUT 40)
# The DXGI proxy thunks are product code (100% forwarding, never fail-fast);
# the ABI test exercises the ASM alone. The research host links the same files.
add_executable(lab_dxgi_forward_tests tests/dxgi_forward_tests.cpp src/dxgi_forward.asm)
add_test(NAME native_dxgi_forward_abi COMMAND lab_dxgi_forward_tests)

# The observation surface's report contract. No NGX, no GPU, no game: it exists
# because the payload was once built and then left out of the reported object.
add_executable(lab_ngx_observer_tests tests/ngx_observer_tests.cpp src/ngx_observer.cpp)
target_link_libraries(lab_ngx_observer_tests PRIVATE lab_controller_includes lab_core lab_ngx_headers minhook)
add_test(NAME native_ngx_observer COMMAND lab_ngx_observer_tests)

# NGX-direct RR -> NR frame translation (route ngx-rr), every refusal on WARP.
add_executable(lab_ngx_frame_tests tests/ngx_frame_tests.cpp)
target_link_libraries(lab_ngx_frame_tests PRIVATE lab_controller_includes lab_core d3d12 dxgi)
add_test(NAME native_ngx_frame COMMAND lab_ngx_frame_tests)

# NGX admission through the REAL evaluate detour on a fake loader in the test
# executable (WARP resources, no NGX): frame generation is forwarded, never
# offered (Hellblade 2). /OPT:NOICF keeps the three fake loader
# entries at distinct addresses for the hooks.
add_executable(lab_ngx_admission_tests tests/ngx_admission_tests.cpp src/ngx_observer.cpp)
target_link_libraries(lab_ngx_admission_tests PRIVATE lab_controller_includes lab_core lab_ngx_headers minhook d3d12 dxgi)
target_link_options(lab_ngx_admission_tests PRIVATE /OPT:NOICF)
add_test(NAME native_ngx_admission COMMAND lab_ngx_admission_tests)

# The controller's own in-game host and its dxgi.dll bootstrap. It links
# lab_provider and lab_live_runtime_client and NO research object; the linker map
# is checked by tests/controller/test_controller_link_closure.py.
add_library(lab_controller_host STATIC src/controller_host.cpp src/late_discovery.cpp
  src/late_slots.cpp src/ngx_observer.cpp src/present_queue_witness.cpp src/late_panel.cpp)
target_link_libraries(lab_controller_host PUBLIC lab_controller_includes lab_core lab_provider
  lab_live_runtime_client lab_overlay lab_native_resume lab_indirect_observer d3d12 dxgi)
# The observer needs the NGX Core ABI headers. It must NOT define
# NGX_SNIPPET_BUILD; lab_ngx_headers only adds the include path.
target_link_libraries(lab_controller_host PRIVATE lab_ngx_headers minhook)

# Late discovery, executed: a pre-existing swapchain, then discovery. Needs a
# real GPU and a window, so it runs serially with the other GPU work.
add_executable(lab_late_discovery_tests tests/late_discovery_tests.cpp src/late_discovery.cpp src/late_slots.cpp)
target_link_libraries(lab_late_discovery_tests PRIVATE lab_controller_includes lab_core minhook d3d12 dxgi)
add_test(NAME native_late_discovery COMMAND lab_late_discovery_tests)
set_tests_properties(native_late_discovery PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS gpu)

# The late panel's present queue, from evidence: a real swapchain, a second
# DIRECT queue and a compute queue; every refusal and the decision, executed.
add_executable(lab_present_queue_witness_tests tests/present_queue_witness_tests.cpp src/present_queue_witness.cpp src/late_slots.cpp)
target_link_libraries(lab_present_queue_witness_tests PRIVATE lab_controller_includes lab_core minhook d3d12 dxgi)
add_test(NAME native_present_queue_witness COMMAND lab_present_queue_witness_tests)
set_tests_properties(native_present_queue_witness PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS gpu)

# The late panel's swapchain binding on real windows (RE9 with frame
# generation): a new flip chain on the window of one the panel was drawing on
# must be created, the panel follows it, re-decides after a contradiction, and
# stays withdrawn past the bound.
add_executable(lab_late_panel_tests tests/late_panel_tests.cpp src/late_panel.cpp)
target_link_libraries(lab_late_panel_tests PRIVATE lab_overlay d3d12 dxgi)
add_test(NAME native_late_panel COMMAND lab_late_panel_tests)
set_tests_properties(native_late_panel PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

# The late-loading controller: injected into a running game, its payload lives in
# a subdirectory of the game rather than replacing dxgi.dll. Deliberately NOT
# named dxgi.dll, which the installation contract also enforces.
add_library(overglaze_controller_loader SHARED src/controller_loader.cpp)
target_link_libraries(overglaze_controller_loader PRIVATE lab_controller_host)
set_target_properties(overglaze_controller_loader PROPERTIES PREFIX "" OUTPUT_NAME "overglaze_controller"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/controller")
target_link_options(overglaze_controller_loader PRIVATE /OPT:NOICF
  "/MAP:${CMAKE_BINARY_DIR}/controller/overglaze_controller.map")

# The root proxy: the ONLY Lab file in a game's own directory under the root
# strategy, and the only one whose imports the game's loader must resolve before
# the game runs. It therefore links nothing -- no host, no Lab library -- and
# uses the static CRT so no MSVC runtime DLL appears in its import table either.
# The previous controller dxgi.dll linked the whole host and pulled 25 modules
# (d3d12, the shader compiler, WinTrust, OLE32, Shell32, the CRT, and dxgi.dll
# itself) into the loader lock; Alan Wake 2 hung there.
# test_proxy_imports.py fails the build if that ever creeps back.
add_library(overglaze_controller_proxy SHARED src/dxgi_proxy.cpp src/dxgi_forward.asm src/dxgi_proxy.def)
set_target_properties(overglaze_controller_proxy PROPERTIES PREFIX "" OUTPUT_NAME "dxgi"
  MSVC_RUNTIME_LIBRARY "MultiThreaded"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/controller"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/controller"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/controller")
target_link_options(overglaze_controller_proxy PRIVATE "/MAP:${CMAKE_BINARY_DIR}/controller/dxgi.map")
# The proxy's two modes on the built file: on Insert nothing is loaded while the
# game starts and the first factory is not held up; the ordinary mode still
# loads the host at once. No key is synthesised.
add_executable(lab_proxy_on_insert_tests tests/proxy_on_insert_tests.cpp)
add_dependencies(lab_proxy_on_insert_tests overglaze_controller_proxy)
add_test(NAME native_proxy_on_insert COMMAND lab_proxy_on_insert_tests $<TARGET_FILE:overglaze_controller_proxy> ${CMAKE_BINARY_DIR}/proxy-on-insert-test on-insert)
add_test(NAME native_proxy_at_first_factory COMMAND lab_proxy_on_insert_tests $<TARGET_FILE:overglaze_controller_proxy> ${CMAKE_BINARY_DIR}/proxy-first-factory-test at-first-factory)
set_tests_properties(native_proxy_on_insert native_proxy_at_first_factory PROPERTIES TIMEOUT 30)

# The controller host, executed: a synthetic game with real D3D12 and real NR.
# Driven by Test-ControllerHost.ps1, never by ctest (it needs an NVIDIA RTX GPU,
# the user's own model file and an isolated run directory). /OPT:NOICF keeps the synthetic
# Streamline functions at distinct addresses, which the fixture attach validates.
add_executable(lab_controller_harness tests/controller_harness.cpp)
target_link_libraries(lab_controller_harness PRIVATE lab_controller_includes lab_core lab_provider d3d12 dxgi d3dcompiler)
target_link_options(lab_controller_harness PRIVATE /OPT:NOICF)
