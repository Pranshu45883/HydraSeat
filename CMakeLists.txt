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
    set(WIN32_LIBS User32.lib Cfgmgr32.lib Dxgi.lib Setupapi.lib Hid.lib Xinput.lib Comctl32.lib)
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
    src/virtual_xinput_protocol.cpp
    src/virtual_xinput_service.cpp
    src/virtual_xinput_pipe.cpp
    src/xinput_adapter_session.cpp
    src/game_launcher.cpp
    src/gui_win32.cpp
    ${UI_SOURCES}
)

set(HYDRA_HEADERS
    include/hydra/hardware_detector.hpp
    include/hydra/input_router.hpp
    include/hydra/display_manager.hpp
    include/hydra/workspace_manager.hpp
    include/hydra/runtime_authority.hpp
    include/hydra/controller_identity.hpp
    include/hydra/controller_inventory.hpp
    include/hydra/controller_io.hpp
    include/hydra/controller_pairing.hpp
    include/hydra/controller_virtual_xinput.hpp
    include/hydra/virtual_xinput_protocol.hpp
    include/hydra/virtual_xinput_service.hpp
    include/hydra/virtual_xinput_pipe.hpp
    include/hydra/xinput_adapter_session.hpp
    include/hydra/game_launcher.hpp
    include/hydra/gui_win32.hpp
    ${UI_HEADERS}
)

set(XINPUT_PROBE_SUPPORT_SOURCES
    src/xinput_probe.cpp
)
set(XINPUT_PROBE_SUPPORT_HEADERS
    include/hydra/xinput_probe.hpp
)

# Core Executable target (Windows GUI subsystem)
add_executable(HydraSeat WIN32 src/main.cpp ${HYDRA_SOURCES} ${HYDRA_HEADERS})

if(WIN32)
    target_link_libraries(HydraSeat PRIVATE ${WIN32_LIBS})
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
    ${HYDRA_SOURCES}
    ${HYDRA_HEADERS}
    ${XINPUT_PROBE_SUPPORT_SOURCES}
    ${XINPUT_PROBE_SUPPORT_HEADERS}
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
endif()
if(Qt6_FOUND)
    target_link_libraries(hydra_tests PRIVATE Qt6::Core Qt6::Widgets)
endif()
add_test(NAME EngineUnitTests COMMAND hydra_tests)
if(WIN32)
    add_test(
        NAME XInputProcessIsolation
        COMMAND xinput_process_isolation $<TARGET_FILE:xinput_probe_game>
    )
endif()
