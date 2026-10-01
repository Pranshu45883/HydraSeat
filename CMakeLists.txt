cmake_minimum_required(VERSION 3.20)
project(HydraSeat VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include directories
include_directories(include .)

# Enable UTF-8 encoding for MSVC string literals
if(MSVC)
    add_compile_options(/utf-8 /FI"hydra/qt_msvc_compat.hpp")
endif()

# Windows system libraries for Win32 Raw Input, SetupAPI, DXGI, and Comctl32 GUI
if(WIN32)
    add_compile_definitions(_UNICODE UNICODE WIN32_LEAN_AND_MEAN)
    set(WIN32_LIBS User32.lib Cfgmgr32.lib Dxgi.lib Setupapi.lib Hid.lib Xinput.lib Comctl32.lib Ole32.lib Mmdevapi.lib runtimeobject.lib)

    # Check for PKEY_AudioEndpoint_StableId availability in the current SDK
    include(CheckCXXSourceCompiles)
    check_cxx_source_compiles("
        #include <windows.h>
        #include <mmdeviceapi.h>
        #include <functiondiscoverykeys_devpkey.h>
        int main() {
            (void)PKEY_AudioEndpoint_StableId;
            return 0;
        }
    " HYDRA_HAS_PKEY_AUDIOENDPOINT_STABLEID)

    if(HYDRA_HAS_PKEY_AUDIOENDPOINT_STABLEID)
        add_compile_definitions(HYDRA_HAS_PKEY_AUDIOENDPOINT_STABLEID=1)
    endif()
endif()

# Find Qt6 components if available
find_package(Qt6 COMPONENTS Core Widgets QUIET)

if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTORCC ON)
    set(UI_SOURCES
        ui/app_window.cpp
        ui/engine_poller.cpp
        ui/host_control_client.cpp
        ui/routing_controller.cpp
        ui/pages/dashboard_page.cpp
        ui/pages/seats_page.cpp
    ui/pages/settings_page.cpp
        ui/pages/applications_page.cpp
        ui/pages/audio_page.cpp
        ui/pages/hardware_page.cpp
        ui/pages/diagnostics_page.cpp
    )
    set(UI_HEADERS
        ui/app_window.hpp
        ui/engine_poller.hpp
        ui/host_control_client.hpp
        ui/routing_controller.hpp
        ui/pages/dashboard_page.hpp
        ui/pages/seats_page.hpp
        ui/pages/applications_page.hpp
        ui/pages/audio_page.hpp
        ui/pages/hardware_page.hpp
        ui/pages/diagnostics_page.hpp
    )
endif()

# Source files
set(HYDRA_SOURCES
    src/hardware_detector.cpp
    src/input_router.cpp
    src/display_manager.cpp
    src/workspace_manager.cpp
    src/runtime_authority.cpp
    src/controller_identity.cpp
    src/controller_inventory.cpp
    src/controller_io.cpp
    src/controller_pairing.cpp
    src/controller_virtual_xinput.cpp
    src/gui_win32.cpp
    ${UI_SOURCES}
)

if(WIN32)
    list(APPEND HYDRA_SOURCES
        src/audio_endpoint_inventory.cpp
        src/audio_session_observer.cpp
    )
endif()

set(HYDRA_HEADERS
    include/hydra/hardware_detector.hpp
    include/hydra/input_router.hpp
    include/hydra/display_manager.hpp
    include/hydra/workspace_manager.hpp
    include/hydra/runtime_authority.hpp
    include/hydra/process_identity.hpp
    include/hydra/controller_identity.hpp
    include/hydra/controller_inventory.hpp
    include/hydra/controller_io.hpp
    include/hydra/controller_pairing.hpp
    include/hydra/controller_virtual_xinput.hpp
    include/hydra/game_launcher.hpp
    include/hydra/gui_win32.hpp
    include/hydra/audio_endpoint_inventory.hpp
    include/hydra/audio_session_observer.hpp
    ${UI_HEADERS}
)

set(XINPUT_PROBE_SUPPORT_SOURCES
    src/xinput_probe.cpp
)
set(XINPUT_PROBE_SUPPORT_HEADERS
    include/hydra/xinput_probe.hpp
)
set(XINPUT_ABI_PROBE_SUPPORT_SOURCES
    src/xinput_abi_probe.cpp
)
set(XINPUT_ABI_PROBE_SUPPORT_HEADERS
    include/hydra/xinput_abi_probe.hpp
)

# Core Executable target (Windows GUI subsystem)
add_executable(HydraSeat WIN32 src/main.cpp ${HYDRA_SOURCES} ${HYDRA_HEADERS})

if(WIN32)
    target_link_libraries(HydraSeat PRIVATE ${WIN32_LIBS})

    # HydraAudioTest (WASAPI sine wave generator)
    add_executable(HydraAudioTest tests/hydra_audio_test.cpp)
    target_link_libraries(HydraAudioTest PRIVATE ${WIN32_LIBS})

    # HydraAudioRouter (CLI for undocumented AudioPolicyConfig)
    add_executable(HydraAudioRouter 
        tests/hydra_audio_router.cpp 
        src/audio_routing_experiment.cpp 
        src/audio_endpoint_inventory.cpp
        src/audio_session_observer.cpp
    )
    target_include_directories(HydraAudioRouter PRIVATE include)
    target_link_libraries(HydraAudioRouter PRIVATE ${WIN32_LIBS})
endif()

if(Qt6_FOUND)
    target_link_libraries(HydraSeat PRIVATE Qt6::Core Qt6::Widgets)
    target_compile_definitions(HydraSeat PRIVATE HYDRA_HAS_QT=1)
endif()

# Reset Tool Target
if(WIN32)
    add_executable(reset_input src/reset_input.cpp)
    target_link_libraries(reset_input PRIVATE User32.lib)

    add_executable(xinput_probe_game
        tools/xinput_probe_game.cpp
        ${XINPUT_PROBE_SUPPORT_SOURCES}
        ${XINPUT_PROBE_SUPPORT_HEADERS}
        src/virtual_xinput_protocol.cpp
        src/virtual_xinput_pipe.cpp
        src/virtual_xinput_service.cpp
        src/controller_io.cpp
        src/controller_inventory.cpp
        src/controller_identity.cpp
    )
    target_link_libraries(xinput_probe_game PRIVATE ${WIN32_LIBS})
    if(MINGW)
        target_link_options(xinput_probe_game PRIVATE -municode)
    endif()

    add_executable(xinput_adapter_exports
        tests/test_xinput_adapter_exports.cpp
    )
    if(MINGW)
        target_link_options(xinput_adapter_exports PRIVATE -municode)
    endif()

    add_executable(xinput_abi_probe_game
        tools/xinput_abi_probe_game.cpp
        ${XINPUT_ABI_PROBE_SUPPORT_SOURCES}
        ${XINPUT_ABI_PROBE_SUPPORT_HEADERS}
    )
    if(MINGW)
        target_link_options(xinput_abi_probe_game PRIVATE -municode)
    endif()

    add_library(hydra_xinput_adapter SHARED
        src/xinput_adapter_dll.cpp
        src/xinput_adapter_session.cpp
        src/virtual_xinput_protocol.cpp
        src/virtual_xinput_pipe.cpp
        src/hydra_xinput_adapter.def
        include/hydra/xinput_adapter_session.hpp
        include/hydra/virtual_xinput_protocol.hpp
        include/hydra/virtual_xinput_pipe.hpp
    )
    target_compile_definitions(
        hydra_xinput_adapter PRIVATE HYDRA_VIRTUAL_XINPUT_CLIENT_ONLY=1)
    set_target_properties(
        hydra_xinput_adapter PROPERTIES OUTPUT_NAME "hydra_xinput_adapter")
    add_dependencies(xinput_adapter_exports hydra_xinput_adapter)
endif()

# Test Suite Executable Target
enable_testing()
add_executable(hydra_tests
    tests/test_hydra.cpp
    tests/test_controller_identity.cpp
    tests/test_controller_pairing.cpp
    tests/test_controller_virtual_xinput.cpp
    tests/test_virtual_xinput_protocol.cpp
    tests/test_virtual_xinput_service.cpp
    tests/test_virtual_xinput_pipe.cpp
    tests/test_xinput_probe_game.cpp
    tests/test_xinput_adapter_session.cpp
    tests/test_xinput_abi_probe.cpp
    src/virtual_xinput_protocol.cpp
    src/virtual_xinput_service.cpp
    src/virtual_xinput_pipe.cpp
    src/xinput_adapter_session.cpp
    include/hydra/virtual_xinput_protocol.hpp
    include/hydra/virtual_xinput_service.hpp
    include/hydra/virtual_xinput_pipe.hpp
    include/hydra/xinput_adapter_session.hpp
    ${HYDRA_SOURCES}
    ${HYDRA_HEADERS}
    ${XINPUT_PROBE_SUPPORT_SOURCES}
    ${XINPUT_PROBE_SUPPORT_HEADERS}
    ${XINPUT_ABI_PROBE_SUPPORT_SOURCES}
    ${XINPUT_ABI_PROBE_SUPPORT_HEADERS}
)
if(WIN32)
    target_link_libraries(hydra_tests PRIVATE ${WIN32_LIBS})

    add_executable(xinput_process_isolation
        tests/test_xinput_process_isolation.cpp
        src/virtual_xinput_protocol.cpp
        src/virtual_xinput_service.cpp
        src/virtual_xinput_pipe.cpp
        src/controller_virtual_xinput.cpp
        src/controller_io.cpp
        src/controller_inventory.cpp
        src/controller_identity.cpp
    )
    target_link_libraries(xinput_process_isolation PRIVATE ${WIN32_LIBS})
    add_dependencies(xinput_process_isolation xinput_probe_game)

    add_executable(xinput_adapter_process_isolation
        tests/test_xinput_adapter_process_isolation.cpp
        src/virtual_xinput_protocol.cpp
        src/virtual_xinput_service.cpp
        src/virtual_xinput_pipe.cpp
        src/controller_virtual_xinput.cpp
        src/controller_io.cpp
        src/controller_inventory.cpp
        src/controller_identity.cpp
    )
    target_link_libraries(xinput_adapter_process_isolation PRIVATE ${WIN32_LIBS})
    add_dependencies(
        xinput_adapter_process_isolation
        xinput_abi_probe_game
        hydra_xinput_adapter)
endif()
if(Qt6_FOUND)
    target_link_libraries(hydra_tests PRIVATE Qt6::Core Qt6::Widgets)
endif()
add_test(NAME EngineUnitTests COMMAND hydra_tests)
if(WIN32)
    add_executable(hydra_process_owner_child
        tests/process_owner_child.cpp
    )
    if(MINGW)
        target_link_options(hydra_process_owner_child PRIVATE -municode)
    endif()

    add_test(
        NAME XInputProcessIsolation
        COMMAND xinput_process_isolation $<TARGET_FILE:xinput_probe_game>
    )
    add_test(
        NAME XInputAdapterExports
        COMMAND xinput_adapter_exports $<TARGET_FILE:hydra_xinput_adapter>
    )
    add_test(
        NAME XInputAdapterProcessIsolation
        COMMAND xinput_adapter_process_isolation
                $<TARGET_FILE:xinput_abi_probe_game>
                $<TARGET_FILE:hydra_xinput_adapter>
    )
endif()

# Process/display ownership foundation. These modules remain separate from the
# GUI/runtime state machine until the host/client integration contract lands.
add_library(hydra_process_display STATIC
    src/process_group.cpp
    src/process_launcher.cpp
    src/display_topology.cpp
    src/display_recovery.cpp
    src/seat_display_layout.cpp
    src/window_tracker.cpp
    src/window_policy.cpp
    src/window_placement.cpp
)
target_include_directories(hydra_process_display PUBLIC include)
if(WIN32)
    target_link_libraries(hydra_process_display PUBLIC ${WIN32_LIBS})
endif()

add_executable(process_group_tests tests/test_process_group.cpp)
target_link_libraries(process_group_tests PRIVATE hydra_process_display)
add_test(NAME ProcessGroupTests COMMAND process_group_tests)

add_executable(display_topology_tests tests/test_display_topology.cpp)
target_link_libraries(display_topology_tests PRIVATE hydra_process_display)
add_test(NAME DisplayTopologyTests COMMAND display_topology_tests)

add_executable(display_recovery_tests tests/test_display_recovery.cpp)
target_link_libraries(display_recovery_tests PRIVATE hydra_process_display)
add_test(NAME DisplayRecoveryTests COMMAND display_recovery_tests)

add_executable(seat_display_layout_tests tests/test_seat_display_layout.cpp)
target_link_libraries(seat_display_layout_tests PRIVATE hydra_process_display)
add_test(NAME SeatDisplayLayoutTests COMMAND seat_display_layout_tests)

add_executable(window_policy_tests tests/test_window_policy.cpp)
target_link_libraries(window_policy_tests PRIVATE hydra_process_display)
add_test(NAME WindowPolicyTests COMMAND window_policy_tests)

add_executable(window_tracker_tests tests/test_window_tracker.cpp)
target_link_libraries(window_tracker_tests PRIVATE hydra_process_display)
add_test(NAME WindowTrackerTests COMMAND window_tracker_tests)

if(WIN32)
    add_executable(hydra_process_tree_child tests/process_tree_child.cpp)
    add_executable(hydra_window_test_app tests/window_test_app.cpp)
    target_link_libraries(hydra_window_test_app PRIVATE User32.lib)
    add_dependencies(process_group_tests hydra_process_tree_child)
    add_dependencies(window_policy_tests hydra_window_test_app)
    add_dependencies(window_tracker_tests hydra_window_test_app)
endif()

add_library(hydra_seat_policy STATIC
    src/seat_runtime_policy.cpp
)
target_include_directories(hydra_seat_policy PUBLIC include)
target_link_libraries(hydra_seat_policy PUBLIC hydra_process_display)

add_executable(hardware_identity_tests tests/test_hardware_identity.cpp)
target_include_directories(hardware_identity_tests PRIVATE include)
add_test(NAME HardwareIdentityTests COMMAND hardware_identity_tests)

add_executable(seat_runtime_policy_tests tests/test_seat_runtime_policy.cpp)
target_link_libraries(seat_runtime_policy_tests PRIVATE hydra_seat_policy)
add_test(NAME SeatRuntimePolicyTests COMMAND seat_runtime_policy_tests)

add_library(hydra_input_isolation STATIC
    src/directinput_policy.cpp
    src/hidhide_probe.cpp
    src/input_isolation.cpp
    src/input_metrics.cpp
    src/raw_input_probe_trace.cpp
    src/raw_input_utils.cpp
    src/workspace_manager.cpp
)
target_include_directories(hydra_input_isolation PUBLIC include)
if(WIN32)
    target_link_libraries(hydra_input_isolation PUBLIC ${WIN32_LIBS} Version.lib)
endif()

add_executable(directinput_policy_tests tests/test_directinput_policy.cpp)
target_link_libraries(directinput_policy_tests PRIVATE hydra_input_isolation)
add_test(NAME DirectInputPolicyTests COMMAND directinput_policy_tests)

add_executable(hidhide_probe_tests tests/test_hidhide_probe.cpp)
target_link_libraries(hidhide_probe_tests PRIVATE hydra_input_isolation)
add_test(NAME HidHideProbeTests COMMAND hidhide_probe_tests)

add_executable(input_metrics_tests tests/test_input_metrics.cpp)
target_link_libraries(input_metrics_tests PRIVATE hydra_input_isolation)
add_test(NAME InputMetricsTests COMMAND input_metrics_tests)

add_executable(isolation_planner_tests tests/test_isolation_planner.cpp)
target_link_libraries(isolation_planner_tests PRIVATE hydra_input_isolation)
add_test(NAME IsolationPlannerTests COMMAND isolation_planner_tests)

add_executable(raw_input_probe_trace_tests tests/test_raw_input_probe_trace.cpp)
target_link_libraries(raw_input_probe_trace_tests PRIVATE hydra_input_isolation)
add_test(NAME RawInputProbeTraceTests COMMAND raw_input_probe_trace_tests)

add_library(hydra_recovery_core STATIC
    src/watchdog_protocol.cpp
    src/rollback_registry.cpp
    src/startup_policy.cpp
)
target_include_directories(hydra_recovery_core PUBLIC include)
if(WIN32)
    target_link_libraries(hydra_recovery_core PUBLIC Kernel32.lib)
endif()

add_executable(watchdog_core_tests tests/test_watchdog_core.cpp)
target_link_libraries(watchdog_core_tests PRIVATE hydra_recovery_core)
add_test(NAME WatchdogCoreTests COMMAND watchdog_core_tests)

add_executable(startup_policy_tests tests/test_startup_policy.cpp)
target_link_libraries(startup_policy_tests PRIVATE hydra_recovery_core)
add_test(NAME StartupPolicyTests COMMAND startup_policy_tests)

# Canonical host authority boundary. This target owns SessionController behind a
# pointer-free, bounded protocol model; transport and launch mutation land in
# dependent backend PRs.
add_library(hydra_host_authority STATIC
    src/host_protocol.cpp
    src/runtime_host.cpp
    src/runtime_authority.cpp
    src/controller_identity.cpp
    src/controller_inventory.cpp
    src/controller_io.cpp
    src/controller_virtual_xinput.cpp
)
target_include_directories(hydra_host_authority PUBLIC include)
if(WIN32)
    target_link_libraries(hydra_host_authority PUBLIC ${WIN32_LIBS})
endif()

add_executable(host_protocol_v2_tests tests/test_host_protocol_v2.cpp)
target_link_libraries(host_protocol_v2_tests PRIVATE hydra_host_authority)
add_test(NAME HostProtocolV2Tests COMMAND host_protocol_v2_tests)

add_executable(runtime_host_v1_tests tests/test_runtime_host_v1.cpp)
target_link_libraries(runtime_host_v1_tests PRIVATE hydra_host_authority)
add_test(NAME RuntimeHostV1Tests COMMAND runtime_host_v1_tests)

add_executable(runtime_lease_authority_tests tests/test_runtime_leases.cpp)
target_link_libraries(runtime_lease_authority_tests PRIVATE hydra_host_authority)
add_test(NAME RuntimeLeaseAuthorityTests COMMAND runtime_lease_authority_tests)

# Host-owned process launch boundary. GameLauncher delegates every runtime
# mutation to RuntimeHost in production, while retaining a narrow
# SessionController compatibility constructor for focused lower-level tests.
add_library(hydra_game_launcher STATIC
    src/game_launcher.cpp
    src/virtual_xinput_protocol.cpp
    src/virtual_xinput_service.cpp
    src/virtual_xinput_pipe.cpp
)
target_include_directories(hydra_game_launcher PUBLIC include)
target_link_libraries(hydra_game_launcher PUBLIC hydra_host_authority)
if(WIN32)
    target_link_libraries(hydra_game_launcher PUBLIC ${WIN32_LIBS})
endif()

# The legacy executable/test surfaces consume the same launcher library instead
# of compiling a second authority implementation.
target_link_libraries(HydraSeat PRIVATE hydra_game_launcher)
target_link_libraries(hydra_tests PRIVATE hydra_game_launcher)

if(WIN32)
    add_executable(game_launcher_process_ownership
        tests/test_game_launcher_process_ownership.cpp
    )
    target_link_libraries(game_launcher_process_ownership PRIVATE hydra_game_launcher)
    if(MINGW)
        target_link_options(game_launcher_process_ownership PRIVATE -municode)
    endif()
    add_dependencies(game_launcher_process_ownership hydra_process_owner_child)
    add_test(
        NAME GameLauncherProcessOwnership
        COMMAND game_launcher_process_ownership $<TARGET_FILE:hydra_process_owner_child>
    )

    add_executable(game_launcher_xinput_integration
        tests/test_game_launcher_xinput_integration.cpp
    )
    target_link_libraries(game_launcher_xinput_integration PRIVATE hydra_game_launcher)
    if(MINGW)
        target_link_options(game_launcher_xinput_integration PRIVATE -municode)
    endif()
    add_dependencies(game_launcher_xinput_integration hydra_process_owner_child)
    add_test(
        NAME GameLauncherXInputIntegration
        COMMAND game_launcher_xinput_integration $<TARGET_FILE:hydra_process_owner_child>
    )
endif()

if(WIN32)
    add_library(hydra_windows_audio_router STATIC
        src/windows_audio_router.cpp
        src/audio_session_observer.cpp
    )
    target_include_directories(hydra_windows_audio_router PUBLIC include)
    target_link_libraries(hydra_windows_audio_router PUBLIC ${WIN32_LIBS})
endif()

add_library(hydra_host_transport STATIC
    src/host_transport.cpp
)
target_include_directories(hydra_host_transport PUBLIC include)
target_link_libraries(hydra_host_transport
    PUBLIC hydra_host_authority
    PRIVATE hydra_game_launcher
)
if(WIN32)
    target_link_libraries(hydra_host_transport PUBLIC
        Kernel32.lib
        hydra_windows_audio_router
    )
endif()

if(Qt6_FOUND)
    # The GUI is an IPC client of the canonical host. It does not construct
    # SessionController/RuntimeHost authority in-process.
    target_link_libraries(HydraSeat PRIVATE hydra_host_transport)
    target_link_libraries(hydra_tests PRIVATE hydra_host_transport)
endif()

add_executable(host_transport_session_tests
    tests/test_host_transport_session.cpp
)
target_link_libraries(host_transport_session_tests PRIVATE hydra_host_transport)
add_test(NAME HostTransportSessionTests COMMAND host_transport_session_tests)

add_executable(host_pipe_tests tests/test_host_pipe.cpp)
target_link_libraries(host_pipe_tests PRIVATE hydra_host_transport)
if(WIN32)
    add_dependencies(host_pipe_tests hydra_process_owner_child)
    add_test(
        NAME HostPipeTests
        COMMAND host_pipe_tests $<TARGET_FILE:hydra_process_owner_child>
    )
else()
    add_test(NAME HostPipeTests COMMAND host_pipe_tests)
endif()

if(WIN32)
    add_executable(hydra_host src/host_main.cpp)
    target_link_libraries(hydra_host PRIVATE hydra_host_transport)

    add_executable(hydraseat_hostctl src/hostctl_main.cpp)
    target_link_libraries(hydraseat_hostctl PRIVATE hydra_host_transport)
    if(MINGW)
        target_link_options(hydraseat_hostctl PRIVATE -municode)
    endif()
endif()

# Exact-owned audio routing transaction boundary. The default backend is
# observe-only; no undocumented Windows routing mutation is enabled here.
add_library(hydra_audio_routing_contract STATIC
    src/audio_routing.cpp
)
target_include_directories(hydra_audio_routing_contract PUBLIC include)
if(WIN32)
    target_sources(hydra_audio_routing_contract PRIVATE
        src/audio_session_observer.cpp
    )
    target_link_libraries(hydra_audio_routing_contract PUBLIC ${WIN32_LIBS})
endif()

add_executable(audio_routing_contract_tests tests/test_audio_routing.cpp)
target_link_libraries(audio_routing_contract_tests PRIVATE
    hydra_audio_routing_contract
)
add_test(NAME AudioRoutingContractTests COMMAND audio_routing_contract_tests)

# Canonical Seat/profile/provider foundation. This activates the previously
# preserved backend schema/provider code without changing the legacy UI model.
add_library(hydra_profile_provider_foundation STATIC
    src/profile_schema.cpp
    src/game_catalog.cpp
    src/provider_adapter.cpp
    src/custom_executable_provider.cpp
    src/steam_provider.cpp
)
target_include_directories(hydra_profile_provider_foundation PUBLIC include)
if(WIN32)
    target_link_libraries(hydra_profile_provider_foundation PUBLIC ${WIN32_LIBS})
endif()

add_executable(profile_schema_tests tests/test_profile_schema.cpp)
target_link_libraries(profile_schema_tests PRIVATE hydra_profile_provider_foundation)
add_test(NAME ProfileSchemaTests COMMAND profile_schema_tests)

add_executable(provider_adapter_tests tests/test_provider_adapter.cpp)
target_link_libraries(provider_adapter_tests PRIVATE hydra_profile_provider_foundation)
add_test(NAME ProviderAdapterTests COMMAND provider_adapter_tests)

add_executable(custom_executable_provider_tests
    tests/test_custom_executable_provider.cpp
)
target_link_libraries(custom_executable_provider_tests PRIVATE
    hydra_profile_provider_foundation
)
add_test(NAME CustomExecutableProviderTests COMMAND custom_executable_provider_tests)

add_executable(steam_provider_tests tests/test_steam_provider.cpp)
target_link_libraries(steam_provider_tests PRIVATE hydra_profile_provider_foundation)
# Fixtures intentionally use Windows path semantics; HydraSeat is Windows-only.
if(WIN32)
    add_test(NAME SteamProviderTests COMMAND steam_provider_tests)
endif()

add_executable(game_catalog_tests tests/test_game_catalog.cpp)
target_link_libraries(game_catalog_tests PRIVATE hydra_profile_provider_foundation)
add_test(NAME GameCatalogTests COMMAND game_catalog_tests)

# Backend-only per-Seat game lifecycle. This layer owns the two-Seat state
# machine but delegates concrete launch/mutation work to ISeatGameInstance.
add_library(hydra_seat_game_lifecycle STATIC
    src/seat_game_lifecycle.cpp
)
target_include_directories(hydra_seat_game_lifecycle PUBLIC include)

add_executable(seat_game_lifecycle_tests
    tests/test_seat_game_lifecycle.cpp
)
target_link_libraries(seat_game_lifecycle_tests PRIVATE
    hydra_seat_game_lifecycle
)
add_test(NAME SeatGameLifecycleTests COMMAND seat_game_lifecycle_tests)

add_library(hydra_two_seat_launch STATIC
    src/runtime_state.cpp
    src/two_seat_launch.cpp
)
target_include_directories(hydra_two_seat_launch PUBLIC include)
target_link_libraries(hydra_two_seat_launch PUBLIC
    hydra_seat_game_lifecycle
)

add_executable(two_seat_launch_foundation_tests
    tests/test_two_seat_launch_foundation.cpp
)
target_link_libraries(two_seat_launch_foundation_tests PRIVATE
    hydra_two_seat_launch
)
add_test(NAME TwoSeatLaunchFoundationTests
    COMMAND two_seat_launch_foundation_tests
)

add_library(hydra_virtual_input_core STATIC
    src/gate_c_protocol.cpp
    src/gate_c_transport.cpp
    src/gate_c_probe_snapshot.cpp
    src/virtual_input_state.cpp
    src/virtual_raw_input.cpp
    src/virtual_xinput_state.cpp
)
target_include_directories(hydra_virtual_input_core PUBLIC include)
if(WIN32)
    target_link_libraries(hydra_virtual_input_core PUBLIC Bcrypt.lib)
endif()

foreach(test_name IN ITEMS
    gate_c_protocol
    gate_c_probe_snapshot
    virtual_raw_input
    virtual_xinput_state
)
    add_executable(${test_name}_core_tests tests/test_${test_name}.cpp)
    target_link_libraries(${test_name}_core_tests PRIVATE
        hydra_virtual_input_core
    )
endforeach()

add_test(NAME GateCProtocolCoreTests COMMAND gate_c_protocol_core_tests)
add_test(NAME GateCProbeSnapshotCoreTests COMMAND gate_c_probe_snapshot_core_tests)
add_test(NAME VirtualRawInputCoreTests COMMAND virtual_raw_input_core_tests)
add_test(NAME VirtualXInputStateCoreTests COMMAND virtual_xinput_state_core_tests)

add_library(hydra_gate_c_architecture STATIC
    src/gate_c_architecture.cpp
)
target_include_directories(hydra_gate_c_architecture PUBLIC include)

add_executable(gate_c_architecture_backend_tests
    tests/test_gate_c_architecture.cpp
)
target_link_libraries(gate_c_architecture_backend_tests PRIVATE
    hydra_gate_c_architecture
)
add_test(NAME GateCArchitectureBackendTests
    COMMAND gate_c_architecture_backend_tests
)

if(WIN32)
    add_library(hydra_gate_c_adapter SHARED
        src/gate_c_adapter.cpp
    )
    target_compile_definitions(hydra_gate_c_adapter PRIVATE
        HYDRA_GATE_C_ADAPTER_BUILD=1
    )
    target_include_directories(hydra_gate_c_adapter PUBLIC include)
    target_link_libraries(hydra_gate_c_adapter PRIVATE
        hydra_virtual_input_core
    )

    add_library(hydra_gate_c_shim_support STATIC
        src/gate_c_cursor_focus_shim.cpp
        src/gate_c_raw_input_shim.cpp
        src/gate_c_shim_iat.cpp
    )
    target_include_directories(hydra_gate_c_shim_support PUBLIC include)

    add_library(hydra_gate_c_shim SHARED
        src/gate_c_shim.cpp
    )
    target_compile_definitions(hydra_gate_c_shim PRIVATE
        HYDRA_GATE_C_SHIM_BUILD=1
    )
    target_include_directories(hydra_gate_c_shim PUBLIC include)
    target_link_libraries(hydra_gate_c_shim PRIVATE
        hydra_gate_c_adapter
        hydra_gate_c_shim_support
        User32.lib
    )
    set_target_properties(hydra_gate_c_shim PROPERTIES
        OUTPUT_NAME hydra_gate_c_shim
    )

    add_executable(hydra_gate_c_target src/gate_c_target.cpp)
    target_link_libraries(hydra_gate_c_target PRIVATE
        hydra_gate_c_architecture
        hydra_virtual_input_core
        hydra_gate_c_adapter
        User32.lib
    )
    if(MINGW)
        target_link_options(hydra_gate_c_target PRIVATE -municode)
    endif()

    add_executable(hydra_gate_c_api_probe src/gate_c_api_probe.cpp)
    target_link_libraries(hydra_gate_c_api_probe PRIVATE
        hydra_gate_c_architecture
        hydra_virtual_input_core
        hydra_gate_c_adapter
        User32.lib
    )
    add_dependencies(hydra_gate_c_api_probe hydra_gate_c_shim)
    if(MINGW)
        target_link_options(hydra_gate_c_api_probe PRIVATE -municode)
    endif()

    add_library(hydra_gate_c_external_bridge SHARED
        src/gate_c_external_bridge.cpp
    )
    target_include_directories(hydra_gate_c_external_bridge PUBLIC include)
    target_link_libraries(hydra_gate_c_external_bridge PRIVATE
        hydra_gate_c_architecture
        hydra_virtual_input_core
        hydra_gate_c_adapter
        hydra_gate_c_shim
        User32.lib
    )
    set_target_properties(hydra_gate_c_external_bridge PROPERTIES
        OUTPUT_NAME hydra_gate_c_external_bridge
    )

    add_executable(hydra_gate_c_external_harness
        src/gate_c_external_harness.cpp
    )
    target_link_libraries(hydra_gate_c_external_harness PRIVATE
        hydra_gate_c_architecture
        hydra_virtual_input_core
        User32.lib
    )
    if(MINGW)
        target_link_options(hydra_gate_c_external_harness PRIVATE -municode)
    endif()

    add_executable(gate_c_adapter_backend_tests
        tests/test_gate_c_adapter.cpp
    )
    target_link_libraries(gate_c_adapter_backend_tests PRIVATE
        hydra_gate_c_adapter
    )

    add_executable(gate_c_xinput_adapter_backend_tests
        tests/test_gate_c_xinput_adapter.cpp
    )
    target_link_libraries(gate_c_xinput_adapter_backend_tests PRIVATE
        hydra_gate_c_adapter
    )

    add_executable(gate_c_polling_shim_backend_tests
        tests/test_gate_c_polling_shim.cpp
    )
    target_link_libraries(gate_c_polling_shim_backend_tests PRIVATE
        hydra_gate_c_adapter
        hydra_gate_c_shim
        hydra_gate_c_shim_support
        User32.lib
    )

    add_executable(gate_c_cursor_focus_shim_backend_tests
        tests/test_gate_c_cursor_focus_shim.cpp
    )
    target_link_libraries(gate_c_cursor_focus_shim_backend_tests PRIVATE
        hydra_gate_c_shim_support
    )

    add_executable(gate_c_raw_input_shim_backend_tests
        tests/test_gate_c_raw_input_shim.cpp
    )
    target_link_libraries(gate_c_raw_input_shim_backend_tests PRIVATE
        hydra_gate_c_shim_support
    )

    add_test(NAME GateCAdapterBackendTests
        COMMAND gate_c_adapter_backend_tests
    )
    add_test(NAME GateCXInputAdapterBackendTests
        COMMAND gate_c_xinput_adapter_backend_tests
    )
    add_test(NAME GateCPollingShimBackendTests
        COMMAND gate_c_polling_shim_backend_tests
    )
    add_test(NAME GateCCursorFocusShimBackendTests
        COMMAND gate_c_cursor_focus_shim_backend_tests
    )
    add_test(NAME GateCRawInputShimBackendTests
        COMMAND gate_c_raw_input_shim_backend_tests
    )
    add_test(NAME GateCTargetBackendSelfTest
        COMMAND hydra_gate_c_target --self-test
    )
    add_test(NAME GateCExternalHarnessBackendHelp
        COMMAND hydra_gate_c_external_harness --help
    )
endif()

add_library(hydra_hidhide_session_guard STATIC
    src/hidhide_session_backend.cpp
    src/hidhide_session_native.cpp
)
target_include_directories(hydra_hidhide_session_guard PUBLIC include)
target_link_libraries(hydra_hidhide_session_guard PUBLIC
    hydra_input_isolation
)
if(WIN32)
    target_link_libraries(hydra_hidhide_session_guard PUBLIC
        Advapi32.lib
        Cfgmgr32.lib
    )
endif()

add_library(hydra_hidhide_recovery STATIC
    src/hidhide_session_recovery.cpp
)
target_include_directories(hydra_hidhide_recovery PUBLIC include)
target_link_libraries(hydra_hidhide_recovery PUBLIC
    hydra_hidhide_session_guard
    hydra_recovery_core
)

add_executable(hidhide_session_guard_tests
    tests/test_hidhide_session_backend.cpp
)
target_link_libraries(hidhide_session_guard_tests PRIVATE
    hydra_hidhide_session_guard
)
add_test(NAME HidHideSessionGuardTests COMMAND hidhide_session_guard_tests)

add_executable(hidhide_session_recovery_backend_tests
    tests/test_hidhide_session_recovery.cpp
)
target_link_libraries(hidhide_session_recovery_backend_tests PRIVATE
    hydra_hidhide_recovery
)
add_test(NAME HidHideSessionRecoveryBackendTests
    COMMAND hidhide_session_recovery_backend_tests
)

add_library(hydra_crash_journal STATIC
    src/crash_journal.cpp
)
target_include_directories(hydra_crash_journal PUBLIC include)
target_link_libraries(hydra_crash_journal PUBLIC hydra_recovery_core)
if(WIN32)
    target_link_libraries(hydra_crash_journal PRIVATE Shell32.lib Ole32.lib)
endif()

add_library(hydra_reset_actions STATIC
    src/reset_actions.cpp
)
target_include_directories(hydra_reset_actions PUBLIC include)
target_link_libraries(hydra_reset_actions PUBLIC
    hydra_crash_journal
    hydra_recovery_core
)

add_library(hydra_gate_c_recovery STATIC
    src/gate_c_recovery.cpp
)
target_include_directories(hydra_gate_c_recovery PUBLIC include)
target_link_libraries(hydra_gate_c_recovery PUBLIC
    hydra_crash_journal
    hydra_recovery_core
)

add_executable(watchdog_protocol_backend_tests
    tests/test_watchdog_protocol.cpp
)
target_link_libraries(watchdog_protocol_backend_tests PRIVATE
    hydra_recovery_core
)
add_test(NAME WatchdogProtocolBackendTests
    COMMAND watchdog_protocol_backend_tests
)

add_executable(crash_journal_backend_tests tests/test_crash_journal.cpp)
target_link_libraries(crash_journal_backend_tests PRIVATE hydra_crash_journal)
add_test(NAME CrashJournalBackendTests COMMAND crash_journal_backend_tests)

add_executable(reset_actions_backend_tests tests/test_reset_actions.cpp)
target_link_libraries(reset_actions_backend_tests PRIVATE hydra_reset_actions)
add_test(NAME ResetActionsBackendTests COMMAND reset_actions_backend_tests)

add_executable(gate_c_recovery_backend_tests tests/test_gate_c_recovery.cpp)
target_link_libraries(gate_c_recovery_backend_tests PRIVATE hydra_gate_c_recovery)
add_test(NAME GateCRecoveryBackendTests COMMAND gate_c_recovery_backend_tests)

if(WIN32)
    add_executable(hydra_watchdog src/watchdog_main.cpp)
    target_link_libraries(hydra_watchdog PRIVATE
        hydra_recovery_core
        hydra_hidhide_recovery
    )
    if(MINGW)
        target_link_options(hydra_watchdog PRIVATE -municode)
    endif()

    add_executable(hydra_reset src/reset_main.cpp)
    target_link_libraries(hydra_reset PRIVATE
        hydra_reset_actions
        hydra_hidhide_recovery
    )

    add_executable(watchdog_process_backend_tests
        tests/test_watchdog_process.cpp
    )
    target_link_libraries(watchdog_process_backend_tests PRIVATE
        hydra_recovery_core
    )
    add_dependencies(watchdog_process_backend_tests hydra_watchdog)
    if(MINGW)
        target_link_options(watchdog_process_backend_tests PRIVATE -municode)
    endif()
    add_test(NAME WatchdogProcessBackendTests
        COMMAND watchdog_process_backend_tests
    )

    add_executable(reset_process_backend_tests
        tests/test_reset_process.cpp
    )
    target_link_libraries(reset_process_backend_tests PRIVATE
        hydra_reset_actions
    )
    add_dependencies(reset_process_backend_tests hydra_reset)
    add_test(NAME ResetProcessBackendTests
        COMMAND reset_process_backend_tests
    )
endif()

# Activate preserved trust/metrics/compatibility foundations required by
# production installer, update, and support flows.
add_library(hydra_artifact_trust STATIC
    src/artifact_trust.cpp
)
target_include_directories(hydra_artifact_trust PUBLIC include)

add_library(hydra_session_metrics STATIC
    src/session_metrics.cpp
)
target_include_directories(hydra_session_metrics PUBLIC include)
target_link_libraries(hydra_session_metrics PUBLIC
    hydra_input_isolation
)

add_library(hydra_compatibility_result STATIC
    src/compatibility_result.cpp
    src/compatibility_aggregation.cpp
)
target_include_directories(hydra_compatibility_result PUBLIC include)
target_link_libraries(hydra_compatibility_result PUBLIC
    hydra_session_metrics
)

add_library(hydra_support_bundle STATIC
    src/support_bundle.cpp
)
target_include_directories(hydra_support_bundle PUBLIC include)
target_link_libraries(hydra_support_bundle PUBLIC
    hydra_compatibility_result
    hydra_crash_journal
    hydra_session_metrics
)

add_library(hydra_privilege_broker STATIC
    src/privilege_broker.cpp
)
target_include_directories(hydra_privilege_broker PUBLIC include)
target_link_libraries(hydra_privilege_broker PUBLIC
    hydra_artifact_trust
)

add_library(hydra_installer_transaction STATIC
    src/installer_transaction.cpp
)
target_include_directories(hydra_installer_transaction PUBLIC include)
target_link_libraries(hydra_installer_transaction PUBLIC
    hydra_artifact_trust
    hydra_recovery_core
)

add_library(hydra_update_transaction STATIC
    src/update_transaction.cpp
)
target_include_directories(hydra_update_transaction PUBLIC include)
target_link_libraries(hydra_update_transaction PUBLIC
    hydra_installer_transaction
)

add_executable(artifact_trust_backend_tests tests/test_artifact_trust.cpp)
target_link_libraries(artifact_trust_backend_tests PRIVATE hydra_artifact_trust)
add_test(NAME ArtifactTrustBackendTests COMMAND artifact_trust_backend_tests)

add_executable(session_metrics_backend_tests tests/test_session_metrics.cpp)
target_link_libraries(session_metrics_backend_tests PRIVATE hydra_session_metrics)
add_test(NAME SessionMetricsBackendTests COMMAND session_metrics_backend_tests)

add_executable(compatibility_result_backend_tests tests/test_compatibility_result.cpp)
target_link_libraries(compatibility_result_backend_tests PRIVATE hydra_compatibility_result)
add_test(NAME CompatibilityResultBackendTests COMMAND compatibility_result_backend_tests)

add_executable(support_bundle_backend_tests tests/test_support_bundle.cpp)
target_link_libraries(support_bundle_backend_tests PRIVATE hydra_support_bundle)
add_test(NAME SupportBundleBackendTests COMMAND support_bundle_backend_tests)

add_executable(privilege_broker_backend_tests tests/test_privilege_broker.cpp)
target_link_libraries(privilege_broker_backend_tests PRIVATE hydra_privilege_broker)
add_test(NAME PrivilegeBrokerBackendTests COMMAND privilege_broker_backend_tests)

add_executable(installer_transaction_backend_tests tests/test_installer_transaction.cpp)
target_link_libraries(installer_transaction_backend_tests PRIVATE hydra_installer_transaction)
add_test(NAME InstallerTransactionBackendTests COMMAND installer_transaction_backend_tests)

add_executable(update_transaction_backend_tests tests/test_update_transaction.cpp)
target_link_libraries(update_transaction_backend_tests PRIVATE hydra_update_transaction)
add_test(NAME UpdateTransactionBackendTests COMMAND update_transaction_backend_tests)

# Signed-package bootstrap UX delegates mutation to the existing transactional
# installer script instead of creating a second installer authority.
add_library(hydra_installer_bootstrap STATIC
    src/installer_bootstrap.cpp
)
target_include_directories(hydra_installer_bootstrap PUBLIC include)
if(WIN32)
    target_link_libraries(hydra_installer_bootstrap PRIVATE
        Wintrust.lib Crypt32.lib Shell32.lib Advapi32.lib)
endif()

add_executable(installer_bootstrap_tests tests/test_installer_bootstrap.cpp)
target_link_libraries(installer_bootstrap_tests PRIVATE hydra_installer_bootstrap)
add_test(NAME InstallerBootstrapTests COMMAND installer_bootstrap_tests)

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(HydraSeatSetup WIN32 src/installer_bootstrap_main.cpp)
    target_link_libraries(HydraSeatSetup PRIVATE
        hydra_installer_bootstrap User32.lib)
    if(MINGW)
        target_link_options(HydraSeatSetup PRIVATE -municode)
    endif()
endif()

add_library(hydra_two_player_setup_model STATIC
    src/two_player_setup_editor.cpp
)
target_include_directories(hydra_two_player_setup_model PUBLIC include)
target_link_libraries(hydra_two_player_setup_model PUBLIC
    hydra_profile_provider_foundation
)

add_library(hydra_setup_package STATIC
    src/setup_package.cpp
)
target_include_directories(hydra_setup_package PUBLIC include)
target_link_libraries(hydra_setup_package PUBLIC
    hydra_two_player_setup_model
)

add_executable(two_player_setup_model_tests
    tests/test_two_player_setup_editor.cpp
)
target_link_libraries(two_player_setup_model_tests PRIVATE
    hydra_two_player_setup_model
)
add_test(NAME TwoPlayerSetupModelTests COMMAND two_player_setup_model_tests)

add_executable(setup_package_backend_tests
    tests/test_setup_package.cpp
)
target_link_libraries(setup_package_backend_tests PRIVATE
    hydra_setup_package
)
add_test(NAME SetupPackageBackendTests COMMAND setup_package_backend_tests)

add_executable(hydra_hardware_detect
    src/hardware_detect_cli.cpp
    src/hardware_detector.cpp
)
target_include_directories(hydra_hardware_detect PRIVATE include)
target_link_libraries(hydra_hardware_detect PRIVATE
    hydra_host_authority
)
if(WIN32)
    target_link_libraries(hydra_hardware_detect PRIVATE ${WIN32_LIBS})
endif()

add_executable(hydra_isolation_plan src/isolation_plan_cli.cpp)
target_link_libraries(hydra_isolation_plan PRIVATE hydra_input_isolation)

add_executable(hydra_hidhide_probe_cli_diag src/hidhide_probe_cli.cpp)
target_link_libraries(hydra_hidhide_probe_cli_diag PRIVATE hydra_input_isolation)

add_executable(hydra_input_metrics_cli_diag src/input_metrics_cli.cpp)
target_link_libraries(hydra_input_metrics_cli_diag PRIVATE hydra_input_isolation)

add_executable(hydra_display_diag src/display_diag_main.cpp)
target_link_libraries(hydra_display_diag PRIVATE hydra_process_display)

add_executable(hydra_directinput_probe src/directinput_probe.cpp)
target_link_libraries(hydra_directinput_probe PRIVATE hydra_input_isolation)
if(WIN32)
    target_link_libraries(hydra_directinput_probe PRIVATE Dinput8.lib Dxguid.lib)
endif()

add_executable(hydra_gate_c_raw_input_probe_diag
    src/gate_c_raw_input_probe.cpp
)
target_link_libraries(hydra_gate_c_raw_input_probe_diag PRIVATE
    hydra_input_isolation
)
if(WIN32)
    target_link_libraries(hydra_gate_c_raw_input_probe_diag PRIVATE User32.lib)
endif()

add_executable(hydra_controller_diag src/controller_diag_main.cpp)
target_link_libraries(hydra_controller_diag PRIVATE hydra_host_authority)

if(WIN32)
    add_executable(hydra_handle_diagnostic src/handle_diagnostic.cpp)
    target_link_libraries(hydra_handle_diagnostic PRIVATE User32.lib)
endif()

add_test(NAME HidHideProbeCliSelfTest
    COMMAND hydra_hidhide_probe_cli_diag --self-test)
add_test(NAME InputMetricsCliSelfTest
    COMMAND hydra_input_metrics_cli_diag --self-test)
add_test(NAME DirectInputProbeSeatASelfTest
    COMMAND hydra_directinput_probe --controlled-self-test seat-a)
add_test(NAME DirectInputProbeSeatBSelfTest
    COMMAND hydra_directinput_probe --controlled-self-test seat-b)
add_test(NAME GateCRawInputProbeSelfTest
    COMMAND hydra_gate_c_raw_input_probe_diag --self-test)
add_test(NAME ControllerDiagHelp
    COMMAND hydra_controller_diag --help)
add_test(NAME IsolationPlanList
    COMMAND hydra_isolation_plan --list)

# Activate the preserved provider-plan and compatibility/community pipeline.
# These are data/plan authorities only; no UI state is allowed to become runtime
# authority through these targets.
add_library(hydra_provider_launch_plan STATIC
    src/provider_launch_plan.cpp
)
target_include_directories(hydra_provider_launch_plan PUBLIC include)
target_link_libraries(hydra_provider_launch_plan PUBLIC
    hydra_profile_provider_foundation
    hydra_two_seat_launch
)

add_library(hydra_plan_preflight STATIC
    src/plan_preflight.cpp
)
target_include_directories(hydra_plan_preflight PUBLIC include)
target_link_libraries(hydra_plan_preflight PUBLIC
    hydra_provider_launch_plan
)

add_library(hydra_profile_migration STATIC
    src/profile_migration.cpp
)
target_include_directories(hydra_profile_migration PUBLIC include)
target_link_libraries(hydra_profile_migration PUBLIC
    hydra_profile_provider_foundation
)

add_library(hydra_profile_cli STATIC
    src/profile_cli.cpp
)
target_include_directories(hydra_profile_cli PUBLIC include)
target_link_libraries(hydra_profile_cli PUBLIC
    hydra_profile_provider_foundation
    hydra_provider_launch_plan
)

add_executable(hydraseat_profilectl src/profilectl_main.cpp)
target_link_libraries(hydraseat_profilectl PRIVATE hydra_profile_cli)

add_library(hydra_community_submission STATIC
    src/community_submission.cpp
    src/compatibility_share_model.cpp
    src/compatibility_local_store.cpp
)
target_include_directories(hydra_community_submission PUBLIC include)
target_link_libraries(hydra_community_submission PUBLIC
    hydra_compatibility_result
)

add_library(hydra_community_package STATIC
    src/community_package.cpp
    src/community_setup.cpp
    src/compatibility_catalog.cpp
)
target_include_directories(hydra_community_package PUBLIC include)
target_link_libraries(hydra_community_package PUBLIC
    hydra_artifact_trust
    hydra_compatibility_result
    hydra_setup_package
    hydra_provider_launch_plan
)

add_library(hydra_catalog_cache STATIC
    src/catalog_cache.cpp
)
target_include_directories(hydra_catalog_cache PUBLIC include)
target_link_libraries(hydra_catalog_cache PUBLIC
    hydra_artifact_trust
)

add_library(hydra_game_runtime_requirement_resolver STATIC
    src/game_runtime_requirement_resolver.cpp
)
target_include_directories(hydra_game_runtime_requirement_resolver PUBLIC include)
target_link_libraries(hydra_game_runtime_requirement_resolver PUBLIC
    hydra_compatibility_result
    hydra_profile_provider_foundation
    hydra_provider_launch_plan
    hydra_community_submission
)
if(WIN32)
    target_link_libraries(hydra_game_runtime_requirement_resolver PRIVATE Bcrypt.lib)
endif()

# Local evidence collection, persistence, and publication remain separate
# authorities. Controlled/synthetic evidence is never promoted to Physical.
add_library(hydra_local_compatibility_evidence STATIC
    src/local_compatibility_evidence.cpp
)
target_include_directories(hydra_local_compatibility_evidence PUBLIC include)
target_link_libraries(hydra_local_compatibility_evidence PUBLIC
    hydra_community_submission
)

add_library(hydra_runtime_requirement_authority STATIC
    src/runtime_requirement_authority.cpp
)
target_include_directories(hydra_runtime_requirement_authority PUBLIC include)
target_link_libraries(hydra_runtime_requirement_authority PUBLIC
    hydra_game_runtime_requirement_resolver
)

add_library(hydra_local_compatibility_runner STATIC
    src/local_compatibility_runner.cpp
)
target_include_directories(hydra_local_compatibility_runner PUBLIC include)
target_link_libraries(hydra_local_compatibility_runner PUBLIC
    hydra_process_display
    hydra_session_metrics
)

add_executable(local_compatibility_evidence_tests
    tests/test_local_compatibility_evidence.cpp)
target_link_libraries(local_compatibility_evidence_tests PRIVATE
    hydra_local_compatibility_evidence)
add_test(NAME LocalCompatibilityEvidenceTests
    COMMAND local_compatibility_evidence_tests)

add_executable(runtime_requirement_authority_tests
    tests/test_runtime_requirement_authority.cpp)
target_link_libraries(runtime_requirement_authority_tests PRIVATE
    hydra_runtime_requirement_authority)
add_test(NAME RuntimeRequirementAuthorityTests
    COMMAND runtime_requirement_authority_tests)

add_executable(local_compatibility_runner_tests
    tests/test_local_compatibility_runner.cpp)
target_link_libraries(local_compatibility_runner_tests PRIVATE
    hydra_local_compatibility_runner)
add_test(NAME LocalCompatibilityRunnerTests
    COMMAND local_compatibility_runner_tests)

add_executable(hydraseat_community_validate src/community_validate_main.cpp)
target_link_libraries(hydraseat_community_validate PRIVATE
    hydra_compatibility_result
    hydra_setup_package
)

foreach(test_name IN ITEMS
    provider_launch_plan
    plan_preflight
    profile_migration
    profile_cli
    compatibility_aggregation
    community_package
    community_setup
    compatibility_catalog
    catalog_cache
    community_submission
    compatibility_share_model
    compatibility_local_store
    game_runtime_requirement_resolver
)
    add_executable(${test_name}_pipeline_tests tests/test_${test_name}.cpp)
endforeach()

target_link_libraries(provider_launch_plan_pipeline_tests PRIVATE hydra_provider_launch_plan)
target_link_libraries(plan_preflight_pipeline_tests PRIVATE hydra_plan_preflight)
target_link_libraries(profile_migration_pipeline_tests PRIVATE hydra_profile_migration)
target_link_libraries(profile_cli_pipeline_tests PRIVATE hydra_profile_cli)
target_link_libraries(compatibility_aggregation_pipeline_tests PRIVATE hydra_compatibility_result)
target_link_libraries(community_package_pipeline_tests PRIVATE hydra_community_package)
target_link_libraries(community_setup_pipeline_tests PRIVATE hydra_community_package)
target_link_libraries(compatibility_catalog_pipeline_tests PRIVATE hydra_community_package)
target_link_libraries(catalog_cache_pipeline_tests PRIVATE hydra_catalog_cache)
target_link_libraries(community_submission_pipeline_tests PRIVATE hydra_community_submission)
target_link_libraries(compatibility_share_model_pipeline_tests PRIVATE hydra_community_submission)
target_link_libraries(compatibility_local_store_pipeline_tests PRIVATE hydra_community_submission)
target_link_libraries(game_runtime_requirement_resolver_pipeline_tests PRIVATE
    hydra_game_runtime_requirement_resolver)

add_test(NAME ProviderLaunchPlanPipelineTests COMMAND provider_launch_plan_pipeline_tests)
add_test(NAME PlanPreflightPipelineTests COMMAND plan_preflight_pipeline_tests)
add_test(NAME ProfileMigrationPipelineTests COMMAND profile_migration_pipeline_tests)
add_test(NAME ProfileCliPipelineTests COMMAND profile_cli_pipeline_tests)
add_test(NAME CompatibilityAggregationPipelineTests COMMAND compatibility_aggregation_pipeline_tests)
add_test(NAME CommunityPackagePipelineTests COMMAND community_package_pipeline_tests)
add_test(NAME CommunitySetupPipelineTests COMMAND community_setup_pipeline_tests)
add_test(NAME CompatibilityCatalogPipelineTests COMMAND compatibility_catalog_pipeline_tests)
add_test(NAME CatalogCachePipelineTests COMMAND catalog_cache_pipeline_tests)
add_test(NAME CommunitySubmissionPipelineTests COMMAND community_submission_pipeline_tests)
add_test(NAME CompatibilityShareModelPipelineTests COMMAND compatibility_share_model_pipeline_tests)
add_test(NAME CompatibilityLocalStorePipelineTests COMMAND compatibility_local_store_pipeline_tests)
add_test(NAME GameRuntimeRequirementResolverPipelineTests
    COMMAND game_runtime_requirement_resolver_pipeline_tests)

add_library(hydra_acceptance_campaign STATIC
    src/acceptance_campaign.cpp
)
target_include_directories(hydra_acceptance_campaign PUBLIC include)

add_executable(hydraseat_acceptance_campaignctl
    src/acceptance_campaignctl_main.cpp
)
target_link_libraries(hydraseat_acceptance_campaignctl PRIVATE
    hydra_acceptance_campaign
)

add_executable(acceptance_campaign_backend_tests
    tests/test_acceptance_campaign.cpp
)
target_link_libraries(acceptance_campaign_backend_tests PRIVATE
    hydra_acceptance_campaign
)
add_test(NAME AcceptanceCampaignBackendTests
    COMMAND acceptance_campaign_backend_tests
)

add_library(hydra_acceptance_probe STATIC
    src/acceptance_probe.cpp
)
target_include_directories(hydra_acceptance_probe PUBLIC include)
target_link_libraries(hydra_acceptance_probe PUBLIC
    hydra_process_display
    hydra_host_authority
)

if(WIN32)
    add_library(hydra_audio_inventory_readonly STATIC
        src/audio_endpoint_inventory.cpp
    )
    target_include_directories(hydra_audio_inventory_readonly PUBLIC include)
    target_link_libraries(hydra_audio_inventory_readonly PUBLIC ${WIN32_LIBS})

    target_link_libraries(hydra_acceptance_probe PRIVATE
        hydra_audio_inventory_readonly
        Bcrypt.lib
        Wintrust.lib
        Psapi.lib
    )
endif()

add_executable(hydraseat_acceptance_probe
    src/acceptance_probe_main.cpp
)
target_link_libraries(hydraseat_acceptance_probe PRIVATE hydra_acceptance_probe)

add_executable(acceptance_probe_process_backend_tests
    tests/test_acceptance_probe_process.cpp
)
target_link_libraries(acceptance_probe_process_backend_tests PRIVATE
    hydra_acceptance_probe
)

if(WIN32)
    add_dependencies(acceptance_probe_process_backend_tests
        hydra_process_tree_child
    )
    add_test(NAME AcceptanceProbeProcessBackendTests
        COMMAND acceptance_probe_process_backend_tests
            $<TARGET_FILE:hydra_process_tree_child>
    )
else()
    add_test(NAME AcceptanceProbeProcessBackendTests
        COMMAND acceptance_probe_process_backend_tests
    )
endif()

# Release/QA validators are data-only and fail closed. Self-test mode performs
# no signing, install, packaging, network, or machine mutation.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME PreMergeGateSelfTest
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_premerge_gate.py"
                --self-test)
    add_test(NAME ReleaseArtifactManifestSelfTest
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_release_artifact_manifest.py"
                --self-test)
    add_test(NAME ReleaseInstallerContractSelfTest
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_release_installer_contract.py"
                --self-test)
    add_test(NAME ReleaseSigningManifestSelfTest
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_release_signing_manifest.py"
                --self-test)
    add_test(NAME ProductionReachabilitySelfTest
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_production_reachability.py"
                --self-test)
    add_test(NAME PublicAbiContractSelfTest
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_public_abi_contracts.py"
                --self-test)
    add_test(NAME RepositoryPublicationHygieneSelfTest
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_repository_publication_hygiene.py"
                --self-test)
    add_test(NAME ReleaseScopeSelfTest
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_release_scope.py"
                --self-test)
    add_test(NAME V1AcceptanceCampaignValidatorSelfTest
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_v1_acceptance_campaign.py"
                --self-test)
    add_test(NAME Phase3TraceParserSelfTest
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/summarize_phase3_trace.py"
                --self-test)
endif()

if(WIN32)
    find_program(HYDRA_POWERSHELL_EXECUTABLE
        NAMES powershell.exe powershell pwsh)
    if(HYDRA_POWERSHELL_EXECUTABLE)
        add_test(NAME ReleasePowerShellSyntaxSelfTest
            COMMAND "${HYDRA_POWERSHELL_EXECUTABLE}"
                    -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_release_powershell_syntax.ps1"
                    -RepositoryRoot "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME Phase3HardwareAcceptanceRunnerSelfTest
            COMMAND "${HYDRA_POWERSHELL_EXECUTABLE}"
                    -NoProfile -ExecutionPolicy Bypass
                    -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/run_phase3_hardware_acceptance.ps1"
                    -SelfTest)
    endif()
endif()
