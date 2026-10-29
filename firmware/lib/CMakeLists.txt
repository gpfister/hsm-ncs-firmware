# Pure HSM core logic library
add_library(hsm_core STATIC
    src/hsm_core.c
)

target_include_directories(hsm_core PUBLIC
    include
)
