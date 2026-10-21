cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME code)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
option(HMI_CONNECTION_TEST "仅运行屏幕连接测试，泵阀保持关闭" OFF)
option(HMI_SERIAL_BRIDGE "使用 STM32 转发屏幕下载串口，泵阀保持关闭" OFF)
if(HMI_CONNECTION_TEST AND HMI_SERIAL_BRIDGE)
    message(FATAL_ERROR "屏幕连接测试与串口转发不能同时启用")
endif()
if(HMI_SERIAL_BRIDGE)
    set(HMI_SOURCE Core/Src/hmi_serial_bridge.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HMI_SERIAL_BRIDGE)
elseif(HMI_CONNECTION_TEST)
    set(HMI_SOURCE Core/Src/hmi_connection_test.c)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HMI_CONNECTION_TEST)
else()
    set(HMI_SOURCE Core/Src/hmi_screen.c)
endif()
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Src/liquid_measurement.c
    Core/Src/esp_network.c
    Core/Src/network_config.c
    ${HMI_SOURCE}
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
