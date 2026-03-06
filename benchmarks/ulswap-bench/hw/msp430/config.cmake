function (set_msp430_config)
    message(STATUS "Setting MSP430 configuration")

    # MSP430_GCC_ROOT and PICOLIBC_SPECS are set by toolchain.cmake
    # Validate they are available (should be set via toolchain file)
    if(NOT DEFINED MSP430_GCC_ROOT)
        # Fallback: try environment variable directly
        if(DEFINED ENV{MSP430_GCC_ROOT})
            set(MSP430_GCC_ROOT $ENV{MSP430_GCC_ROOT})
        else()
            message(FATAL_ERROR
                "MSP430_GCC_ROOT is not set. Use the toolchain file:\n"
                "  cmake -DCMAKE_TOOLCHAIN_FILE=hw/msp430/toolchain.cmake ...")
        endif()
    endif()

    # Auto-detect picolibc.specs if not already set by toolchain
    if(NOT DEFINED PICOLIBC_SPECS)
        file(GLOB PICOLIBC_SPECS_CANDIDATES "${MSP430_GCC_ROOT}/lib/gcc/msp430-elf/*/picolibc.specs")
        list(LENGTH PICOLIBC_SPECS_CANDIDATES SPECS_COUNT)
        if(SPECS_COUNT EQUAL 0)
            message(FATAL_ERROR
                "picolibc.specs not found in ${MSP430_GCC_ROOT}/lib/gcc/msp430-elf/*/\n"
                "Please install picolibc for MSP430. See hw/msp430/README.md for instructions.")
        endif()
        list(GET PICOLIBC_SPECS_CANDIDATES 0 PICOLIBC_SPECS)
    endif()

    set(CC_PATH ${MSP430_GCC_ROOT}/bin)

    set(CMAKE_C_COMPILER ${CC_PATH}/msp430-elf-gcc PARENT_SCOPE)
    set(CMAKE_ASM_COMPILER ${CC_PATH}/msp430-elf-gcc PARENT_SCOPE)

    # -DNDEBUG disables assert() which otherwise requires stderr/getpid/kill not available on bare metal
    set(GENERAL_FLAGS "-Wall;-fno-builtin;-ffreestanding;-fno-optimize-sibling-calls;-fno-builtin-fma;-ffp-contract=off;-ffunction-sections;-DNDEBUG")
    set(MSP430_FLAGS "-mlarge;-mdata-region=upper;-mmcu=msp430fr5994;-mhwmult=none;-specs=${PICOLIBC_SPECS};-T${ARCH_DIR}/memmap.ld;-DCUSTOM_ARCH_STARTUP;-Wl,--gc-sections;-L${MSP430_GCC_ROOT}/include")

    set(ARCH_OBJDUMP "${CC_PATH}/msp430-elf-objdump" PARENT_SCOPE)

    set(ARCH_INC_DIRS "${MSP430_GCC_ROOT}/include" PARENT_SCOPE)

    set(ARCH_FLAGS "${GENERAL_FLAGS};${MSP430_FLAGS}" PARENT_SCOPE)
    set(ARCH_LINK_FLAGS "${GENERAL_FLAGS};${MSP430_FLAGS}" PARENT_SCOPE)
    set(ARCH_SOURCES "${ARCH_DIR}/supportFuncs.c;${ARCH_DIR}/vectors.S" PARENT_SCOPE)
endfunction()
