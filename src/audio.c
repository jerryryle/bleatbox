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
#include "sdcard.h"
#include "vs1053b.h"

LOG_MODULE_REGISTER(audio, LOG_LEVEL_INF);

/*
 * Optional VS1053B firmware patch, applied after every power-up
 * (hardware reset wipes it).  Compiled by scripts/compile_patch.py;
 * absent file means no patch — not an error.
 */
#define AUDIO_PATCH_PATH SDCARD_MOUNT_POINT "/patch.bin"

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

    if (!device_is_ready(g_spi_dev)) {
        LOG_ERR("SPI device not ready");
        return -ENODEV;
    }

    int ret = vs1053b_init(g_spi_dev);
    if (ret) {
        LOG_ERR("VS1053B init failed: %d", ret);
        return ret;
    }

    /* vs1053b_init() leaves the codec in hardware reset (~12 uA) until
     * the first playback.  audio_set_volume() can still be called — it
     * caches the value that vs1053b_power_up() applies on wake. */

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

/*
 * Patch file format (repeated until EOF), as written by
 * scripts/compile_patch.py:
 *   [register: 1 byte] [count: 2 bytes BE] [data: count x 2 bytes BE]
 */
static int apply_patch_records(struct fs_file_t *f)
{
    for (;;) {
        uint8_t reg;
        ssize_t n = fs_read(f, &reg, 1);
        if (n == 0) {
            return 0; /* EOF — all records applied */
        }
        if (n != 1) {
            return -EIO; /* read error is not EOF — don't claim success */
        }

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
}

/* A missing patch file is normal; anything else is logged. */
static void apply_patch_if_present(void)
{
    struct fs_file_t f;
    fs_file_t_init(&f);

    if (fs_open(&f, AUDIO_PATCH_PATH, FS_O_READ) < 0) {
        return;
    }

    int ret = apply_patch_records(&f);
    fs_close(&f);

    if (ret) {
        LOG_ERR("Patch %s failed: %d", AUDIO_PATCH_PATH, ret);
    } else {
        LOG_INF("Patch %s applied", AUDIO_PATCH_PATH);
    }
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

/*
 * Mark playback over and notify the main loop.  Must run on every
 * exit path of a playback request — a skipped EVENT_AUDIO_DONE
 * leaves the main loop's vibration cooldown stuck forever.
 */
static void finish_request(void)
{
    g_playing = false;

    if (g_event_q) {
        struct event done_evt = {.type = EVENT_AUDIO_DONE};
        /* Unlike trigger events, AUDIO_DONE must not be dropped — the
         * main loop restarts the vibration cooldown only on receipt.
         * Block until a slot frees: the main loop always drains the
         * queue and never waits on this thread, so this is brief and
         * cannot deadlock. */
        k_msgq_put(g_event_q, &done_evt, K_FOREVER);
    }
}

static void playback_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (;;) {
        struct play_request req;
        if (k_msgq_get(&g_play_q, &req, K_FOREVER)) {
            continue;
        }
        LOG_INF("Received playback request: path=%s delay=%u ms", req.path, req.delay_ms);

        if (req.delay_ms > 0) {
            LOG_INF("Delaying playback for %u ms", req.delay_ms);
            k_msleep(req.delay_ms);
        }

        struct fs_file_t f;
        fs_file_t_init(&f);
        int ret = fs_open(&f, req.path, FS_O_READ);
        if (ret < 0) {
            LOG_ERR("Cannot open %s: %d", req.path, ret);
            finish_request();
            continue;
        }

        ret = vs1053b_power_up();
        if (ret) {
            LOG_ERR("Codec power-up failed: %d", ret);
            vs1053b_power_down();
            fs_close(&f);
            finish_request();
            continue;
        }
        apply_patch_if_present();

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
        vs1053b_power_down();
        finish_request();
        LOG_INF("Playback finished");
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
    if (k_msgq_put(&g_play_q, &req, K_NO_WAIT)) {
        /* Queue full: lost a race with another caller, whose pending
         * request will clear g_playing via finish_request().  Keep the
         * flag set — a sound is still in flight. */
        LOG_WRN("Playback request dropped — another request pending");
    }
}
