# Pure HSM core logic library
if(TARGET zephyr_interface)
    add_library(hsm_core STATIC
        src/hsm_core.c
        src/at_parser.c
        src/crypto_cc310.c
        src/keystore.c
        src/at_commands.c
        src/sss.c
    )

    target_include_directories(hsm_core PUBLIC
        include
    )

    target_link_libraries(hsm_core PRIVATE zephyr_interface)
else()
    cmake_minimum_required(VERSION 3.20.0)
    project(hsm_lib C)
    set(CMAKE_C_STANDARD 11)

    add_library(hsm_core STATIC
        src/sss.c
    )

    target_include_directories(hsm_core PUBLIC
        include
    )

    add_executable(test_sss
        tests/test_sss.c
    )

    target_link_libraries(test_sss PRIVATE hsm_core)
    enable_testing()
    add_test(NAME test_sss COMMAND test_sss)
endif()
