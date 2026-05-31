cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bleatbox)

target_sources(app PRIVATE
    src/main.c
    src/accel.c
    src/vs1053b.c
    src/sdcard.c
    src/audio.c
    src/ble.c
    src/assignments.c
    src/sounds.c
    src/clamp.c
    src/device_config.c
    src/shell.c
)
