#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_BRIDGES 4

typedef enum { LINK_CONNECTING, LINK_OK, LINK_ERROR } LinkState;

typedef struct {
    char host[64];
    int port;
    char token[96];

    /* Shared with the worker thread; guarded by mu. */
    pthread_mutex_t mu;
    char *body;   /* latest successful response body (malloc'd) */
    uint64_t gen; /* bumps on every new body */
    LinkState state;
    char error[64];
} Bridge;

typedef struct {
    int n_bridges;
    Bridge bridges[MAX_BRIDGES];
    char pet_name[16];
    bool rumble;
    int screen_timeout_s; /* 0 = never turn the backlight off */
    bool awake_on_power;  /* keep the screen on while charging */
    int volume;           /* 0..10 sound cues; 0 = silent */
    bool quiet_nights;    /* no sound 23:00-07:00 bridge time */
    int night_dim;        /* backlight level 0..10 for 23:00-07:00 while on power; -1 = off */
    bool battery_care;    /* charge to 4.1 V instead of 4.2 V (Mini Plus) */
} Config;

/* Loads key = value config. Returns false with a message in err on failure. */
bool config_load(const char *path, Config *cfg, char *err, size_t errlen);

/* Starts one detached long-poll thread per bridge. */
void net_start(Config *cfg);

/* Returns a malloc'd copy of the newest body if it changed since *gen_seen (NULL otherwise),
 * and always reports the current link state. */
char *bridge_take(Bridge *b, uint64_t *gen_seen, LinkState *state, char *err, size_t errlen);

/* Blocking single fetch (rev=-1, no wait); used by the --shot smoke mode. */
bool bridge_fetch_once(Bridge *b, char **body_out, char *err, size_t errlen);
