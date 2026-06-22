/*
 * BleatBox second-stage updater.
 *
 * The Adafruit UF2 bootloader always launches the image at 0x26000 — this
 * updater.  On every boot it:
 *
 *   1. Mounts the SD card and looks for a staged image (FW_IMAGE_SD_PATH).
 *   2. Validates its 16-byte header + CRC over the payload.
 *   3. If valid, erases and rewrites the main_app flash region from the
 *      file, then re-reads flash and re-checks the CRC.
 *   4. Deletes the file and chain-loads the main app at 0x3e000.
 *
 * The design is power-loss-resumable: the staged file is deleted only
 * after a verified write, so an interrupted flash simply re-runs on the
 * next boot.  We never erase the main app until the file CRC checks out,
 * and if a write fails after erasing we reboot to retry rather than jump
 * into a half-written app.  Worst case, the UF2 bootloader + USB drag-drop
 * always recovers the box.
 *
 * The chain-load mirrors MCUboot's Cortex-M handoff and can only be
 * validated on hardware.
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include <ff.h>

#include <cmsis_core.h>
#include <hal/nrf_clock.h>
#include <hal/nrf_power.h>
#include <hal/nrf_usbd.h>

#include "fw_image.h"

/* Base address of the main application's vector table. */
#define MAIN_APP_ADDR DT_REG_ADDR(DT_NODELABEL(main_app))

/* nRF52840 internal flash: uniform 4 KB pages, 4-byte write granularity. */
#define FLASH_PAGE_SIZE   4096
#define FLASH_WRITE_ALIGN 4

#define CHUNK_SIZE 4096

#define SD_MOUNT_POINT "/SD:"

static uint8_t g_chunk[CHUNK_SIZE];

static FATFS g_fat_fs;
static struct fs_mount_t g_mount = {
    .type = FS_FATFS,
    .fs_data = &g_fat_fs,
    .mnt_point = SD_MOUNT_POINT,
};

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec g_led = GPIO_DT_SPEC_GET_OR(LED0_NODE, gpios, {0});

/* ------------------------------------------------------------------ */
/* Chain-load                                                         */
/* ------------------------------------------------------------------ */

/*
 * Return the clock and USB hardware to its reset state before handing off.
 *
 * The app's USB controller starts the HFXO through the clock-control onoff
 * manager and then spins until the USBD signals ready — which only happens
 * once the HFXO is running.  After a chain-load the CLOCK/POWER peripherals
 * are still in the state we (and the bootloader's USB session) left them,
 * which stops that HFXO start from ever completing and hangs the app's
 * usbd_enable().  Clearing the interrupts, stopping the clocks, and clearing
 * the stale events lets the app's USB stack bring it all up from scratch.
 */
static void reset_usb_clocks(void)
{
    NRF_USBD->ENABLE = 0;

    NRF_CLOCK->INTENCLR = 0xFFFFFFFFUL;
    NRF_CLOCK->TASKS_HFCLKSTOP = 1;
    NRF_CLOCK->TASKS_LFCLKSTOP = 1;
    NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
    NRF_CLOCK->EVENTS_LFCLKSTARTED = 0;

    NRF_POWER->INTENCLR = 0xFFFFFFFFUL;
    NRF_POWER->EVENTS_USBDETECTED = 0;
    NRF_POWER->EVENTS_USBPWRRDY = 0;
    NRF_POWER->EVENTS_USBREMOVED = 0;
}

static void jump_to_app(uint32_t addr)
{
    /* The app's vector table sits at the start of its flash image: word 0 is
     * the initial MSP, word 1 the reset handler.  Read them through this
     * pointer (held in a register), not into stack locals — after __set_MSP
     * this function's stack is abandoned. */
    const struct arm_vector_table {
        uint32_t msp;
        uint32_t reset;
    } *vt = (const struct arm_vector_table *)(uintptr_t)addr;

    /*
     * Silence every NVIC interrupt and the kernel tick so none fire into the
     * app before it re-initializes (mirrors MCUboot's cleanup).  nRF52 drives
     * the system clock from the RTC — an NVIC peripheral, not the Cortex
     * SysTick — so clearing the NVIC is what actually stops the tick.
     *
     * Deliberately do NOT set PRIMASK: Zephyr on Cortex-M locks interrupts via
     * BASEPRI and never clears PRIMASK at startup, so masking interrupts
     * globally here would hang the app at its first k_msleep().
     */
    for (size_t i = 0; i < ARRAY_SIZE(NVIC->ICER); i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }
    SysTick->CTRL = 0;

#if defined(CONFIG_ARM_MPU)
    /* Disable the MPU: the updater's regions are still active, and they would
     * fault the app during its early init, before it configures its own
     * (this is the step MCUboot performs as z_arm_clear_arm_mpu_config()). */
    MPU->CTRL = 0;
    __DSB();
    __ISB();
#endif

    /* Hand the app clean clock/USB hardware (see reset_usb_clocks). */
    reset_usb_clocks();

    /* Point the vector table at the app and synchronize. */
    SCB->VTOR = addr;
    __DSB();
    __ISB();

    /* Load the app's stack pointer (on the main stack) and branch to its
     * reset handler. */
    __set_MSP(vt->msp);
    __set_CONTROL(0);
    __ISB();

    ((void (*)(void))vt->reset)();

    /* Unreachable. */
    for (;;) {
    }
}

static void boot_main_app(void)
{
    jump_to_app(MAIN_APP_ADDR);
}

/* ------------------------------------------------------------------ */
/* SD card                                                            */
/* ------------------------------------------------------------------ */

static int mount_sd(void)
{
    static const char *disk = "SD";

    if (disk_access_init(disk) != 0) {
        return -EIO;
    }

    g_mount.storage_dev = (void *)disk;
    return fs_mount(&g_mount);
}

/* ------------------------------------------------------------------ */
/* Apply a staged image                                               */
/* ------------------------------------------------------------------ */

/* Stream the payload (starting at file offset FW_IMAGE_HEADER_SIZE) through
 * the CRC, without writing flash.  Returns the final CRC or a negative
 * errno on read error. */
static int64_t file_payload_crc(struct fs_file_t *f, uint32_t len)
{
    int ret = fs_seek(f, FW_IMAGE_HEADER_SIZE, FS_SEEK_SET);
    if (ret) {
        return ret;
    }

    uint32_t crc = FW_CRC32_INIT;
    uint32_t remaining = len;
    while (remaining > 0) {
        size_t want = MIN(remaining, sizeof(g_chunk));
        ssize_t got = fs_read(f, g_chunk, want);
        if (got <= 0) {
            return got < 0 ? got : -EIO;
        }
        crc = fw_crc32_update(crc, g_chunk, got);
        remaining -= got;
    }

    return (int64_t)(uint32_t)fw_crc32_final(crc);
}

/* Erase enough of main_app for the payload, then stream the payload from
 * the file into flash.  The final partial chunk is padded to the write
 * alignment with 0xFF. */
static int flash_payload(const struct flash_area *fa, struct fs_file_t *f,
                         uint32_t len)
{
    uint32_t erase_len = ROUND_UP(len, FLASH_PAGE_SIZE);
    int ret = flash_area_erase(fa, 0, erase_len);
    if (ret) {
        return ret;
    }

    ret = fs_seek(f, FW_IMAGE_HEADER_SIZE, FS_SEEK_SET);
    if (ret) {
        return ret;
    }

    uint32_t off = 0;
    uint32_t remaining = len;
    while (remaining > 0) {
        size_t want = MIN(remaining, sizeof(g_chunk));
        ssize_t got = fs_read(f, g_chunk, want);
        if (got <= 0) {
            return got < 0 ? got : -EIO;
        }

        size_t wlen = ROUND_UP(got, FLASH_WRITE_ALIGN);
        for (size_t i = got; i < wlen; i++) {
            g_chunk[i] = 0xFF;
        }

        ret = flash_area_write(fa, off, g_chunk, wlen);
        if (ret) {
            return ret;
        }

        off += got;
        remaining -= got;
    }

    return 0;
}

/* Re-read the just-written region from flash and CRC it. */
static int64_t flash_region_crc(const struct flash_area *fa, uint32_t len)
{
    uint32_t crc = FW_CRC32_INIT;
    uint32_t off = 0;
    uint32_t remaining = len;
    while (remaining > 0) {
        size_t want = MIN(remaining, sizeof(g_chunk));
        int ret = flash_area_read(fa, off, g_chunk, want);
        if (ret) {
            return ret;
        }
        crc = fw_crc32_update(crc, g_chunk, want);
        off += want;
        remaining -= want;
    }

    return (int64_t)(uint32_t)fw_crc32_final(crc);
}

/*
 * Apply the staged image if present and valid.  Returns:
 *   0       nothing to do (no file / invalid file) — boot the app as-is
 *   1       image applied and verified — boot the new app
 *  <0       erased-then-failed; caller must reboot to retry, NOT jump
 */
static int apply_staged_image(void)
{
    struct fs_file_t f;
    fs_file_t_init(&f);

    if (fs_open(&f, FW_IMAGE_SD_PATH, FS_O_READ) != 0) {
        return 0; /* normal boot — no update staged */
    }

    uint8_t raw[FW_IMAGE_HEADER_SIZE];
    struct fw_image_header hdr;
    ssize_t got = fs_read(&f, raw, sizeof(raw));
    if (got != (ssize_t)sizeof(raw)) {
        fs_close(&f);
        return 0;
    }
    fw_image_header_parse(raw, &hdr);

    const struct flash_area *fa;
    if (flash_area_open(PARTITION_ID(main_app), &fa) != 0) {
        fs_close(&f);
        return 0;
    }

    /* Validate header bounds (magic + length) before reading the payload. */
    if (hdr.magic != FW_IMAGE_MAGIC ||
        hdr.payload_len == 0 || hdr.payload_len > fa->fa_size) {
        flash_area_close(fa);
        fs_close(&f);
        return 0;
    }

    /* Validate the file CRC BEFORE erasing — never destroy the running app
     * for a corrupt or half-uploaded image. */
    int64_t file_crc = file_payload_crc(&f, hdr.payload_len);
    if (file_crc < 0 || (uint32_t)file_crc != hdr.crc32) {
        flash_area_close(fa);
        fs_close(&f);
        return 0;
    }

    /* Commit point: from here the old app may be erased. */
    if (g_led.port) {
        gpio_pin_set_dt(&g_led, 1);
    }

    int ret = flash_payload(fa, &f, hdr.payload_len);
    if (ret == 0) {
        int64_t written_crc = flash_region_crc(fa, hdr.payload_len);
        if (written_crc < 0 || (uint32_t)written_crc != hdr.crc32) {
            ret = -EIO;
        }
    }

    flash_area_close(fa);
    fs_close(&f);

    if (g_led.port) {
        gpio_pin_set_dt(&g_led, 0);
    }

    if (ret) {
        return ret; /* erased but not verified — caller reboots to retry */
    }

    /* Verified: drop the staged file so we don't re-flash on next boot. */
    fs_unlink(FW_IMAGE_SD_PATH);
    return 1;
}

int main(void)
{
    if (g_led.port) {
        gpio_pin_configure_dt(&g_led, GPIO_OUTPUT_INACTIVE);
    }

    if (mount_sd() == 0) {
        int rc = apply_staged_image();
        if (rc < 0) {
            /* Mid-flash failure: the staged file is still on SD and the
             * app is not yet whole.  Reboot to retry from scratch. */
            fs_unmount(&g_mount);
            sys_reboot(SYS_REBOOT_COLD);
        }
        fs_unmount(&g_mount);
    }

    boot_main_app();
    return 0;
}
