# RockClimb Machine-Level Pass - Post-RegAlloc MachineFunctionPass
# Loads via llc -load, runs on MIR via -run-pass=rockclimb-machine
# No Gurobi dependency.

# Reuse LLVM/json from parent CMakeLists.txt

add_library(RockClimbMachinePass MODULE
    RockClimbMachinePass.cpp
    AssignMIRBBDebugInfoPass.cpp
    MachineEnergyEstimator.cpp
    RockClimbMachineOptimizer.cpp
    MachineDistributedCheckpointing.cpp
    MachineLivenessAnalysis.cpp
    RockClimbMachineInstrumenter.cpp
    # Reuse shared sources
    ${CMAKE_SOURCE_DIR}/bb-energy-analyzer/EnergyModel.cpp
    ${CMAKE_SOURCE_DIR}/src/common/PassStatistics.cpp
    ${CMAKE_SOURCE_DIR}/src/common/Logger.cpp
)

set_target_properties(RockClimbMachinePass PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    PREFIX ""  # Don't prefix with 'lib'
)

target_compile_options(RockClimbMachinePass PRIVATE
    -fno-exceptions
    -fno-rtti
)

# EnergyModel.cpp uses nlohmann/json with try/catch — allow exceptions for that file only
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/bb-energy-analyzer/EnergyModel.cpp
    PROPERTIES COMPILE_OPTIONS "-fexceptions"
)

target_include_directories(RockClimbMachinePass PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/bb-energy-analyzer
)

# On macOS, use flat namespace so undefined symbols are resolved at runtime from llc
if(APPLE)
    set_target_properties(RockClimbMachinePass PROPERTIES
        LINK_FLAGS "-undefined dynamic_lookup"
    )
endif()

target_link_libraries(RockClimbMachinePass PRIVATE
    nlohmann_json::nlohmann_json
    plog::plog
)
