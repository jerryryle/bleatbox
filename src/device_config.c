#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include <string.h>

#include "device_config.h"

LOG_MODULE_REGISTER(device_config, LOG_LEVEL_INF);

#define CONFIG_PATH "/SD:/bleatbox.cfg"
#define LINE_MAX    256

#define DEFAULT_VOLUME 80

static uint8_t device_id;
static uint8_t peers[DEVICE_CONFIG_MAX_PEERS];
static uint8_t peer_count;
static bool has_id;
static uint8_t volume = DEFAULT_VOLUME;

static int parse_hex_byte(const char *s, uint8_t *out)
{
	char *end;
	unsigned long val = strtoul(s, &end, 16);

	if (end == s || val > 0xFF) {
		return -EINVAL;
	}
	*out = (uint8_t)val;
	return 0;
}

static int parse_line(char *line)
{
	while (*line == ' ' || *line == '\t') {
		line++;
	}

	if (*line == '#' || *line == '\n' || *line == '\r' || *line == '\0') {
		return 0;
	}

	char *saveptr;
	char *key = strtok_r(line, " \t\r\n", &saveptr);

	if (!key) {
		return 0;
	}

	if (strcmp(key, "id") == 0) {
		char *val = strtok_r(NULL, " \t\r\n", &saveptr);
		if (!val) {
			LOG_ERR("'id' requires a value");
			return -EINVAL;
		}
		if (parse_hex_byte(val, &device_id)) {
			LOG_ERR("Invalid id value: %s", val);
			return -EINVAL;
		}
		has_id = true;
	} else if (strcmp(key, "peers") == 0) {
		peer_count = 0;
		char *val;

		while ((val = strtok_r(NULL, " \t\r\n", &saveptr)) != NULL) {
			if (peer_count >= DEVICE_CONFIG_MAX_PEERS) {
				LOG_ERR("Too many peers (max %d)",
					DEVICE_CONFIG_MAX_PEERS);
				return -ENOSPC;
			}
			if (parse_hex_byte(val, &peers[peer_count])) {
				LOG_ERR("Invalid peer value: %s", val);
				return -EINVAL;
			}
			peer_count++;
		}
		if (peer_count == 0) {
			LOG_ERR("'peers' requires at least one value");
			return -EINVAL;
		}
	} else if (strcmp(key, "volume") == 0) {
		char *val = strtok_r(NULL, " \t\r\n", &saveptr);
		if (!val) {
			LOG_ERR("'volume' requires a value");
			return -EINVAL;
		}
		unsigned long v = strtoul(val, NULL, 10);
		if (v > 100) {
			LOG_ERR("Volume must be 0-100, got: %s", val);
			return -EINVAL;
		}
		volume = (uint8_t)v;
	} else {
		LOG_WRN("Unknown config key: %s", key);
	}

	return 0;
}

int device_config_load(void)
{
	struct fs_file_t file;
	fs_file_t_init(&file);

	int ret = fs_open(&file, CONFIG_PATH, FS_O_READ);
	if (ret) {
		LOG_ERR("Failed to open %s: %d", CONFIG_PATH, ret);
		return ret;
	}

	char buf[LINE_MAX];
	int pos = 0;
	ssize_t nread;

	while ((nread = fs_read(&file, buf + pos, 1)) == 1) {
		if (buf[pos] == '\n' || pos >= LINE_MAX - 2) {
			buf[pos + 1] = '\0';
			ret = parse_line(buf);
			if (ret) {
				fs_close(&file);
				return ret;
			}
			pos = 0;
		} else {
			pos++;
		}
	}

	if (pos > 0) {
		buf[pos] = '\0';
		ret = parse_line(buf);
		if (ret) {
			fs_close(&file);
			return ret;
		}
	}

	fs_close(&file);

	if (!has_id) {
		LOG_ERR("Config file missing 'id'");
		return -EINVAL;
	}
	if (peer_count == 0) {
		LOG_ERR("Config file missing 'peers'");
		return -EINVAL;
	}

	LOG_INF("Device ID: 0x%02x", device_id);
	LOG_HEXDUMP_INF(peers, peer_count, "Peers:");

	return 0;
}

uint8_t device_config_get_id(void)
{
	return device_id;
}

const uint8_t *device_config_get_peers(uint8_t *count)
{
	*count = peer_count;
	return peers;
}

uint8_t device_config_get_volume(void)
{
	return volume;
}
