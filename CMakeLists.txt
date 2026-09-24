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
endif()

# Test Suite Executable Target
enable_testing()
add_executable(hydra_tests tests/test_hydra.cpp tests/test_controller_identity.cpp tests/test_controller_pairing.cpp tests/test_controller_virtual_xinput.cpp ${HYDRA_SOURCES} ${HYDRA_HEADERS})
if(WIN32)
    target_link_libraries(hydra_tests PRIVATE ${WIN32_LIBS})
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
endif()
