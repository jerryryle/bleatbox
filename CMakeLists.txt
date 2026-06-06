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
    src/broadcast_log.c
    src/broadcast_log_core.c
    src/assignments.c
    src/sounds.c
    src/wrap_to_range.c
    src/device_config.c
    src/device_config_parse.c
    src/sounds_match.c
    src/shell.c
)
