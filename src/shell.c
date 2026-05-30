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
		shell_print(sh, "Usage: bleatbox volume <0-100>");
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

SHELL_STATIC_SUBCMD_SET_CREATE(sub_bleatbox,
	SHELL_CMD_ARG(play, NULL,
		      "Play a sound: bleatbox play <index>",
		      cmd_play, 2, 0),
	SHELL_CMD_ARG(volume, NULL,
		      "Set volume: bleatbox volume <0-100>",
		      cmd_volume, 1, 1),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(bleatbox, &sub_bleatbox,
		   "BleatBox test commands", NULL);
