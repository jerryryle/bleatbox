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
#include <zephyr/sys/atomic.h>

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

/*
 * Set (via compare-and-set) by whoever claims the codec — a playback
 * request or the sine test — and cleared when the codec goes idle.
 */
static atomic_t g_playing;

/* Sine test currently running.  Only touched by audio_sine_test()
 * (shell thread), under the g_playing claim. */
static bool g_sine_active;

/* Set in audio_init() once the SPI bus and codec are up.  When false
 * (init failed), every operate path is a graceful no-op so the rest of
 * the system still runs without a usable codec. */
static bool g_ready;

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
    atomic_set(&g_playing, 0);
    g_sine_active = false;
    g_ready = false;

    if (!device_is_ready(g_spi_dev)) {
        LOG_ERR("SPI device not ready — audio disabled");
        return -ENODEV;
    }

    int ret = vs1053b_init(g_spi_dev);
    if (ret) {
        LOG_ERR("VS1053B init failed: %d — audio disabled", ret);
        return ret;
    }

    /* vs1053b_init() leaves the codec in hardware reset (~12 uA) until
     * the first playback.  audio_set_volume() can still be called — it
     * caches the value that vs1053b_power_up() applies on wake. */

    g_ready = true;
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
    if (!g_ready) {
        return -ENODEV;
    }
    return vs1053b_get_volume(percent);
}

int audio_set_volume(uint8_t percent)
{
    if (!g_ready) {
        return -ENODEV;
    }
    return vs1053b_set_volume(percent);
}

/* ------------------------------------------------------------------ */
/* Playback thread                                                    */
/* ------------------------------------------------------------------ */

/*
 * Post an audio session event to the main loop.  Unlike trigger
 * events, START and DONE must not be dropped — the main loop tracks
 * them as pairs to run its vibration suppression.  Block until a
 * slot frees: the main loop always drains the queue and never waits
 * on this thread, so this is brief and cannot deadlock.
 */
static void post_audio_event(enum event_type type)
{
    if (!g_event_q) {
        return;
    }

    struct event evt = {.type = type};
    k_msgq_put(g_event_q, &evt, K_FOREVER);
}

/*
 * Mark the audio session over and notify the main loop.  Must run on
 * every exit path of a playback request — a skipped EVENT_AUDIO_DONE
 * leaves the main loop's vibration cooldown stuck forever.
 */
static void finish_request(void)
{
    post_audio_event(EVENT_AUDIO_DONE);

    /*
     * Clear the claim only after AUDIO_DONE is queued: any vibration
     * event that arrives once the codec reads as idle is then ordered
     * behind AUDIO_DONE in the queue, so the main loop restarts the
     * cooldown before it can act on the vibration.
     */
    atomic_set(&g_playing, 0);
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

        /* The session covers the delay too — the device is committed
         * to this sound and must not trigger on stray vibration. */
        post_audio_event(EVENT_AUDIO_START);

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
        if (nread < 0) {
            LOG_ERR("Read error on %s: %d", req.path, (int)nread);
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
    return atomic_get(&g_playing) != 0;
}

int audio_play_sound(const char *path, uint16_t delay_ms)
{
    if (!g_ready) {
        LOG_WRN("audio_play_sound — audio disabled (codec init failed)");
        return -ENODEV;
    }

    if (!atomic_cas(&g_playing, 0, 1)) {
        LOG_WRN("audio_play_sound called while already playing — ignored");
        return -EBUSY;
    }

    struct play_request req = {.delay_ms = delay_ms};
    strncpy(req.path, path, sizeof(req.path) - 1);
    req.path[sizeof(req.path) - 1] = '\0';

    if (k_msgq_put(&g_play_q, &req, K_NO_WAIT)) {
        /* Unreachable — a successful claim means the previous request
         * was drained from the queue — but don't strand the claim if
         * that ever changes. */
        LOG_ERR("Playback request dropped — queue full");
        atomic_set(&g_playing, 0);
        return -EAGAIN;
    }
    return 0;
}

int audio_sine_test(bool enable)
{
    if (!g_ready) {
        return -ENODEV;
    }

    if (enable) {
        if (g_sine_active) {
            return 0;
        }

        /* Claim the codec like a playback request: trigger events are
         * dropped by the audio_is_playing() gates while the tone runs,
         * and playback cannot start until the test is stopped. */
        if (!atomic_cas(&g_playing, 0, 1)) {
            return -EBUSY;
        }

        int ret = vs1053b_power_up();
        if (ret == 0) {
            ret = vs1053b_sine_test(true);
        }
        if (ret) {
            /* No START was posted, so no DONE is owed — just release
             * the claim. */
            vs1053b_power_down();
            atomic_set(&g_playing, 0);
            return ret;
        }

        g_sine_active = true;
        post_audio_event(EVENT_AUDIO_START);
        return 0;
    }

    if (!g_sine_active) {
        return -EALREADY;
    }

    int ret = vs1053b_sine_test(false);
    vs1053b_power_down();
    g_sine_active = false;

    /* End the session exactly like a finished playback — DONE arms
     * the cooldown, since the tone shook the enclosure just like a
     * real sound. */
    finish_request();
    return ret;
}
