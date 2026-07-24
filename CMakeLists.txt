cmake_minimum_required(VERSION 3.20)
project(HydraSeat VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include directories
include_directories(include)

# Windows system libraries for Win32 Raw Input, SetupAPI, and DXGI
if(WIN32)
    add_compile_definitions(_UNICODE UNICODE WIN32_LEAN_AND_MEAN)
    set(WIN32_LIBS User32.lib Cfgmgr32.lib Dxgi.lib Setupapi.lib Hid.lib Xinput.lib)
endif()

# Find Qt6 components if available
find_package(Qt6 COMPONENTS Core Widgets QUIET)

# Source files
set(HYDRA_SOURCES
    src/hardware_detector.cpp
    src/main.cpp
)

set(HYDRA_HEADERS
    include/hydra/hardware_detector.hpp
)

# Executable target
add_executable(HydraSeat ${HYDRA_SOURCES} ${HYDRA_HEADERS})

if(WIN32)
    target_link_libraries(HydraSeat PRIVATE ${WIN32_LIBS})
endif()

if(Qt6_FOUND)
    target_link_libraries(HydraSeat PRIVATE Qt6::Core Qt6::Widgets)
    target_compile_definitions(HydraSeat PRIVATE HYDRA_HAS_QT=1)
endif()
