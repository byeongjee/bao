# BB Energy Analyzer - Standalone tool for DWARF-based BB energy analysis
# No Gurobi dependency, links LLVM libraries for DWARF parsing
# Uses external msp430-elf-objdump for disassembly

add_executable(bb-energy-analyzer
    BBEnergyAnalyzer.cpp
    DWARFParser.cpp
    DWARFLineResolver.cpp
    MSP430Disassembler.cpp
    EnergyModel.cpp
    ${CMAKE_SOURCE_DIR}/src/common/Logger.cpp
)

set_target_properties(bb-energy-analyzer PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Get LLVM components needed for this tool
# Note: We use external msp430-elf-objdump for disassembly since LLVM may not
# have MSP430 target. Only need DWARF parsing; object/support come in as
# transitive dependencies (listing them explicitly duplicates them on the
# link line, which the macOS linker warns about).
llvm_map_components_to_libnames(LLVM_LIBS
    debuginfodwarf
)

target_include_directories(bb-energy-analyzer PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(bb-energy-analyzer PRIVATE
    ${LLVM_LIBS}
    nlohmann_json::nlohmann_json
    plog::plog
)

# Disable RTTI to match LLVM
target_compile_options(bb-energy-analyzer PRIVATE
    -fno-rtti
)
