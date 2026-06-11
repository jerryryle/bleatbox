/*
 * Shell commands — test harness for sound playback.
 *
 * Available over USB CDC ACM serial.  Works regardless of whether
 * device provisioning succeeds.
 */

#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>

#include <stdlib.h>
#include <string.h>

#include "accel.h"
#include "audio.h"
#include "sdcard.h"
#include "sounds.h"
#include "vs1053b.h"

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

    if (audio_is_playing()) {
        shell_warn(sh, "Already playing — ignored");
        return 0;
    }

    shell_print(sh, "Playing %s %lu", argv[1], idx);
    audio_play_sound(path, 0);
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
    return 0;
}

static int cmd_sinetest(const struct shell *sh, size_t argc, char **argv)
{
    /* The codec is held in hardware reset while idle, so the test
     * must power it up first and power it back down afterwards. */
    if (!strcmp(argv[1], "on")) {
        if (audio_is_playing()) {
            shell_error(sh, "Busy — sound playing");
            return -EBUSY;
        }
        int ret = vs1053b_power_up();
        if (ret) {
            shell_error(sh, "Codec power-up failed: %d", ret);
            return ret;
        }
        shell_print(sh, "Sine test on — plug in headphones");
        return vs1053b_sine_test(true);
    } else if (!strcmp(argv[1], "off")) {
        /* A playback may have started since "on" (its power-up soft
         * reset already killed the tone) — don't yank the codec into
         * reset mid-stream. */
        if (audio_is_playing()) {
            shell_error(sh, "Busy — sound playing");
            return -EBUSY;
        }
        shell_print(sh, "Sine test off");
        int ret = vs1053b_sine_test(false);
        vs1053b_power_down();
        return ret;
    }

    shell_error(sh, "Usage: sinetest on|off");
    return -EINVAL;
}

SHELL_CMD_ARG_REGISTER(play, NULL,
                       "Play a sound: play goat|misc <index>",
                       cmd_play, 3, 0);

SHELL_CMD_ARG_REGISTER(sinetest, NULL,
                       "VS1053B sine test: sinetest on|off",
                       cmd_sinetest, 2, 0);

SHELL_CMD_ARG_REGISTER(volume, NULL,
                       "Get or set volume: volume [0-100]",
                       cmd_volume, 1, 1);

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

SHELL_CMD_ARG_REGISTER(accel, NULL,
                       "Sample accelerometer: accel [count] (default 200)",
                       cmd_accel, 1, 1);
