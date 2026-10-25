#pragma once
/* Tiny chiptune cue synth. Cues are short, soft, and rate-limited; the caller decides
 * whether sound is allowed right now (config, quiet hours). */
#include <stdbool.h>

typedef enum {
    SFX_FINISHED,  /* an agent finished a turn */
    SFX_NEEDS_YOU, /* an agent waits on a permission prompt (the most noticeable cue) */
    SFX_FAILED,    /* an agent errored */
    SFX_NOM,       /* the pet ate tokens */
    SFX_PAT,       /* the user patted the pet */
    SFX_HATCH,     /* the egg hatched */
    SFX_EVOLVE,    /* the pet evolved */
    SFX_UNLOCK,    /* the pet earned a cosmetic (small sparkly jingle) */
    SFX_COUNT
} Sfx;

/* volume 0..10. Returns false (and every call becomes a no-op) if no audio device. */
bool audio_init(int volume);
void audio_play(Sfx s); /* non-blocking, thread-safe; may drop cues to stay unannoying */
void audio_shutdown(void);
