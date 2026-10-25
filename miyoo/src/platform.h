#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SCREEN_W 640
#define SCREEN_H 480

typedef enum {
    PAD_NONE = 0,
    PAD_UP,
    PAD_DOWN,
    PAD_LEFT,
    PAD_RIGHT,
    PAD_A,
    PAD_B,
    PAD_X,
    PAD_Y,
    PAD_L1,
    PAD_R1,
    PAD_L2,
    PAD_R2,
    PAD_SELECT,
    PAD_START,
    PAD_MENU,
    PAD_QUIT, /* window closed / SIGTERM */
} Button;

bool plat_init(void);
void plat_shutdown(void);
/* Pixels are 0x00RRGGBB, SCREEN_W x SCREEN_H, row-major, top-left origin. */
void plat_present(const uint32_t *px);
/* Next pressed (or auto-repeated) button, BTN_NONE when nothing is queued. */
Button plat_poll(void);
void plat_rumble(bool on);
void plat_backlight(bool on);
/* Battery percent (Onion's /tmp/percBat), -1 if unknown. */
int plat_battery(void);
/* True while external power (USB) is present, whether or not the battery is still charging.
 * Cheap (one PMIC register read on the Mini Plus), but poll it every few seconds, not per frame. */
bool plat_on_power(void);
/* Battery care: on = charge target 4.1 V; off = back to 4.2 V if the target is our 4.1 V. Logs to stderr. */
void plat_battery_care(bool on);
/* Backlight level on Onion's 0..10 scale (the Miyoo writes the PWM duty; the desktop prints it). */
void plat_brightness(int level);
/* The user's own level from Onion's settings (0..10), -1 if unknown. */
int plat_brightness_setting(void);
/* Asks the OS not to idle-sleep while true (Onion keymon's /tmp/stay_awake flag). */
void plat_stay_awake(bool on);

/* Audio out: mono signed 16-bit PCM. *rate is the requested frame rate on input, the real one on
 * output. render() runs on the backend's audio thread; it returns false once it has nothing left to
 * play, after which a backend may stop calling it until plat_audio_wake(). */
typedef bool (*AudioRender)(int16_t *out, int frames);
bool plat_audio_open(int *rate, AudioRender render);
void plat_audio_wake(void); /* new sound queued; safe from any thread */
void plat_audio_close(void); /* stops and joins the audio thread */
