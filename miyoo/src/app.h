#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "feed.h"
#include "gfx.h"
#include "net.h"
#include "pet.h"

/* L/R cycle AGENTS -> DEN -> PET; DETAIL is entered from AGENTS. */
typedef enum { SCREEN_AGENTS, SCREEN_DEN, SCREEN_PET, SCREEN_DETAIL } Screen;

/* Things that happened, produced by main.c's apply_feed/pat handling; consumed by the scene
 * (animations) and the sound cues. */
typedef enum {
    EV_TOKENS,    /* an agent produced `amount` new output tokens */
    EV_FINISHED,  /* an agent's running turn ended */
    EV_NEEDS_YOU, /* an agent started waiting on a permission prompt */
    EV_FAILED,    /* an agent errored */
    EV_HATCHED,   /* the egg hatched */
    EV_EVOLVED,   /* the pet reached a new stage */
    EV_PAT,       /* the user patted the pet */
    EV_UNLOCKED,  /* the pet earned a cosmetic; amount = unlock id (pet.h) */
} EventKind;

typedef struct {
    EventKind kind;
    int bridge;       /* -1 for pet events */
    char agent_id[8]; /* empty for pet events */
    Family family;
    int64_t amount;
    int64_t at_ms; /* monotonic ms */
} Event;

#define EVENT_RING 64
#define RATE_MINUTES 30

typedef struct {
    int bridge;
    Agent agent;
    bool alert; /* unacknowledged: finished, failed, or waiting on you */
} Row;

typedef struct {
    int bridge;
    char id[8];
    Bucket bucket;
    int64_t out_tokens; /* last seen, for per-agent EV_TOKENS deltas */
    bool alert;
    bool present;
} Track;

#define MAX_ROWS (MAX_BRIDGES * MAX_AGENTS)

typedef struct {
    Config cfg;
    char pet_path[512];

    Feed feeds[MAX_BRIDGES];
    bool have_feed[MAX_BRIDGES];
    LinkState link[MAX_BRIDGES];
    char link_err[MAX_BRIDGES][64];
    uint64_t gen[MAX_BRIDGES];

    int64_t clock_offset; /* bridge time - device time */
    bool clock_synced;
    int32_t tz_offset;

    Track track[MAX_ROWS];
    int n_track;
    Row rows[MAX_ROWS];
    int n_rows;
    int n_alerts, n_running;

    Screen screen;
    int sel, scroll;
    char detail_key[8]; /* agent id shown on the detail screen */
    int detail_bridge;

    bool pet_ready;
    Pet pet;
    int64_t last_save;
    Pet saved_pet; /* copy at the last save; pet_save_needed(&pet, &saved_pet) decides writes */

    /* Wardrobe on the Pet screen (pet_view.c owns the behaviour). */
    bool wardrobe_open;
    int wardrobe_slot, wardrobe_sel;

    /* Forms browser on the Pet screen (evolution_view.c owns the behaviour): the open flag and the
     * previewed form 0..PET_FORMS-1. Browsing never changes the pet; only A on a ready form does. */
    bool evolution_open;
    int evolution_sel;

    /* DNA Rewrite overlay of the forms browser (rebirth_view.c owns the behaviour). rebirth_page:
     * 0 closed, 1 hub, 2 trait preview, 3 confirmation, 4 lineage album. The candidate next life
     * and the album's one archived life are cached here on input, never per frame; neither is ever
     * written into `pet`, which only pet_rewrite_commit (Y on the confirmation) replaces. */
    int rebirth_page;
    int rebirth_trait; /* trait field 0..6 (body..motion) the next life inherits */
    bool rebirth_preview_ok;
    Pet rebirth_preview;         /* pet_rewrite_preview(pet, rebirth_trait), valid if rebirth_preview_ok */
    char rebirth_error[128];     /* last failed commit, shown on the confirmation page */
    uint32_t rebirth_album_gen;  /* past life shown, 1..generation-1; 0 = none loaded */
    int rebirth_album_form;      /* previewed form of that life, 0..its forms available-1 */
    bool rebirth_album_ok;       /* rebirth_album holds rebirth_album_gen's archive */
    Pet rebirth_album;           /* read only: never equipped onto or merged into `pet` */
    char rebirth_album_err[128]; /* why rebirth_album_gen could not be shown */

    /* Today across all bridges (bridge-local day), for the den's ATM. Computed by main.c from
     * each Feed's `today` line; only bridges reporting the newest day count. */
    int64_t day_tokens;
    int64_t day_cost_mc; /* millicents (1/1000 of a cent) */
    bool night_dim;      /* screen dimmed for the night (main.c); scenes may calm down */

    char bubble[64];
    int64_t bubble_until_ms;
    int64_t alert_flash_until_ms;
    int battery;   /* plat_battery() value */
    bool charging; /* on USB power (and awake_on_power set): screen stays on */
    /* No bridge reachable for OFFLINE_AFTER_MS: OFFLINE_WIFI if the Miyoo's own Wi-Fi is down,
     * OFFLINE_BRIDGES if Wi-Fi is up but every bridge fails. ui.c shows a banner. */
    enum { ONLINE, OFFLINE_BRIDGES, OFFLINE_WIFI } offline;
    /* Subscription limits: copied from the newest Feed with usage_at > 0 (one bridge runs omp usage). */
    int64_t usage_at;
    int n_usage;
    UsageRow usage[MAX_USAGE];

    /* events[i % EVENT_RING] for i in [max(0, n_events - EVENT_RING), n_events) */
    Event events[EVENT_RING];
    uint64_t n_events;
    /* Output tokens eaten per bridge-clock minute; rate[RATE_MINUTES - 1] is the current minute
     * (index rate_minute = unix_minutes). Older minutes shift left as time passes. */
    int64_t rate[RATE_MINUTES];
    int64_t rate_minute;
} App;

typedef struct Scene Scene;

int64_t app_now(const App *app);
void ui_draw(const App *app, const Scene *scene, Canvas *c, int64_t now_ms);
