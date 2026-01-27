cmake_minimum_required(VERSION 3.20)
project(CheckpointPass)

# Fetch nlohmann/json (header-only JSON library)
include(FetchContent)
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

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
find_library(GUROBI_LIBRARY
    NAMES gurobi130 gurobi120 gurobi110 gurobi100 gurobi
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
    src/AssemblyBasedEstimator.cpp
    src/IRBasedEstimator.cpp
    src/EnergyEstimatorFactory.cpp
    src/CFGAnalysis.cpp
    src/CheckpointOptimizer.cpp
    src/CheckpointInstrumenter.cpp
    src/CheckpointPass.cpp
    src/LoopTripCount.cpp
    src/MaxCheckpointCounter.cpp
    src/CheckpointAnalysisPass.cpp
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
)

# Build BB debug info pass as separate plugin (no Gurobi dependency)
add_subdirectory(bb-debuginfo)

# Build BB energy analyzer tool (standalone executable, no Gurobi dependency)
add_subdirectory(bb-energy-analyzer)
