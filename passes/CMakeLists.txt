cmake_minimum_required(VERSION 3.20)
project(CheckpointPass)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Fetch nlohmann/json (header-only JSON library)
include(FetchContent)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_Declare(plog
    GIT_REPOSITORY https://github.com/SergiusTheBest/plog.git
    GIT_TAG        1.1.11)
FetchContent_MakeAvailable(json plog)

# Find LLVM
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVM from: ${LLVM_DIR}")

# Use LLVM's cmake utilities
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)
include(HandleLLVMOptions)

# Set up LLVM include directories and definitions
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})
include_directories(${LLVM_INCLUDE_DIRS})

# Find Gurobi
set(GUROBI_HOME "$ENV{GUROBI_HOME}" CACHE PATH "Gurobi installation directory")
find_path(GUROBI_INCLUDE_DIR gurobi_c++.h
    PATHS ${GUROBI_HOME}/include)

# Detect Gurobi version from header to find the correct versioned library
# (e.g., libgurobi130 for Gurobi 13.0) without hardcoding version strings.
if(GUROBI_INCLUDE_DIR AND EXISTS "${GUROBI_INCLUDE_DIR}/gurobi_c.h")
    file(STRINGS "${GUROBI_INCLUDE_DIR}/gurobi_c.h" _grb_major_line REGEX "^#define GRB_VERSION_MAJOR")
    file(STRINGS "${GUROBI_INCLUDE_DIR}/gurobi_c.h" _grb_minor_line REGEX "^#define GRB_VERSION_MINOR")
    string(REGEX MATCH "[0-9]+" _grb_major "${_grb_major_line}")
    string(REGEX MATCH "[0-9]+" _grb_minor "${_grb_minor_line}")
    set(_gurobi_lib_name "gurobi${_grb_major}${_grb_minor}")
    message(STATUS "Detected Gurobi version: ${_grb_major}.${_grb_minor} (library: ${_gurobi_lib_name})")
endif()

find_library(GUROBI_LIBRARY
    NAMES ${_gurobi_lib_name} gurobi
    PATHS ${GUROBI_HOME}/lib)
find_library(GUROBI_CXX_LIBRARY gurobi_c++
    PATHS ${GUROBI_HOME}/lib)

if(NOT GUROBI_LIBRARY OR NOT GUROBI_CXX_LIBRARY)
    message(FATAL_ERROR "Gurobi not found. Set GUROBI_HOME environment variable.")
endif()
message(STATUS "Found Gurobi: ${GUROBI_LIBRARY}")

include_directories(
    ${GUROBI_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/include
)

# Build as a shared module (not using add_llvm_pass_plugin to avoid linking LLVM)
add_library(CheckpointPass MODULE
    # Common utilities
    src/common/Logger.cpp
    src/common/BBNaming.cpp
    src/common/BlockSplitter.cpp
    src/common/CFGAnalysis.cpp
src/common/LoopTripCount.cpp
    src/common/BBFreqCollectorPass.cpp
    src/common/PassRegistry.cpp
    src/common/PassStatistics.cpp
    src/common/TripCountAnnotationPass.cpp
    src/common/EdgeSplitPass.cpp
    # Energy estimators
    src/estimator/AssemblyBasedEstimator.cpp
    src/estimator/IRBasedEstimator.cpp
    src/estimator/EnergyEstimatorFactory.cpp
    # MILP checkpoint insertion
    src/milp/BBFreqLoader.cpp
    src/milp/StateAnalysis.cpp
    src/milp/LivenessAnalysis.cpp
    src/milp/EnergyModel.cpp
    src/milp/LoopStripMiningPass.cpp
    src/milp/AbstractCFG.cpp
    src/milp/MILPCheckpointPass.cpp
    src/milp/CheckpointOptimizer.cpp
    src/milp/CheckpointInstrumenter.cpp
    # SCHEMATIC (greedy heuristic baseline)
    src/schematic/SchematicParams.cpp
    src/schematic/SchematicPass.cpp
    src/schematic/SchematicStateAnalysis.cpp
    src/schematic/MemoryAllocator.cpp
    src/schematic/TraceAnalyzer.cpp

    src/schematic/RCGSolver.cpp
    src/schematic/LoopAnalyzer.cpp
    src/schematic/EnergyPropagation.cpp
    src/schematic/SchematicInstrumenter.cpp
    src/schematic/TraceCollectorPass.cpp
    src/schematic/TraceLoader.cpp
    src/schematic/SchematicBlock.cpp
)

# Use C++17 and disable RTTI/exceptions to match LLVM
set_target_properties(CheckpointPass PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    PREFIX ""  # Don't prefix with 'lib'
)

# Add LLVM compile flags (without exceptions, with RTTI disabled)
target_compile_options(CheckpointPass PRIVATE
    -fno-exceptions
    -fno-rtti
)

# On macOS, use flat namespace so undefined symbols are resolved at runtime from opt
if(APPLE)
    set_target_properties(CheckpointPass PROPERTIES
        LINK_FLAGS "-undefined dynamic_lookup"
    )
endif()

# Only link Gurobi (LLVM symbols come from opt at runtime)
target_link_libraries(CheckpointPass PRIVATE
    ${GUROBI_CXX_LIBRARY}
    ${GUROBI_LIBRARY}
    nlohmann_json::nlohmann_json
    plog::plog
)

# Build BB debug info pass as separate plugin (no Gurobi dependency)
add_subdirectory(bb-debuginfo)

# Build BB energy analyzer tool (standalone executable, no Gurobi dependency)
add_subdirectory(bb-energy-analyzer)

# Build RockClimb machine-level pass (post-regalloc, no Gurobi dependency)
add_subdirectory(rockclimb-backend)
