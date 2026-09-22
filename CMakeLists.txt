cmake_minimum_required(VERSION 3.20)
project(HydraSeat VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include directories
include_directories(include .)

# Enable UTF-8 encoding for MSVC string literals
if(MSVC)
    add_compile_options(/utf-8)
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
        ui/workspace_widget.cpp
        ui/main_window.cpp
    )
    set(UI_HEADERS
        ui/workspace_widget.hpp
        ui/main_window.hpp
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
    src/game_launcher.cpp
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
add_executable(reset_input src/reset_input.cpp)
if(WIN32)
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

    add_executable(xinput_adapter_exports
        tests/test_xinput_adapter_exports.cpp
    )

    add_executable(xinput_abi_probe_game
        tools/xinput_abi_probe_game.cpp
        ${XINPUT_ABI_PROBE_SUPPORT_SOURCES}
        ${XINPUT_ABI_PROBE_SUPPORT_HEADERS}
    )

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

    add_executable(game_launcher_process_ownership
        tests/test_game_launcher_process_ownership.cpp
        src/game_launcher.cpp
        src/runtime_authority.cpp
        src/controller_identity.cpp
        src/controller_inventory.cpp
        src/controller_io.cpp
        src/controller_virtual_xinput.cpp
    )
    target_link_libraries(game_launcher_process_ownership PRIVATE Xinput.lib Setupapi.lib Hid.lib)
    add_dependencies(game_launcher_process_ownership hydra_process_owner_child)
    add_test(
        NAME GameLauncherProcessOwnership
        COMMAND game_launcher_process_ownership $<TARGET_FILE:hydra_process_owner_child>
    )

    add_executable(game_launcher_xinput_integration
        tests/test_game_launcher_xinput_integration.cpp
        src/game_launcher.cpp
        src/runtime_authority.cpp
        src/controller_identity.cpp
        src/controller_inventory.cpp
        src/controller_io.cpp
        src/controller_virtual_xinput.cpp
    )
    target_link_libraries(game_launcher_xinput_integration PRIVATE Xinput.lib Setupapi.lib Hid.lib)
    add_dependencies(game_launcher_xinput_integration hydra_process_owner_child)
    add_test(
        NAME GameLauncherXInputIntegration
        COMMAND game_launcher_xinput_integration $<TARGET_FILE:hydra_process_owner_child>
    )
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
    add_executable(hydra_window_test_app WIN32 tests/window_test_app.cpp)
    target_link_libraries(hydra_window_test_app PRIVATE User32.lib)
    add_dependencies(process_group_tests hydra_process_tree_child)
    add_dependencies(window_policy_tests hydra_window_test_app)
    add_dependencies(window_tracker_tests hydra_window_test_app)
endif()
