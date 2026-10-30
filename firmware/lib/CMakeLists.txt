# Pure HSM core logic library
add_library(hsm_core STATIC
    src/hsm_core.c
    src/at_parser.c
    src/crypto_cc310.c
    src/keystore.c
    src/at_commands.c
)

target_include_directories(hsm_core PUBLIC
    include
)

target_link_libraries(hsm_core PRIVATE zephyr_interface)
