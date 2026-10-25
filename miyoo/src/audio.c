/* Tiny chiptune cue synth: band-limited square (any duty), triangle and LFSR noise, each note with a
 * raised-cosine attack/release so nothing clicks. Mono float mix -> gentle low-pass -> soft limiter ->
 * int16, rendered on the platform's audio thread (plat_audio_open). All state sits behind one mutex. */
#include "audio.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "platform.h"

#define AUDIO_RATE 48000 /* Onion's audioserver runs MI_AO at 48 kHz stereo; ask for that to skip resampling */
#define MAX_NOTES 12
#define SLOTS 3          /* the playing cue plus cues fading out after being preempted */
#define PREEMPT_MS 15.0f /* fade-out of a preempted cue */
#define LOWPASS_HZ 6500.0f
#define HEADROOM_DB -6.0f /* trims the cue tables so volume 10 stays under the soft limiter */
#define PI_F 3.14159265f

enum { SQ, TRI, NOISE };

typedef struct {
    uint16_t at, gate;   /* ms from cue start; gate = time until release begins */
    uint8_t wave, note;  /* note: MIDI number (69 = A4 = 440 Hz) */
    int8_t bend;         /* semitones glided over the gate */
    uint8_t duty;        /* square duty, percent */
    uint8_t level;       /* percent */
    uint8_t attack;      /* ms, raised cosine */
    uint8_t decay;       /* ms, linear down to sustain */
    uint8_t sustain;     /* percent of level */
    uint16_t release;    /* ms, raised cosine to silence */
} Note;

/* Positional so every field is explicit:        at  gate wave note bend duty lvl atk dec sus rel */
#define NT(at, gate, wave, note, bend, duty, lvl, atk, dec, sus, rel) \
    {at, gate, wave, note, bend, duty, lvl, atk, dec, sus, rel}

typedef struct {
    const Note *notes;
    int n;
    float gain_db; /* cue loudness relative to the loudest (NEEDS_YOU) */
    int prio;      /* while a cue plays, cues of equal/lower priority are dropped, higher ones preempt */
    int gap_ms;    /* the same cue at most once per gap */
} Cue;

/* Soft rising chime G5-C6-E6: triangle body, a whisper of 12.5% square on top for sparkle. */
static const Note FINISHED_NOTES[] = {
    NT(0, 110, TRI, 79, 0, 50, 70, 8, 60, 55, 220),
    NT(95, 110, TRI, 84, 0, 50, 70, 8, 60, 55, 240),
    NT(190, 160, TRI, 88, 0, 50, 75, 8, 80, 50, 420),
    NT(190, 120, SQ, 88, 0, 12, 10, 10, 60, 40, 380),
};

/* "Hey, over here?" E5-G5-C6, played twice; 25% square doubled an octave down by a triangle. */
static const Note NEEDS_YOU_NOTES[] = {
    NT(0, 80, SQ, 76, 0, 25, 30, 6, 40, 60, 140),
    NT(0, 80, TRI, 64, 0, 50, 45, 6, 40, 60, 140),
    NT(115, 80, SQ, 79, 0, 25, 30, 6, 40, 60, 140),
    NT(115, 80, TRI, 67, 0, 50, 45, 6, 40, 60, 140),
    NT(230, 120, SQ, 84, 0, 25, 34, 6, 50, 60, 220),
    NT(230, 120, TRI, 72, 0, 50, 50, 6, 50, 60, 220),
    NT(560, 80, SQ, 76, 0, 25, 24, 6, 40, 60, 140),
    NT(675, 80, SQ, 79, 0, 25, 24, 6, 40, 60, 140),
    NT(790, 120, SQ, 84, 0, 25, 28, 6, 50, 60, 260),
    NT(790, 120, TRI, 72, 0, 50, 40, 6, 50, 60, 260),
};

/* Soft descending minor third Eb5 -> C5, the second note sagging a semitone ("aww"). */
static const Note FAILED_NOTES[] = {
    NT(0, 150, TRI, 75, 0, 50, 80, 10, 80, 60, 160),
    NT(0, 150, SQ, 75, 0, 50, 12, 10, 80, 50, 160),
    NT(210, 230, TRI, 72, -1, 50, 85, 10, 120, 55, 330),
    NT(210, 230, SQ, 72, -1, 50, 12, 10, 120, 45, 330),
};

/* Two tiny "nom" blips: a short downward square chirp with a breath of noise. */
static const Note NOM_NOTES[] = {
    NT(0, 30, SQ, 69, -7, 50, 60, 4, 20, 50, 40),
    NT(0, 14, NOISE, 96, 0, 50, 22, 4, 10, 40, 20),
    NT(115, 30, SQ, 66, -7, 50, 55, 4, 20, 50, 45),
    NT(115, 14, NOISE, 96, 0, 50, 20, 4, 10, 40, 20),
};

/* Cute two-part chirp sliding up. */
static const Note PAT_NOTES[] = {
    NT(0, 60, TRI, 84, 7, 50, 75, 5, 30, 70, 60),
    NT(0, 60, SQ, 84, 7, 25, 14, 5, 30, 60, 60),
    NT(95, 55, TRI, 91, 5, 50, 65, 5, 30, 70, 90),
};

/* Shell cracks twice, then a C-major arpeggio landing on a held C6+E6. */
static const Note HATCH_NOTES[] = {
    NT(0, 18, NOISE, 100, 0, 50, 40, 4, 12, 30, 30),
    NT(150, 18, NOISE, 103, 0, 50, 45, 4, 12, 30, 30),
    NT(300, 60, SQ, 72, 0, 25, 30, 6, 40, 60, 80),
    NT(380, 60, SQ, 76, 0, 25, 30, 6, 40, 60, 80),
    NT(460, 60, SQ, 79, 0, 25, 30, 6, 40, 60, 80),
    NT(540, 260, TRI, 84, 0, 50, 60, 8, 120, 55, 300),
    NT(540, 260, TRI, 88, 0, 50, 45, 8, 120, 55, 300),
    NT(540, 220, SQ, 84, 0, 12, 14, 8, 100, 45, 280),
    NT(540, 260, TRI, 60, 0, 50, 45, 10, 120, 60, 280),
};

/* Rising shimmer sweep into a bright G-major hit. */
static const Note EVOLVE_NOTES[] = {
    NT(0, 420, SQ, 60, 24, 25, 16, 60, 10, 100, 40),
    NT(0, 420, TRI, 48, 24, 50, 30, 60, 10, 100, 40),
    NT(470, 260, TRI, 79, 0, 50, 50, 8, 120, 55, 380),
    NT(470, 260, TRI, 83, 0, 50, 40, 8, 120, 55, 380),
    NT(470, 260, TRI, 86, 0, 50, 40, 8, 120, 55, 380),
    NT(470, 200, SQ, 91, 0, 12, 12, 8, 100, 40, 360),
    NT(470, 260, TRI, 55, 0, 50, 40, 10, 120, 60, 360),
};

/* A little sparkle: quick C-major run up to C7, then two faint high twinkles (E7, G7). */
static const Note UNLOCK_NOTES[] = {
    NT(0, 45, TRI, 84, 0, 50, 55, 4, 30, 50, 90),
    NT(65, 45, TRI, 88, 0, 50, 55, 4, 30, 50, 90),
    NT(130, 45, TRI, 91, 0, 50, 55, 4, 30, 50, 90),
    NT(195, 140, TRI, 96, 0, 50, 60, 5, 60, 50, 300),
    NT(195, 110, SQ, 96, 0, 12, 8, 6, 50, 40, 260),
    NT(320, 40, TRI, 100, 0, 50, 28, 3, 20, 40, 130),
    NT(410, 40, TRI, 103, 0, 50, 22, 3, 20, 40, 170),
};

#define CUE(arr, db, prio, gap) {arr, (int)(sizeof arr / sizeof arr[0]), db, prio, gap}
static const Cue CUES[SFX_COUNT] = {
    [SFX_FINISHED] = CUE(FINISHED_NOTES, -3.0f, 3, 4000),
    [SFX_NEEDS_YOU] = CUE(NEEDS_YOU_NOTES, 0.0f, 6, 4000),
    [SFX_FAILED] = CUE(FAILED_NOTES, -3.0f, 5, 4000),
    [SFX_NOM] = CUE(NOM_NOTES, -12.0f, 0, 20000),
    [SFX_PAT] = CUE(PAT_NOTES, -6.0f, 1, 1500),
    [SFX_UNLOCK] = CUE(UNLOCK_NOTES, -6.0f, 2, 4000),
    [SFX_HATCH] = CUE(HATCH_NOTES, -3.0f, 4, 4000),
    [SFX_EVOLVE] = CUE(EVOLVE_NOTES, -3.0f, 4, 4000),
};

typedef struct {
    const Cue *cue;
    long t, len;           /* samples since start, total samples */
    float gain, fade, fade_step;
    float f0[MAX_NOTES], phase[MAX_NOTES], noise[MAX_NOTES];
    uint16_t lfsr[MAX_NOTES];
} Slot;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static bool opened;
static int rate = AUDIO_RATE;
static float master, lp_a, lp_y;
static Slot slots[SLOTS];
static int cur = -1;              /* slot of the cue that "is playing" (not fading) */
static int64_t last_ms[SFX_COUNT];

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static float ms_f(int ms, int floor_ms) { return (float)(ms < floor_ms ? floor_ms : ms) / 1000.0f; }

/* Envelope before release: raised-cosine attack, linear decay, flat sustain. */
static float env_gate(const Note *n, float t) {
    float a = ms_f(n->attack, 3), d = ms_f(n->decay, 1), s = n->sustain / 100.0f;
    if (t < a) return 0.5f - 0.5f * cosf(PI_F * t / a);
    if (t < a + d) return 1.0f - (1.0f - s) * (t - a) / d;
    return s;
}

static float env(const Note *n, float t) {
    float g = ms_f(n->gate, 0), r = ms_f(n->release, 4);
    if (t < 0.0f || t >= g + r) return 0.0f;
    if (t < g) return env_gate(n, t);
    return env_gate(n, g) * (0.5f + 0.5f * cosf(PI_F * (t - g) / r));
}

static float polyblep(float p, float dt) {
    if (p < dt) {
        p /= dt;
        return p + p - p * p - 1.0f;
    }
    if (p > 1.0f - dt) {
        p = (p - 1.0f) / dt;
        return p * p + p + p + 1.0f;
    }
    return 0.0f;
}

static float slot_sample(Slot *s) {
    const Cue *c = s->cue;
    float sec = (float)s->t / (float)rate, sum = 0.0f;
    int count = c->n < MAX_NOTES ? c->n : MAX_NOTES;
    for (int j = 0; j < count; j++) {
        const Note *n = &c->notes[j];
        float tn = sec - n->at / 1000.0f;
        float e = env(n, tn);
        if (e <= 0.0f) continue;
        float f = s->f0[j];
        if (n->bend) {
            float g = ms_f(n->gate, 1), k = tn < g ? tn / g : 1.0f;
            f *= exp2f(n->bend * k / 12.0f);
        }
        float dt = f / (float)rate, p = s->phase[j], v;
        if (n->wave == SQ) {
            float d = n->duty / 100.0f;
            v = (p < d ? 1.0f : -1.0f) + polyblep(p, dt) - polyblep(fmodf(p + 1.0f - d, 1.0f), dt);
            v -= 2.0f * d - 1.0f; /* zero-mean, so narrow duties don't thump */
        } else if (n->wave == TRI) {
            v = 4.0f * fabsf(p - 0.5f) - 1.0f;
        } else {
            v = s->noise[j];
        }
        p += n->wave == NOISE ? dt * 8.0f : dt; /* noise: LFSR clocked 8x the note */
        if (p >= 1.0f) {
            p -= floorf(p);
            if (n->wave == NOISE) {
                uint16_t l = s->lfsr[j];
                l = (uint16_t)((l >> 1) | (((l ^ (l >> 1)) & 1u) << 14));
                s->lfsr[j] = l;
                s->noise[j] = (l & 1u) ? 1.0f : -1.0f;
            }
        }
        s->phase[j] = p;
        sum += v * e * (n->level / 100.0f);
    }
    sum *= s->gain * s->fade;
    s->t++;
    if (s->fade_step > 0.0f) s->fade -= s->fade_step;
    if (s->t >= s->len || s->fade <= 0.0f) s->cue = NULL;
    return sum;
}

static bool render(int16_t *out, int frames) {
    pthread_mutex_lock(&mu);
    bool live = false;
    for (int i = 0; i < SLOTS; i++) live |= slots[i].cue != NULL;
    if (!live && fabsf(lp_y) < 1e-5f) {
        lp_y = 0.0f;
        cur = -1;
        pthread_mutex_unlock(&mu);
        memset(out, 0, (size_t)frames * sizeof *out);
        return false;
    }
    for (int i = 0; i < frames; i++) {
        float x = 0.0f;
        for (int k = 0; k < SLOTS; k++)
            if (slots[k].cue) x += slot_sample(&slots[k]);
        lp_y += lp_a * (x * master - lp_y);
        float y = lp_y, m = fabsf(y);
        if (m > 0.8f) y = copysignf(0.8f + 0.2f * tanhf((m - 0.8f) / 0.2f), y); /* soft limit, never wraps */
        out[i] = (int16_t)lrintf(y * 32767.0f);
    }
    if (cur >= 0 && !slots[cur].cue) cur = -1;
    pthread_mutex_unlock(&mu);
    return true;
}

bool audio_init(int volume) {
    if (opened) audio_shutdown();
    int r = AUDIO_RATE;
    if (!plat_audio_open(&r, render)) return false;
    pthread_mutex_lock(&mu);
    rate = r > 0 ? r : AUDIO_RATE;
    if (volume < 0) volume = 0;
    if (volume > 10) volume = 10;
    /* 2 dB per step (perceptually even): 10 = full (loudest cue peaks near -4 dBFS), 4 (the default) = -12 dB. */
    master = volume ? powf(10.0f, (HEADROOM_DB - (10 - volume) * 2.0f) / 20.0f) : 0.0f;
    lp_a = 1.0f - expf(-2.0f * PI_F * LOWPASS_HZ / (float)rate);
    lp_y = 0.0f;
    memset(slots, 0, sizeof slots);
    cur = -1;
    for (int i = 0; i < SFX_COUNT; i++) last_ms[i] = INT64_MIN / 2;
    opened = true;
    pthread_mutex_unlock(&mu);
    return true;
}

void audio_play(Sfx sfx) {
    if ((unsigned)sfx >= SFX_COUNT) return;
    const Cue *c = &CUES[sfx];
    pthread_mutex_lock(&mu);
    int64_t now = now_ms();
    bool ok = opened && master > 0.0f && now - last_ms[sfx] >= c->gap_ms;
    if (ok && cur >= 0 && slots[cur].cue && c->prio <= slots[cur].cue->prio) ok = false;
    if (ok) {
        last_ms[sfx] = now;
        if (cur >= 0 && slots[cur].cue) slots[cur].fade_step = 1.0f / (PREEMPT_MS / 1000.0f * (float)rate);
        int pick = 0; /* a free slot, else the quietest fading one */
        for (int i = 0; i < SLOTS; i++) {
            if (!slots[i].cue) {
                pick = i;
                break;
            }
            if (slots[i].fade < slots[pick].fade) pick = i;
        }
        Slot *s = &slots[pick];
        memset(s, 0, sizeof *s);
        s->cue = c;
        s->fade = 1.0f;
        s->gain = powf(10.0f, c->gain_db / 20.0f);
        int end_ms = 0;
        for (int j = 0; j < c->n && j < MAX_NOTES; j++) {
            const Note *n = &c->notes[j];
            int e = n->at + n->gate + (n->release < 4 ? 4 : n->release);
            if (e > end_ms) end_ms = e;
            s->f0[j] = 440.0f * exp2f((n->note - 69) / 12.0f);
            s->lfsr[j] = (uint16_t)(0x4a3d + 977 * j) & 0x7fff;
            s->noise[j] = 1.0f;
        }
        s->len = (long)end_ms * rate / 1000 + 1;
        cur = pick;
    }
    pthread_mutex_unlock(&mu);
    if (ok) plat_audio_wake();
}

void audio_shutdown(void) {
    pthread_mutex_lock(&mu);
    bool was = opened;
    opened = false;
    pthread_mutex_unlock(&mu);
    if (was) plat_audio_close(); /* joins the audio thread; render may still run until then */
    pthread_mutex_lock(&mu);
    memset(slots, 0, sizeof slots);
    cur = -1;
    pthread_mutex_unlock(&mu);
}
