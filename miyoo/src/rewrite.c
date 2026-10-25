#include "rewrite.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* pet_save() stages "<path>.tmp" next to every file it writes. */
#define STAGE_SUFFIX_LEN 4

static bool fail(char *error, size_t error_len, const char *fmt, ...) {
    if (error && error_len) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(error, error_len, fmt, ap);
        va_end(ap);
    }
    return false;
}

/* True if `path` (and the staging name pet_save() derives from it) fits REWRITE_PATH_MAX. */
static bool path_fits(const char *path) { return strlen(path) + STAGE_SUFFIX_LEN < REWRITE_PATH_MAX; }

static bool life_path(char *out, const char *pet_path, uint32_t generation) {
    int n = snprintf(out, REWRITE_PATH_MAX, "%s.life-%" PRIu32, pet_path, generation);
    return n > 0 && (size_t)n + STAGE_SUFFIX_LEN < REWRITE_PATH_MAX;
}

/* Directory holding `pet_path`: "." for a bare name, "/" for a file in the root. False if the path
 * names no file (empty or ending in '/'). */
static bool dir_of(char *out, const char *pet_path) {
    size_t len = strlen(pet_path);
    if (!len || pet_path[len - 1] == '/' || len >= REWRITE_PATH_MAX) return false;
    const char *slash = strrchr(pet_path, '/');
    if (!slash) {
        memcpy(out, ".", 2);
        return true;
    }
    size_t dir_len = slash == pet_path ? 1 : (size_t)(slash - pet_path);
    memcpy(out, pet_path, dir_len);
    out[dir_len] = 0;
    return true;
}

/* Makes renames inside `dir` durable. On failure errno tells why. */
static bool sync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    if (fsync(fd) != 0) {
        int why = errno;
        close(fd);
        errno = why;
        return false;
    }
    return close(fd) == 0;
}

/* Keys an archive must state explicitly: pet_load() silently defaults or migrates any of them. */
static const char *const REQUIRED_KEYS[] = {"name",      "born",     "xp",         "stage",   "adult_family",
                                            "diet",      "unlocked", "equipped",   "dna",     "form",
                                            "life_born", "life_xp",  "life_diet",  "generation"};
#define N_REQUIRED (sizeof REQUIRED_KEYS / sizeof *REQUIRED_KEYS)
_Static_assert(N_REQUIRED <= 32, "required keys are tracked in a 32-bit mask");

static bool parse_u32(const char *v, uint32_t *out) {
    if (*v < '0' || *v > '9') return false;
    errno = 0;
    char *end;
    unsigned long long n = strtoull(v, &end, 10);
    if (errno || *end || n > UINT32_MAX) return false;
    *out = (uint32_t)n;
    return true;
}

static bool parse_hex64(const char *v, uint64_t *out) {
    if (!*v || !strchr("0123456789abcdefABCDEF", *v)) return false;
    errno = 0;
    char *end;
    unsigned long long n = strtoull(v, &end, 16);
    if (errno || *end) return false;
    *out = (uint64_t)n;
    return true;
}

/* Checks the raw archive text before pet_load() reads it: complete newline-terminated lines no
 * longer than pet_load() handles, every required key exactly once, and the stated generation and
 * nonzero DNA. */
static bool scan_archive(const char *path, uint32_t generation, uint64_t *dna, char *error, size_t error_len) {
    FILE *f = fopen(path, "r");
    if (!f) return fail(error, error_len, "cannot open %s: %s", path, strerror(errno));
    char line[256]; /* pet_load()'s line limit */
    uint32_t have = 0, gen = 0;
    bool ok = true;
    const char *why = NULL;
    while (ok && fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        if (!len || line[len - 1] != '\n') {
            ok = false, why = feof(f) ? "truncated last line" : "overlong line";
            break;
        }
        line[len - 1] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        for (size_t i = 0; i < N_REQUIRED; i++) {
            if (strcmp(k, REQUIRED_KEYS[i])) continue;
            if (have & (UINT32_C(1) << i)) ok = false, why = "duplicate field";
            have |= UINT32_C(1) << i;
            if (!strcmp(k, "generation") && !parse_u32(v, &gen)) ok = false, why = "bad generation";
            if (!strcmp(k, "dna") && (!parse_hex64(v, dna) || !*dna)) ok = false, why = "bad dna";
            break;
        }
    }
    if (ok && ferror(f)) ok = false, why = "read error";
    fclose(f);
    if (!ok) return fail(error, error_len, "invalid life archive %s: %s", path, why);
    for (size_t i = 0; i < N_REQUIRED; i++)
        if (!(have & (UINT32_C(1) << i)))
            return fail(error, error_len, "invalid life archive %s: missing %s", path, REQUIRED_KEYS[i]);
    if (gen != generation)
        return fail(error, error_len, "life archive %s records generation %" PRIu32 ", expected %" PRIu32, path, gen,
                    generation);
    return true;
}

bool pet_lineage_load(const char *pet_path, uint32_t generation, Pet *out, char *error, size_t error_len) {
    if (error && error_len) error[0] = 0;
    if (!pet_path || !*pet_path || !path_fits(pet_path)) return fail(error, error_len, "invalid pet path");
    if (!generation) return fail(error, error_len, "invalid generation 0");
    char path[REWRITE_PATH_MAX];
    if (!life_path(path, pet_path, generation)) return fail(error, error_len, "life archive path too long");
    uint64_t dna = 0;
    if (!scan_archive(path, generation, &dna, error, error_len)) return false;
    Pet p;
    if (!pet_load(&p, path)) return fail(error, error_len, "cannot load %s: %s", path, strerror(errno));
    if (p.generation != generation || p.dna != dna || !p.name[0] || !pet_rewrite_ready(&p))
        return fail(error, error_len, "invalid life archive %s: not a completed generation %" PRIu32 " life", path,
                    generation);
    *out = p;
    return true;
}

bool pet_rewrite_commit(Pet *p, const char *pet_path, int trait, int64_t now, char *error, size_t error_len) {
    if (error && error_len) error[0] = 0;
    char archive[REWRITE_PATH_MAX], dir[REWRITE_PATH_MAX];
    if (!p || !pet_path || !*pet_path || !path_fits(pet_path) || !dir_of(dir, pet_path))
        return fail(error, error_len, "invalid pet path");
    Pet next;
    if (!pet_rewrite_preview(p, trait, now, &next)) return fail(error, error_len, "DNA Rewrite is not available");
    if (!life_path(archive, pet_path, p->generation)) return fail(error, error_len, "life archive path too long");
    uint64_t dna = pet_dna(p);

    /* A leftover archive of this very life (an earlier commit that stopped before the cutover) may be
     * refreshed; anything else stays as it is. */
    struct stat st;
    if (lstat(archive, &st) == 0) {
        if (!S_ISREG(st.st_mode)) return fail(error, error_len, "refusing to replace %s: not a regular file", archive);
        Pet old;
        char why[REWRITE_PATH_MAX + 128];
        if (!pet_lineage_load(pet_path, p->generation, &old, why, sizeof why))
            return fail(error, error_len, "refusing to replace existing archive: %s", why);
        if (old.dna != dna) return fail(error, error_len, "refusing to replace %s: it holds another life", archive);
    } else if (errno != ENOENT) {
        return fail(error, error_len, "cannot inspect %s: %s", archive, strerror(errno));
    }

    errno = 0;
    if (!pet_save(p, archive))
        return fail(error, error_len, "cannot write %s: %s", archive, errno ? strerror(errno) : "save failed");
    if (!sync_dir(dir)) return fail(error, error_len, "cannot sync %s: %s", dir, strerror(errno));
    Pet check;
    char why[REWRITE_PATH_MAX + 128];
    if (!pet_lineage_load(pet_path, p->generation, &check, why, sizeof why))
        return fail(error, error_len, "archive verification failed: %s", why);
    if (check.dna != dna) return fail(error, error_len, "archive verification failed: %s holds another life", archive);

    errno = 0;
    if (!pet_save(&next, pet_path))
        return fail(error, error_len, "cannot write %s: %s", pet_path, errno ? strerror(errno) : "save failed");
    *p = next; /* the new life is the current pet file now; never report a rollback past this point */
    if (!sync_dir(dir)) {
        fprintf(stderr, "DNA Rewrite: cannot sync %s after replacing %s: %s\n", dir, pet_path, strerror(errno));
        fail(error, error_len, "saved, but %s may not survive power loss", pet_path);
    }
    return true;
}
