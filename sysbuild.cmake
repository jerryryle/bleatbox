# Sysbuild: build the second-stage updater alongside the main app.
#
# This is NOT MCUboot — the updater is our own minimal loader that the UF2
# bootloader launches at 0x26000. Sysbuild builds both images and merges
# them into merged.hex for provisioning.

ExternalZephyrProject_Add(
    APPLICATION updater
    SOURCE_DIR ${APP_DIR}/updater
)
