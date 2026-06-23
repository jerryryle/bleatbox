/*
 * Shell commands — test harness for sound playback.
 *
 * Available over USB CDC ACM serial.  Works regardless of whether
 * device provisioning succeeds.
 */

#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_power.h>

#include <stdlib.h>
#include <string.h>

#include "accel.h"
#include "audio.h"
#include "battery.h"
#include "ble.h"
#include "compose.h"
#include "device_config.h"
#include "device_config_parse.h"
#include "message.h"
#include "ota.h"
#include "sdcard.h"
#include "sounds.h"

LOG_MODULE_REGISTER(shell_cmds, LOG_LEVEL_INF);

static int cmd_play(const struct shell *sh, size_t argc, char **argv)
{
    enum sound_type type;
    if (!strcmp(argv[1], "goat")) {
        type = SOUND_TYPE_GOAT;
    } else if (!strcmp(argv[1], "misc")) {
        type = SOUND_TYPE_MISC;
    } else {
        shell_error(sh, "Unknown sound type '%s' (use goat or misc)",
                    argv[1]);
        return -EINVAL;
    }

    char *end;
    unsigned long idx = strtoul(argv[2], &end, 0);
    if (end == argv[2] || idx > UINT8_MAX) {
        shell_error(sh, "Invalid index: %s", argv[2]);
        return -EINVAL;
    }

    char path[32];
    int ret = sounds_get_path(type, (uint8_t)idx, path, sizeof(path));
    if (ret) {
        uint8_t count = sounds_get_count(type);
        shell_error(sh, "Index must be 0-%u (%u %s sounds available)",
                    count ? count - 1 : 0, count, argv[1]);
        return -EINVAL;
    }

    if (!sdcard_is_mounted()) {
        shell_error(sh, "SD card not mounted");
        return -ENODEV;
    }

    if (audio_play_sound(path, 0)) {
        shell_warn(sh, "Already playing — ignored");
        return 0;
    }

    shell_print(sh, "Playing %s %lu", argv[1], idx);
    return 0;
}

static int cmd_volume(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        uint8_t vol;
        int ret = audio_get_volume(&vol);
        if (ret) {
            shell_error(sh, "Failed to read volume: %d", ret);
            return ret;
        }
        shell_print(sh, "Volume: %u%%", vol);
        return 0;
    }

    char *end;
    unsigned long vol = strtoul(argv[1], &end, 10);

    if (end == argv[1] || vol > 100) {
        shell_error(sh, "Volume must be 0-100");
        return -EINVAL;
    }

    int ret = audio_set_volume((uint8_t)vol);
    if (ret) {
        shell_error(sh, "Failed to set volume: %d", ret);
        return ret;
    }

    shell_print(sh, "Volume set to %lu%%", vol);

    ret = device_config_save_volume((uint8_t)vol);
    if (ret) {
        shell_warn(sh, "Applied, but failed to save to config: %d", ret);
        return ret;
    }

    shell_print(sh, "Saved to config");
    return 0;
}

static int cmd_sinetest(const struct shell *sh, size_t argc, char **argv)
{
    if (!strcmp(argv[1], "on")) {
        int ret = audio_sine_test(true);
        if (ret == -EBUSY) {
            shell_error(sh, "Busy — sound playing");
            return ret;
        }
        if (ret) {
            shell_error(sh, "Sine test start failed: %d", ret);
            return ret;
        }
        shell_print(sh, "Sine test on — plug in headphones");
        return 0;
    } else if (!strcmp(argv[1], "off")) {
        int ret = audio_sine_test(false);
        if (ret == -EALREADY) {
            shell_error(sh, "Sine test not running");
            return ret;
        }
        if (ret) {
            shell_error(sh, "Sine test stop failed: %d", ret);
            return ret;
        }
        shell_print(sh, "Sine test off");
        return 0;
    }

    shell_error(sh, "Usage: sinetest on|off");
    return -EINVAL;
}

static int cmd_accel(const struct shell *sh, size_t argc, char **argv)
{
    unsigned long count = 200;

    if (argc >= 2) {
        char *end;
        count = strtoul(argv[1], &end, 10);
        if (end == argv[1] || count == 0) {
            shell_error(sh, "Count must be a positive integer");
            return -EINVAL;
        }
    }

    uint8_t prev_ctrl1;
    int ret = accel_odr_boost(&prev_ctrl1);
    if (ret) {
        shell_error(sh, "Failed to boost ODR: %d", ret);
        return ret;
    }

    /* Let the new ODR settle */
    k_msleep(20);

    shell_print(sh, "Sampling %lu readings at ~100 Hz (values in mg)...",
                count);

    for (unsigned long i = 0; i < count; i++) {
        struct sensor_value x, y, z;

        ret = accel_sample_xyz(&x, &y, &z);
        if (ret) {
            shell_error(sh, "Sample failed: %d", ret);
            break;
        }

        /* sensor_value is in m/s² (val1=integer, val2=micro-fractional).
         * Convert to milli-g: mg = (m/s² * 1000000 + micro) / 9807 */
        int32_t x_mg = ((int64_t)x.val1 * 1000000 + x.val2) / 9807;
        int32_t y_mg = ((int64_t)y.val1 * 1000000 + y.val2) / 9807;
        int32_t z_mg = ((int64_t)z.val1 * 1000000 + z.val2) / 9807;

        shell_print(sh, "X=%6d  Y=%6d  Z=%6d", x_mg, y_mg, z_mg);

        k_msleep(10);
    }

    ret = accel_odr_restore(prev_ctrl1);
    if (ret) {
        shell_error(sh, "Failed to restore ODR: %d", ret);
        return ret;
    }
    return 0;
}


static int cmd_threshold(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        uint16_t mg;
        int ret = accel_get_threshold(&mg);
        if (ret) {
            shell_error(sh, "Failed to read threshold: %d", ret);
            return ret;
        }
        shell_print(sh, "Vibration threshold: %u mg", mg);
        return 0;
    }

    char *end;
    unsigned long mg = strtoul(argv[1], &end, 10);
    if (end == argv[1] || mg == 0 || mg > UINT16_MAX) {
        shell_error(sh, "Threshold must be 1-%u mg", UINT16_MAX);
        return -EINVAL;
    }

    int ret = accel_set_threshold((uint16_t)mg);
    if (ret) {
        shell_error(sh, "Failed to set threshold: %d", ret);
        return ret;
    }

    shell_print(sh, "Vibration threshold set to %lu mg", mg);

    ret = device_config_save_accel_threshold((uint16_t)mg);
    if (ret) {
        shell_warn(sh, "Applied, but failed to save to config: %d", ret);
        return ret;
    }

    shell_print(sh, "Saved to config");
    return 0;
}

static int cmd_id(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        uint8_t cur;
        int ret = ble_get_device_id(&cur);
        if (ret) {
            shell_error(sh, "Failed to read id: %d", ret);
            return ret;
        }
        shell_print(sh, "Device id: 0x%02X", cur);
        return 0;
    }

    uint8_t id;
    if (device_config_parse_hex_byte(argv[1], &id)) {
        shell_error(sh, "ID must be a hex byte 00-FF");
        return -EINVAL;
    }
    if (id == MESSAGE_EXT_ORIGINATOR) {
        shell_error(sh, "ID 0x%02X is reserved", MESSAGE_EXT_ORIGINATOR);
        return -EINVAL;
    }

    int ret = ble_set_device_id(id);
    if (ret) {
        shell_error(sh, "Failed to set id: %d", ret);
        return ret;
    }
    ota_set_device_id(id);

    shell_print(sh, "Device id set to 0x%02X", id);

    ret = device_config_save_id(id);
    if (ret) {
        shell_warn(sh, "Applied, but failed to save to config: %d", ret);
        return ret;
    }

    shell_print(sh, "Saved to config");
    return 0;
}

static int cmd_delay(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        uint16_t min_ms, max_ms;
        compose_get_delay(&min_ms, &max_ms);
        shell_print(sh, "Delay range: %u-%u ms", min_ms, max_ms);
        return 0;
    }
    if (argc < 3) {
        shell_error(sh, "Usage: delay <min> <max>");
        return -EINVAL;
    }

    char *end;
    unsigned long min_ms = strtoul(argv[1], &end, 10);
    if (end == argv[1] || min_ms > UINT16_MAX) {
        shell_error(sh, "Delay values must be 0-%u ms", UINT16_MAX);
        return -EINVAL;
    }
    unsigned long max_ms = strtoul(argv[2], &end, 10);
    if (end == argv[2] || max_ms > UINT16_MAX) {
        shell_error(sh, "Delay values must be 0-%u ms", UINT16_MAX);
        return -EINVAL;
    }
    if (min_ms > max_ms) {
        shell_error(sh, "delay_min (%lu) must not exceed delay_max (%lu)",
                    min_ms, max_ms);
        return -EINVAL;
    }

    compose_set_delay((uint16_t)min_ms, (uint16_t)max_ms);
    shell_print(sh, "Delay range set to %lu-%lu ms", min_ms, max_ms);

    int ret = device_config_save_delay((uint16_t)min_ms, (uint16_t)max_ms);
    if (ret) {
        shell_warn(sh, "Applied, but failed to save to config: %d", ret);
        return ret;
    }

    shell_print(sh, "Saved to config");
    return 0;
}

static int cmd_relay_ttl(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        uint8_t cur;
        int ret = ble_get_relay_ttl(&cur);
        if (ret) {
            shell_error(sh, "Failed to read relay TTL: %d", ret);
            return ret;
        }
        shell_print(sh, "Relay TTL: %u", cur);
        return 0;
    }

    char *end;
    unsigned long ttl = strtoul(argv[1], &end, 10);
    if (end == argv[1] || ttl > UINT8_MAX) {
        shell_error(sh, "Relay TTL must be 0-%u", UINT8_MAX);
        return -EINVAL;
    }

    int ret = ble_set_relay_ttl((uint8_t)ttl);
    if (ret) {
        shell_error(sh, "Failed to set relay TTL: %d", ret);
        return ret;
    }

    shell_print(sh, "Relay TTL set to %lu", ttl);

    ret = device_config_save_relay_ttl((uint8_t)ttl);
    if (ret) {
        shell_warn(sh, "Applied, but failed to save to config: %d", ret);
        return ret;
    }

    shell_print(sh, "Saved to config");
    return 0;
}

/* Rough LiPo state-of-charge: linear between 3.3 V (empty) and 4.2 V
 * (full), clamped.  A crude approximation — the discharge curve is not
 * linear — but enough to tell a healthy pack from a dying one. */
static int battery_percent(int32_t mv)
{
    if (mv <= 3300) {
        return 0;
    }
    if (mv >= 4200) {
        return 100;
    }
    return (mv - 3300) * 100 / (4200 - 3300);
}

static int cmd_battery(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    int32_t mv;
    int ret = battery_read_mv(&mv);
    if (ret) {
        shell_error(sh, "Battery read failed: %d", ret);
        return ret;
    }

    shell_print(sh, "Battery: %d mV (~%d%%)", mv, battery_percent(mv));
    return 0;
}

static int cmd_ota(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);

    if (!strcmp(argv[1], "status")) {
        shell_print(sh, "OTA window: %s",
                    ota_is_active() ? "open" : "closed");
        return 0;
    }

    if (!strcmp(argv[1], "arm")) {
        ota_start();
        shell_print(sh, "OTA window opened");
        return 0;
    }

    if (!strcmp(argv[1], "cancel")) {
        ota_cancel();
        shell_print(sh, "OTA window closed");
        return 0;
    }

    shell_error(sh, "Usage: ota status|arm|cancel");
    return -EINVAL;
}

/* Adafruit nRF52 bootloader magic: GPREGRET == 0x57 (DFU_MAGIC_UF2_RESET)
 * makes the bootloader stay in UF2 mass-storage mode after a reset, so a
 * sealed-up box can be reflashed over USB without the double-tap-reset dance.
 * The register survives the soft reset; the bootloader reads and clears it. */
#define UF2_BOOTLOADER_MAGIC 0x57

static int cmd_bootloader(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    shell_print(sh, "Rebooting into UF2 bootloader...");

    nrf_power_gpregret_set(NRF_POWER, 0, UF2_BOOTLOADER_MAGIC);

    /* Let the shell flush the line over USB CDC ACM before we reset. */
    k_msleep(100);

    sys_reboot(SYS_REBOOT_COLD);

    return 0; /* unreachable */
}

SHELL_CMD_ARG_REGISTER(play, NULL,
                       "Play a sound: play goat|misc <index>",
                       cmd_play, 3, 0);

SHELL_CMD_ARG_REGISTER(sinetest, NULL,
                       "VS1053B sine test: sinetest on|off",
                       cmd_sinetest, 2, 0);

SHELL_CMD_ARG_REGISTER(volume, NULL,
                       "Get, or set and save, volume: volume [0-100]",
                       cmd_volume, 1, 1);

SHELL_CMD_ARG_REGISTER(accel, NULL,
                       "Sample accelerometer: accel [count] (default 200)",
                       cmd_accel, 1, 1);

SHELL_CMD_ARG_REGISTER(threshold, NULL,
                       "Get, or set and save, vibration threshold in mg: "
                       "threshold [mg]",
                       cmd_threshold, 1, 1);

SHELL_CMD_ARG_REGISTER(id, NULL,
                       "Get, or set and save, device id in hex: "
                       "id [hex]",
                       cmd_id, 1, 1);

SHELL_CMD_ARG_REGISTER(delay, NULL,
                       "Get, or set and save, broadcast delay range in ms: "
                       "delay [min max]",
                       cmd_delay, 1, 2);

SHELL_CMD_ARG_REGISTER(relay_ttl, NULL,
                       "Get, or set and save, relay TTL (max mesh hops): "
                       "relay_ttl [0-255]",
                       cmd_relay_ttl, 1, 1);

SHELL_CMD_ARG_REGISTER(battery, NULL,
                       "Read battery voltage and approximate charge",
                       cmd_battery, 1, 0);

SHELL_CMD_ARG_REGISTER(ota, NULL,
                       "Over-the-air update window: ota status|arm|cancel",
                       cmd_ota, 2, 0);

SHELL_CMD_ARG_REGISTER(bootloader, NULL,
                       "Reboot into the UF2 bootloader for USB firmware update",
                       cmd_bootloader, 1, 0);
