# Standalone plugin - no Gurobi dependency
add_library(BBDebugInfoPass MODULE
    AssignBBDebugInfoPass.cpp
)

set_target_properties(BBDebugInfoPass PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    PREFIX ""
)

# Enable exceptions for nlohmann/json compatibility
# Unlike the main CheckpointPass, this plugin needs exceptions for JSON library
target_compile_options(BBDebugInfoPass PRIVATE
    -fno-rtti
)

# Link nlohmann_json for JSON output
target_link_libraries(BBDebugInfoPass PRIVATE
    nlohmann_json::nlohmann_json
)

if(APPLE)
    set_target_properties(BBDebugInfoPass PROPERTIES
        LINK_FLAGS "-undefined dynamic_lookup"
    )
endif()
