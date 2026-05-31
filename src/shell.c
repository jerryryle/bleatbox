/*
 * Shell commands — test harness for sound playback.
 *
 * Available over USB CDC ACM serial.  Works regardless of whether
 * device provisioning succeeds.
 */

#include <zephyr/shell/shell.h>
#include <stdlib.h>

#include "audio.h"
#include "sdcard.h"
#include "sounds.h"
#include "vs1053b.h"

static int cmd_play(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long idx = strtoul(argv[1], NULL, 0);

	uint8_t count = sounds_get_count();
	if (!count || idx >= count) {
		shell_error(sh, "Sound index must be 0-%u (%u available)",
			    count ? count - 1 : 0, count);
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

	shell_print(sh, "Playing sound %lu", idx);
	audio_play_sound((uint8_t)idx);
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
	if (!strcmp(argv[1], "on")) {
		shell_print(sh, "Sine test on — plug in headphones");
		return vs1053b_sine_test(true);
	} else if (!strcmp(argv[1], "off")) {
		shell_print(sh, "Sine test off");
		return vs1053b_sine_test(false);
	}

	shell_error(sh, "Usage: sinetest on|off");
	return -EINVAL;
}

SHELL_CMD_ARG_REGISTER(play, NULL,
		       "Play a sound: play <index>",
		       cmd_play, 2, 0);

SHELL_CMD_ARG_REGISTER(sinetest, NULL,
		       "VS1053B sine test: sinetest on|off",
		       cmd_sinetest, 2, 0);

SHELL_CMD_ARG_REGISTER(volume, NULL,
		       "Get or set volume: volume [0-100]",
		       cmd_volume, 1, 1);
