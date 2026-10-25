/* Desktop backend for development. Keys mirror the Miyoo keyboard codes, plus Z=A and X=B. */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static bool lit = true;

bool plat_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    const char *scale_env = getenv("PAGER_SCALE");
    int scale = scale_env ? atoi(scale_env) : 1;
    if (scale < 1) scale = 1;
    win = SDL_CreateWindow("Paseo Pager", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_W * scale,
                           SCREEN_H * scale, 0);
    ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (!ren) ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE) : NULL;
    tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H) : NULL;
    if (!tex) {
        fprintf(stderr, "SDL setup: %s\n", SDL_GetError());
        return false;
    }
    SDL_RenderSetLogicalSize(ren, SCREEN_W, SCREEN_H);
    return true;
}

void plat_shutdown(void) {
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
}

void plat_present(const uint32_t *px) {
    SDL_UpdateTexture(tex, NULL, px, SCREEN_W * 4);
    SDL_RenderClear(ren);
    if (lit) SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

static Button map_key(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: return PAD_UP;
    case SDLK_DOWN: return PAD_DOWN;
    case SDLK_LEFT: return PAD_LEFT;
    case SDLK_RIGHT: return PAD_RIGHT;
    case SDLK_SPACE: case SDLK_z: return PAD_A;
    case SDLK_LCTRL: case SDLK_x: return PAD_B;
    case SDLK_LSHIFT: case SDLK_a: return PAD_X;
    case SDLK_LALT: case SDLK_s: return PAD_Y;
    case SDLK_e: return PAD_L1;
    case SDLK_t: return PAD_R1;
    case SDLK_TAB: return PAD_L2;
    case SDLK_BACKSPACE: return PAD_R2;
    case SDLK_RCTRL: return PAD_SELECT;
    case SDLK_RETURN: return PAD_START;
    case SDLK_ESCAPE: return PAD_MENU;
    default: return PAD_NONE;
    }
}

Button plat_poll(void) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) return PAD_QUIT;
        if (ev.type == SDL_KEYDOWN) {
            Button b = map_key(ev.key.keysym.sym);
            if (b != PAD_NONE) return b;
        }
    }
    return PAD_NONE;
}

void plat_rumble(bool on) {
    if (on) fprintf(stderr, "bzzt\n");
}

void plat_backlight(bool on) {
    lit = on;
    SDL_SetWindowTitle(win, on ? "Paseo Pager" : "Paseo Pager (screen off)");
}

int plat_battery(void) { return -1; }

/* Desktop: set PAGER_CHARGING=1 to simulate USB power. */
bool plat_on_power(void) { return getenv("PAGER_CHARGING") != NULL; }

void plat_battery_care(bool on) { fprintf(stderr, "battery care: %s (no PMIC on desktop)\n", on ? "on" : "off"); }

void plat_brightness(int level) { fprintf(stderr, "brightness %d\n", level); }

/* Desktop: pretend Onion's default level. */
int plat_brightness_setting(void) { return 7; }

void plat_stay_awake(bool on) { fprintf(stderr, "stay awake: %d\n", on); }

/* Audio: SDL's own callback thread. Initialised separately from video so a machine without a sound
 * device still gets a window. */
static SDL_AudioDeviceID audio_dev;
static AudioRender audio_render;

static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    audio_render((int16_t *)stream, len / (int)sizeof(int16_t));
}

bool plat_audio_open(int *rate, AudioRender render) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL audio: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want = {0}, have;
    want.freq = *rate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = audio_cb;
    audio_render = render;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!audio_dev) {
        fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    *rate = have.freq;
    SDL_PauseAudioDevice(audio_dev, 0);
    return true;
}

void plat_audio_wake(void) {}

void plat_audio_close(void) {
    if (!audio_dev) return;
    SDL_CloseAudioDevice(audio_dev);
    audio_dev = 0;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool plat_wifi_up(void) { return getenv("PAGER_NO_WIFI") == NULL; }
