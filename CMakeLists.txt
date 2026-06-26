# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_FLASH_SIZE_BYTES 8388608)  # W25Q64JVSSIQ = 8MB

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(HP-WPC-Audio-FW C CXX ASM)

# Force a debug build to preserve source mapping
set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(HP-WPC-Audio-FW 
    src/HP-WPC-Audio-FW.c
    src/hw_config.c
    src/AudioChain.c
    src/DataIO.c
    src/SharedState.c
    src/Core1Audio.c
    src/ym2151.c
    src/WPCS6809.c
    src/WPCSBoard.c
    src/SDCardFunctions.c
#    src/GetawaySoundROM.c
#    src/FishTalesSoundROM.c    
#    src/WhiteWaterSoundROM18.c    
#    src/WhiteWaterSoundROM15.c
#    src/WhiteWaterSoundROM18.c
)
pico_set_linker_script(HP-WPC-Audio-FW ${CMAKE_CURRENT_SOURCE_DIR}/custom_memmap.ld)
pico_set_program_name(HP-WPC-Audio-FW "HP-WPC-Audio-FW")
pico_set_program_version(HP-WPC-Audio-FW "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(HP-WPC-Audio-FW 0)
pico_enable_stdio_usb(HP-WPC-Audio-FW 0)
pico_generate_pio_header(HP-WPC-Audio-FW ${CMAKE_CURRENT_LIST_DIR}/pio/ws2812.pio)
pico_generate_pio_header(HP-WPC-Audio-FW ${CMAKE_CURRENT_LIST_DIR}/pio/i2s.pio)
pico_generate_pio_header(HP-WPC-Audio-FW ${CMAKE_CURRENT_LIST_DIR}/pio/DataIO.pio)

add_subdirectory(fatfs/src)

# Add the standard library to the build
target_link_libraries(HP-WPC-Audio-FW 
    pico_stdlib
    hardware_pio 
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
    pico_multicore
    hardware_flash
    hardware_sync
)

# Add the standard include files to the build
target_include_directories(HP-WPC-Audio-FW PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
)


# Force aggressive optimization for the audio DSP math
target_compile_options(HP-WPC-Audio-FW PRIVATE -O3)

pico_add_extra_outputs(HP-WPC-Audio-FW)

