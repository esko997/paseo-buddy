#pragma once
/* DNA Rewrite storage: archives each completed life next to the current pet, then cuts the current
 * pet over to its next life. Archives are full Pet snapshots in the pet_save() format at
 * "<pet_path>.life-<generation>", one per completed life, never rewritten once the generation has
 * advanced. Only one process may write the pet directory (the app); there is no file locking. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pet.h"

/* Longest accepted path, including the ".tmp" staging name pet_save() derives from it. */
#define REWRITE_PATH_MAX 768

/* Loads the archived life `generation` (1 = the original) of the pet saved at `pet_path`. The file
 * must carry every identity and life field explicitly, record the same generation and describe a
 * life that had reached its final form; anything else (missing, empty, unreadable, truncated, other
 * generation, legacy save) is rejected. On failure returns false with a message in `error` and
 * leaves *out untouched. `error` may be NULL when error_len is 0. */
bool pet_lineage_load(const char *pet_path, uint32_t generation, Pet *out, char *error, size_t error_len);

/* Rewrites the pet's DNA: builds the next life with pet_rewrite_preview(p, trait, now), durably
 * archives *p as "<pet_path>.life-<p->generation>" (file and directory fsynced), and only then
 * replaces the current pet file with the new life via pet_save(). *p becomes the new life only once
 * that replacement succeeded.
 *
 * False (with a message in `error`): *p and the current pet file are unchanged. A failure after the
 * archive was written leaves that archive as retryable staging; a later commit of the same life
 * (same generation and DNA) refreshes it with the newest stats. An existing archive that belongs to
 * a different life, or that cannot be validated, is never overwritten.
 *
 * True: *p is the new life. `error` is empty, or holds a durability warning when the directory
 * fsync after the cutover failed (the new pet file is in place; the rename may not survive power
 * loss). */
bool pet_rewrite_commit(Pet *p, const char *pet_path, int trait, int64_t now, char *error, size_t error_len);
