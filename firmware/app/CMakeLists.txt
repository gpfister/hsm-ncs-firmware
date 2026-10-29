cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hms_app)

# Add pure HSM logic library
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib ${CMAKE_CURRENT_BINARY_DIR}/lib)

target_sources(app PRIVATE
    src/main.c
    src/led_status.c
    src/usb_serial.c
    src/dfu_service.c
)

target_link_libraries(app PRIVATE hsm_core)
