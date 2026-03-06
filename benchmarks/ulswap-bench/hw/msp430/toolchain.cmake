# CMake toolchain file for MSP430
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR msp430)

# Validate required environment variable
if(NOT DEFINED ENV{MSP430_GCC_ROOT})
    message(FATAL_ERROR
        "MSP430_GCC_ROOT environment variable is not set.\n"
        "Please set it to your MSP430 GCC toolchain installation path.\n"
        "Example: export MSP430_GCC_ROOT=\$HOME/ti/msp430-gcc\n"
        "See hw/msp430/README.md for setup instructions.")
endif()

set(MSP430_GCC_ROOT $ENV{MSP430_GCC_ROOT})

# Validate the toolchain path exists
if(NOT EXISTS "${MSP430_GCC_ROOT}/bin/msp430-elf-gcc")
    message(FATAL_ERROR
        "MSP430 GCC compiler not found at ${MSP430_GCC_ROOT}/bin/msp430-elf-gcc\n"
        "Please verify MSP430_GCC_ROOT points to a valid MSP430 GCC installation.")
endif()

# Auto-detect picolibc.specs by globbing for GCC version directory
file(GLOB PICOLIBC_SPECS_CANDIDATES "${MSP430_GCC_ROOT}/lib/gcc/msp430-elf/*/picolibc.specs")
list(LENGTH PICOLIBC_SPECS_CANDIDATES SPECS_COUNT)

if(SPECS_COUNT EQUAL 0)
    message(FATAL_ERROR
        "picolibc.specs not found in ${MSP430_GCC_ROOT}/lib/gcc/msp430-elf/*/\n"
        "Please install picolibc for MSP430. See hw/msp430/README.md for instructions.")
elseif(SPECS_COUNT GREATER 1)
    message(WARNING "Multiple picolibc.specs found, using first one: ${PICOLIBC_SPECS_CANDIDATES}")
endif()

list(GET PICOLIBC_SPECS_CANDIDATES 0 PICOLIBC_SPECS)
message(STATUS "Using picolibc.specs: ${PICOLIBC_SPECS}")

set(CMAKE_C_COMPILER ${MSP430_GCC_ROOT}/bin/msp430-elf-gcc)
set(CMAKE_ASM_COMPILER ${MSP430_GCC_ROOT}/bin/msp430-elf-gcc)
set(CMAKE_OBJDUMP ${MSP430_GCC_ROOT}/bin/msp430-elf-objdump)

# Export for use in config.cmake
set(MSP430_GCC_ROOT ${MSP430_GCC_ROOT} CACHE INTERNAL "MSP430 GCC root directory")
set(PICOLIBC_SPECS ${PICOLIBC_SPECS} CACHE INTERNAL "Path to picolibc.specs")

# Skip compiler tests since this is a cross-compiler
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_ASM_COMPILER_WORKS 1)

# Don't look for programs in host paths
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
