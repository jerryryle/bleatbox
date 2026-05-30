/*
 * Shell commands — test harness for sound playback.
 *
 * Available over USB CDC ACM serial.  Works regardless of whether
 * device provisioning succeeds.
 */

#include <zephyr/shell/shell.h>
#include <stdlib.h>

#include "audio.h"

static int cmd_play(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long idx = strtoul(argv[1], NULL, 0);

	if (idx > 255) {
		shell_error(sh, "Sound index must be 0-255");
		return -EINVAL;
	}

	if (audio_is_playing()) {
		shell_warn(sh, "Already playing — ignored");
		return 0;
	}

	shell_print(sh, "Playing sound %lu", idx);
	audio_play_sound((uint8_t)idx);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_bleatbox,
	SHELL_CMD_ARG(play, NULL,
		      "Play a sound: bleatbox play <index>",
		      cmd_play, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(bleatbox, &sub_bleatbox,
		   "BleatBox test commands", NULL);
