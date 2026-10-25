/* Miyoo Mini Plus backend: raw /dev/fb0 + evdev + sysfs GPIO. No SDL, no MI_GFX.
 * Facts from OnionUI sources (src/common/system/{display,rumble,keymap_hw,battery}.h):
 *   fb0 is 640x480 ARGB8888, yres_virtual 960, panel mounted rotated 180 degrees;
 *   MainUI may leave yoffset at 480; rumble = GPIO 48 active-low; screen power = GPIO 4.
 * Audio: OSS /dev/dsp, which Onion routes to its audioserver (the one process holding SigmaStar's MI_AO)
 * through LD_PRELOAD=libpadsp.so; see plat_audio_open. */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform.h"

#define GPIO_RUMBLE "48"
#define GPIO_SCREEN "4"
#define PWM_ENABLE "/sys/devices/soc0/soc/1f003400.pwm/pwm/pwmchip0/pwm0/enable"

static int fb_fd = -1, in_fd = -1;
static uint32_t *fb_mem;
static size_t fb_size;
static struct fb_var_screeninfo vinfo;
static int fb_stride, fb_pages, fb_page;

static void sysfs_write(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    ssize_t n = write(fd, value, strlen(value));
    (void)n;
    close(fd);
}

/* Writing "high"/"low" to direction makes the pin an output with that initial level in one step;
 * plain "out" would drive it low first (a rumble blip, or a backlight flicker). */
static void gpio_out(const char *gpio, const char *level) {
    char path[64];
    snprintf(path, sizeof path, "/sys/class/gpio/gpio%s", gpio);
    struct stat st;
    if (stat(path, &st) != 0) sysfs_write("/sys/class/gpio/export", gpio);
    snprintf(path, sizeof path, "/sys/class/gpio/gpio%s/direction", gpio);
    sysfs_write(path, level);
}

static void gpio_set(const char *gpio, const char *value) {
    char path[64];
    snprintf(path, sizeof path, "/sys/class/gpio/gpio%s/value", gpio);
    sysfs_write(path, value);
}

bool plat_init(void) {
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        perror("open /dev/fb0");
        return false;
    }
    struct fb_fix_screeninfo finfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        perror("fb ioctl");
        return false;
    }
    if (vinfo.xres != SCREEN_W || vinfo.yres != SCREEN_H || vinfo.bits_per_pixel != 32) {
        fprintf(stderr, "unexpected fb mode %ux%u@%u\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);
        return false;
    }
    vinfo.yoffset = 0;
    ioctl(fb_fd, FBIOPUT_VSCREENINFO, &vinfo);
    fb_stride = (int)(finfo.line_length / 4);
    fb_size = finfo.smem_len;
    fb_pages = vinfo.yres_virtual >= 2 * SCREEN_H && fb_size >= (size_t)finfo.line_length * SCREEN_H * 2 ? 2 : 1;
    fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("mmap fb0");
        return false;
    }
    in_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    if (in_fd < 0) perror("open /dev/input/event0");
    gpio_out(GPIO_RUMBLE, "high"); /* active-low: high = off */
    return true;
}

void plat_shutdown(void) {
    if (fb_mem && fb_mem != MAP_FAILED) {
        memset(fb_mem, 0, fb_size);
        vinfo.yoffset = 0;
        ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo);
        munmap(fb_mem, fb_size);
    }
    if (fb_fd >= 0) close(fb_fd);
    if (in_fd >= 0) close(in_fd);
}

void plat_present(const uint32_t *px) {
    /* Draw into the hidden page rotated 180 degrees, then pan to it. */
    int page = fb_pages == 2 ? fb_page ^ 1 : 0;
    uint32_t *base = fb_mem + (size_t)page * SCREEN_H * fb_stride;
    for (int y = 0; y < SCREEN_H; y++) {
        const uint32_t *src = px + y * SCREEN_W;
        uint32_t *dst = base + (size_t)(SCREEN_H - 1 - y) * fb_stride + (SCREEN_W - 1);
        for (int x = 0; x < SCREEN_W; x++) *dst-- = src[x] | 0xff000000u;
    }
    if (fb_pages == 2) {
        vinfo.yoffset = (unsigned)(page * SCREEN_H);
        ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo);
        fb_page = page;
    }
}

static Button map_key(int code) {
    switch (code) {
    case KEY_UP: return PAD_UP;
    case KEY_DOWN: return PAD_DOWN;
    case KEY_LEFT: return PAD_LEFT;
    case KEY_RIGHT: return PAD_RIGHT;
    case KEY_SPACE: return PAD_A;
    case KEY_LEFTCTRL: return PAD_B;
    case KEY_LEFTSHIFT: return PAD_X;
    case KEY_LEFTALT: return PAD_Y;
    case KEY_E: return PAD_L1;
    case KEY_T: return PAD_R1;
    case KEY_TAB: return PAD_L2;
    case KEY_BACKSPACE: return PAD_R2;
    case KEY_RIGHTCTRL: return PAD_SELECT;
    case KEY_ENTER: return PAD_START;
    case KEY_ESC: return PAD_MENU;
    default: return PAD_NONE; /* POWER/volume belong to Onion's keymon */
    }
}

Button plat_poll(void) {
    if (in_fd < 0) return PAD_NONE;
    struct input_event ev;
    while (read(in_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type != EV_KEY || ev.value == 0) continue;
        Button b = map_key(ev.code);
        if (b == PAD_NONE) continue;
        bool repeatable = b == PAD_UP || b == PAD_DOWN || b == PAD_L2 || b == PAD_R2;
        if (ev.value == 2 && !repeatable) continue;
        return b;
    }
    return PAD_NONE;
}

void plat_rumble(bool on) { gpio_set(GPIO_RUMBLE, on ? "0" : "1"); }

void plat_backlight(bool on) {
    gpio_out(GPIO_SCREEN, on ? "high" : "low");
    if (on) {
        sysfs_write(PWM_ENABLE, "0");
        sysfs_write(PWM_ENABLE, "1");
    }
}

int plat_battery(void) {
    FILE *f = fopen("/tmp/percBat", "r");
    if (!f) return -1;
    int pct = -1;
    if (fscanf(f, "%d", &pct) != 1) pct = -1;
    fclose(f);
    return pct;
}

/* ---- AXP223 PMIC (Mini Plus) over I2C ------------------------------------------------------------
 * Same access as OnionUI src/common/system/axp.h: /dev/i2c-1, 7-bit address 0x34, one I2C_RDWR
 * transaction per read (register-address write + 1-byte read, atomic on the bus). Register meanings
 * from the AXP223 datasheet (also Linux drivers/power/supply/axp20x_{usb,ac}_power.c, axp20x_battery.c):
 *   REG 00h Power Input Status: bit7 ACIN present, bit6 ACIN usable, bit5 VBUS present, bit4 VBUS usable.
 *   REG 33h Charge Control 1: bits 6:5 charge target 00 = 4.1 V, 01 = 4.22 V, 10 = 4.2 V (default),
 *   11 = 4.24 V. */
#define AXP_DEV "/dev/i2c-1"
#define AXP_ADDR 0x34
#define AXP_POWER_STATUS 0x00
#define AXP_CHARGE_CTRL1 0x33
#define AXP_EXT_POWER (0x80 | 0x20) /* ACIN or VBUS present */
#define AXP_TARGET_MASK 0x60
#define AXP_TARGET_4V1 0x00
#define AXP_TARGET_4V2 0x40

static int axp_fd = -2; /* -2: not opened yet, -1: no PMIC (original Mini, desktop) */

static int axp_open(void) {
    if (axp_fd == -2) {
        axp_fd = open(AXP_DEV, O_RDWR | O_CLOEXEC);
        if (axp_fd >= 0) {
            ioctl(axp_fd, I2C_TIMEOUT, 5); /* units of 10 ms */
            ioctl(axp_fd, I2C_RETRIES, 1);
        }
    }
    return axp_fd;
}

/* Register value 0..255, or -1. */
static int axp_read(uint8_t reg) {
    if (axp_open() < 0) return -1;
    uint8_t val = 0;
    struct i2c_msg msgs[2] = {
        {.addr = AXP_ADDR, .flags = 0, .len = 1, .buf = &reg},
        {.addr = AXP_ADDR, .flags = I2C_M_RD, .len = 1, .buf = &val},
    };
    struct i2c_rdwr_ioctl_data xfer = {.msgs = msgs, .nmsgs = 2};
    return ioctl(axp_fd, I2C_RDWR, &xfer) < 0 ? -1 : val;
}

static bool axp_write(uint8_t reg, uint8_t val) {
    if (axp_open() < 0) return false;
    uint8_t buf[2] = {reg, val};
    struct i2c_msg msg = {.addr = AXP_ADDR, .flags = 0, .len = 2, .buf = buf};
    struct i2c_rdwr_ioctl_data xfer = {.msgs = &msg, .nmsgs = 1};
    return ioctl(axp_fd, I2C_RDWR, &xfer) >= 0;
}

/* External power, not "charging": with battery care the charger stops at 4.1 V but the cable is
 * still in. The Mini Plus asks the AXP223 (one I2C transaction, well under a millisecond); the
 * original Mini has no PMIC and reports charging on GPIO 59, as in Onion's battery_isCharging(). */
bool plat_on_power(void) {
    int status = axp_read(AXP_POWER_STATUS);
    if (status >= 0) return (status & AXP_EXT_POWER) != 0;
    FILE *f = fopen("/sys/class/gpio/gpio59/value", "r");
    if (!f) return false;
    int c = fgetc(f);
    fclose(f);
    return c == '1';
}

static const char *target_name(int reg) {
    static const char *names[] = {"4.1V", "4.22V", "4.2V", "4.24V"};
    return names[(reg & AXP_TARGET_MASK) >> 5];
}

/* Read-modify-write of REG 33h bits 6:5 only; nothing else on the PMIC is ever written. */
void plat_battery_care(bool on) {
    int old = axp_read(AXP_CHARGE_CTRL1);
    if (old < 0) return; /* no AXP223 */
    int target = old & AXP_TARGET_MASK;
    int want;
    if (on) want = AXP_TARGET_4V1;
    else if (target == AXP_TARGET_4V1) want = AXP_TARGET_4V2; /* undo our own setting only */
    else want = target;
    if (want == target) {
        fprintf(stderr, "battery care: REG33=0x%02x (charge target %s), unchanged\n", old, target_name(old));
        return;
    }
    uint8_t val = (uint8_t)((old & ~AXP_TARGET_MASK) | want);
    int now = axp_write(AXP_CHARGE_CTRL1, val) ? axp_read(AXP_CHARGE_CTRL1) : -1;
    fprintf(stderr, "battery care: REG33 0x%02x (%s) -> 0x%02x, read back 0x%02x (%s)\n", old, target_name(old), val,
            now & 0xff, now >= 0 ? target_name(now) : "write failed");
}

/* ---- backlight level ------------------------------------------------------------------------------
 * Onion's level -> PWM duty curve (src/common/system/display.h display_setBrightness):
 * duty = round(3.0 * exp(0.350656 * level)), level 0..10 (3..100). */
#define PWM_DUTY "/sys/devices/soc0/soc/1f003400.pwm/pwm/pwmchip0/pwm0/duty_cycle"

void plat_brightness(int level) {
    if (level < 0) level = 0;
    if (level > 10) level = 10;
    char buf[16];
    snprintf(buf, sizeof buf, "%d", (int)lround(3.0 * exp(0.350656 * level)));
    sysfs_write(PWM_DUTY, buf);
}

/* Onion's saved brightness ("brightness": 0..10 in /appconfigs/system.json), or -1. */
int plat_brightness_setting(void) {
    FILE *f = fopen("/appconfigs/system.json", "r");
    if (!f) return -1;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *k = strstr(buf, "\"brightness\"");
    int level = -1;
    if (k && sscanf(k + 12, " : %d", &level) != 1) level = -1;
    return level < 0 ? -1 : level > 10 ? 10 : level;
}

/* Onion's keymon skips its idle hibernate while /tmp/stay_awake exists (as GMU and FFplay do);
 * runtime.sh deletes the flag after every app exits. */
void plat_stay_awake(bool on) {
    if (on) close(open("/tmp/stay_awake", O_WRONLY | O_CREAT | O_CLOEXEC, 0644));
    else unlink("/tmp/stay_awake");
}

/* ---- audio -------------------------------------------------------------------------------------
 * Onion keeps SigmaStar's MI_AO owned by one long-lived `audioserver` (started by
 * .tmp_update/runtime.sh start_audioserver before every app) and launches apps with
 * LD_PRELOAD=/mnt/SDCARD/miyoo/lib/libpadsp.so (src/common/utils/apps.h), a shim that hooks open() of
 * /dev/dsp and forwards OSS writes to that server. Onion's own prompt/tweaks play sound this way
 * (stock SDL 1.2 + SDL_mixer at 48 kHz stereo, src/common/utils/sdl_init.h), and so does MinUI on
 * the Plus (skeleton/SYSTEM/miyoomini/paks/MinUI.pak/launch.sh). Onion's RetroArch OSS driver
 * (OnionUI/RetroArch audio/drivers/oss_miyoomini.c) notes the hook covers open() but not open64()
 * or __open(), hence the guard below; it also shows the server path takes 48 kHz S16LE stereo.
 * This requires a dynamically linked binary (a static one never sees the preload): see Containerfile. */
#if defined(_FILE_OFFSET_BITS) && _FILE_OFFSET_BITS == 64 && !defined(__LP64__)
#error "libpadsp hooks open(), not open64(): build plat_miyoo.c without -D_FILE_OFFSET_BITS=64"
#endif

#define DSP_FRAMES 512 /* ~11 ms per write at 48 kHz */

static int dsp_fd = -1;
static pthread_t dsp_thread;
static pthread_mutex_t dsp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dsp_cv = PTHREAD_COND_INITIALIZER;
static bool dsp_wake, dsp_quit;
static AudioRender dsp_render;

static bool dsp_should_quit(void) {
    pthread_mutex_lock(&dsp_mu);
    bool q = dsp_quit;
    pthread_mutex_unlock(&dsp_mu);
    return q;
}

static void dsp_write_all(const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(dsp_fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return; /* server went away: drop this block */
        p += n;
        len -= (size_t)n;
    }
}

/* Sleeps while silent so an idle pager costs no CPU; plays until render() reports silence. */
static void *dsp_main(void *arg) {
    (void)arg;
    static int16_t mono[DSP_FRAMES], stereo[DSP_FRAMES * 2];
    for (;;) {
        pthread_mutex_lock(&dsp_mu);
        while (!dsp_wake && !dsp_quit) pthread_cond_wait(&dsp_cv, &dsp_mu);
        bool quit = dsp_quit;
        dsp_wake = false;
        pthread_mutex_unlock(&dsp_mu);
        if (quit) return NULL;
        bool more;
        do {
            more = dsp_render(mono, DSP_FRAMES);
            for (int i = 0; i < DSP_FRAMES; i++) stereo[2 * i] = stereo[2 * i + 1] = mono[i];
            dsp_write_all(stereo, sizeof stereo);
        } while (more && !dsp_should_quit());
    }
}

bool plat_audio_open(int *rate, AudioRender render) {
    dsp_fd = open("/dev/dsp", O_WRONLY); /* plain open(): the call libpadsp intercepts */
    if (dsp_fd < 0) {
        perror("open /dev/dsp");
        return false;
    }
    int frag = (8 << 16) | 11; /* 8 x 2 KiB fragments; advisory, the server sets its own latency */
    ioctl(dsp_fd, SNDCTL_DSP_SETFRAGMENT, &frag);
    int channels = 2, format = AFMT_S16_LE, speed = 48000;
    if (ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &channels) < 0 || channels != 2 ||
        ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &format) < 0 || format != AFMT_S16_LE ||
        ioctl(dsp_fd, SNDCTL_DSP_SPEED, &speed) < 0 || speed <= 0) {
        perror("/dev/dsp setup");
        close(dsp_fd);
        dsp_fd = -1;
        return false;
    }
    *rate = speed;
    dsp_render = render;
    dsp_wake = dsp_quit = false;
    if (pthread_create(&dsp_thread, NULL, dsp_main, NULL) != 0) {
        close(dsp_fd);
        dsp_fd = -1;
        return false;
    }
    return true;
}

void plat_audio_wake(void) {
    pthread_mutex_lock(&dsp_mu);
    dsp_wake = true;
    pthread_cond_signal(&dsp_cv);
    pthread_mutex_unlock(&dsp_mu);
}

void plat_audio_close(void) {
    if (dsp_fd < 0) return;
    pthread_mutex_lock(&dsp_mu);
    dsp_quit = true;
    pthread_cond_signal(&dsp_cv);
    pthread_mutex_unlock(&dsp_mu);
    pthread_join(dsp_thread, NULL);
    close(dsp_fd);
    dsp_fd = -1;
}
