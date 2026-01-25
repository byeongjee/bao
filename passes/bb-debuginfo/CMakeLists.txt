# Standalone plugin - no Gurobi dependency
add_library(BBDebugInfoPass MODULE
    AssignBBDebugInfoPass.cpp
)

set_target_properties(BBDebugInfoPass PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    PREFIX ""
)

target_compile_options(BBDebugInfoPass PRIVATE
    -fno-exceptions
    -fno-rtti
)

if(APPLE)
    set_target_properties(BBDebugInfoPass PROPERTIES
        LINK_FLAGS "-undefined dynamic_lookup"
    )
endif()
