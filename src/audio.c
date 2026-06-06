/*
 * Audio subsystem — sound playback via VS1053B codec.
 *
 * Playback runs on a dedicated background thread.  audio_play_sound()
 * signals the thread and returns immediately so the caller is never
 * blocked during audio streaming.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <string.h>

#include "audio.h"
#include "events.h"
#include "vs1053b.h"

LOG_MODULE_REGISTER(audio, LOG_LEVEL_INF);

/* ------------------------------------------------------------------ */
/* Hardware                                                           */
/* ------------------------------------------------------------------ */

static const struct device *g_spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi1));
static struct k_msgq *g_event_q;

static volatile bool g_playing;

#define SOUND_PATH_MAX 32

struct play_request {
    char path[SOUND_PATH_MAX];
    uint16_t delay_ms;
};

K_MSGQ_DEFINE(g_play_q, sizeof(struct play_request), 1, 4);

#define AUDIO_STACK_SIZE 4096
#define AUDIO_PRIORITY 5

static void playback_thread(void *p1, void *p2, void *p3);
K_THREAD_DEFINE(audio_tid, AUDIO_STACK_SIZE,
                playback_thread, NULL, NULL, NULL,
                AUDIO_PRIORITY, 0, 0);

int audio_init(struct k_msgq *event_q)
{
    g_event_q = event_q;
    g_playing = false;
    k_msgq_purge(&g_play_q);

    if (!device_is_ready(g_spi_dev)) {
        LOG_ERR("SPI device not ready");
        return -ENODEV;
    }

    int ret = vs1053b_init(g_spi_dev);
    if (ret) {
        LOG_ERR("VS1053B init failed: %d", ret);
        return ret;
    }

    LOG_INF("Audio initialized");
    return 0;
}

/*
 * Read one big-endian uint16_t from the file.
 * Returns 0 on success, -EIO on short/failed read.
 */
static int read_u16(struct fs_file_t *f, uint16_t *val)
{
    uint8_t buf[2];

    if (fs_read(f, buf, 2) != 2) {
        return -EIO;
    }
    *val = ((uint16_t)buf[0] << 8) | buf[1];
    return 0;
}

static int apply_patch_records(struct fs_file_t *f)
{
    uint8_t reg;

    while (fs_read(f, &reg, 1) == 1) {
        uint16_t count;
        int ret = read_u16(f, &count);
        if (ret) {
            return ret;
        }

        for (uint16_t i = 0; i < count; i++) {
            uint16_t val;
            ret = read_u16(f, &val);
            if (ret) {
                return ret;
            }
            ret = vs1053b_write_reg(reg, val);
            if (ret) {
                return ret;
            }
        }
    }

    return 0;
}

int audio_apply_patch(const char *path)
{
    struct fs_file_t f;
    fs_file_t_init(&f);

    int ret = fs_open(&f, path, FS_O_READ);
    if (ret < 0) {
        LOG_ERR("Cannot open patch %s: %d", path, ret);
        return ret;
    }

    ret = apply_patch_records(&f);
    fs_close(&f);

    if (ret) {
        LOG_ERR("Patch %s failed: %d", path, ret);
    } else {
        LOG_INF("Patch %s applied", path);
    }
    return ret;
}

int audio_get_volume(uint8_t *percent)
{
    return vs1053b_get_volume(percent);
}

int audio_set_volume(uint8_t percent)
{
    return vs1053b_set_volume(percent);
}

/* ------------------------------------------------------------------ */
/* Playback thread                                                    */
/* ------------------------------------------------------------------ */

static void playback_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (;;) {
        struct play_request req;
        k_msgq_get(&g_play_q, &req, K_FOREVER);

        if (req.delay_ms > 0) {
            k_msleep(req.delay_ms);
        }

        struct fs_file_t f;
        fs_file_t_init(&f);
        int ret = fs_open(&f, req.path, FS_O_READ);
        if (ret < 0) {
            LOG_ERR("Cannot open %s: %d", req.path, ret);
            g_playing = false;
            continue;
        }

        LOG_INF("Playing %s", req.path);

        uint8_t buf[VS1053B_DATA_CHUNK];
        ssize_t nread;
        while ((nread = fs_read(&f, buf, sizeof(buf))) > 0) {
            ret = vs1053b_write_data(buf, nread);
            if (ret) {
                LOG_ERR("Playback aborted: SPI error");
                break;
            }
        }

        fs_close(&f);
        vs1053b_end_playback();
        g_playing = false;
        LOG_INF("Playback finished");

        if (g_event_q) {
            struct event done_evt = {.type = EVENT_AUDIO_DONE};
            k_msgq_put(g_event_q, &done_evt, K_NO_WAIT);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

bool audio_is_playing(void)
{
    return g_playing;
}

void audio_play_sound(const char *path, uint16_t delay_ms)
{
    if (g_playing) {
        LOG_WRN("audio_play_sound called while already playing — ignored");
        return;
    }

    struct play_request req = {.delay_ms = delay_ms};
    strncpy(req.path, path, sizeof(req.path) - 1);
    req.path[sizeof(req.path) - 1] = '\0';

    g_playing = true;
    k_msgq_put(&g_play_q, &req, K_NO_WAIT);
}
