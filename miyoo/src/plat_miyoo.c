/* Miyoo Mini Plus backend: raw /dev/fb0 + evdev + sysfs GPIO. No SDL, no MI_GFX.
 * Facts from OnionUI sources (src/common/system/{display,rumble,keymap_hw,battery}.h):
 *   fb0 is 640x480 ARGB8888, yres_virtual 960, panel mounted rotated 180 degrees;
 *   MainUI may leave yoffset at 480; rumble = GPIO 48 active-low; screen power = GPIO 4.
 * Audio: OSS /dev/dsp, which Onion routes to its audioserver (the one process holding SigmaStar's MI_AO)
 * through LD_PRELOAD=libpadsp.so; see plat_audio_open. */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
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

/* Mirrors Onion's battery_isCharging() (src/common/system/battery.h): the Mini Plus asks the AXP223
 * PMIC through the stock axp_test tool ("charging":3 = on USB power); the original Mini reads GPIO 59. */
static bool probe_charging(void) {
    if (access("/customer/app/axp_test", X_OK) == 0) {
        FILE *p = popen("cd /customer/app && ./axp_test 2>/dev/null", "r");
        if (!p) return false;
        char buf[128] = {0};
        int charging = 0;
        if (fgets(buf, sizeof buf, p)) {
            const char *k = strstr(buf, "\"charging\":");
            if (k) charging = atoi(k + 11);
        }
        pclose(p);
        return charging == 3;
    }
    FILE *f = fopen("/sys/class/gpio/gpio59/value", "r");
    if (!f) return false;
    int c = fgetc(f);
    fclose(f);
    return c == '1';
}

/* popen + a PMIC query can take a while, so it runs on its own thread (every 15 s) and the UI
 * loop only reads the cached answer. */
static volatile int charging_cached;
static pthread_once_t charging_once = PTHREAD_ONCE_INIT;

static void *charging_main(void *arg) {
    (void)arg;
    for (;;) {
        __atomic_store_n(&charging_cached, probe_charging(), __ATOMIC_RELAXED);
        sleep(15);
    }
    return NULL;
}

static void charging_start(void) {
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    pthread_create(&t, &attr, charging_main, NULL);
    pthread_attr_destroy(&attr);
}

bool plat_charging(void) {
    pthread_once(&charging_once, charging_start);
    return __atomic_load_n(&charging_cached, __ATOMIC_RELAXED) != 0;
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
