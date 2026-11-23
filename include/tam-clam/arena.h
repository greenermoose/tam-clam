/* SPDX-License-Identifier: GPL-3.0-or-later */
/* An arena: memory for one command, freed all at once when the command
 * finishes. Allocation never aborts; it returns NULL when memory runs out
 * or when the arena reaches its cap, so a runaway input cannot exhaust
 * the machine. */
#ifndef TAM_CLAM_ARENA_H
#define TAM_CLAM_ARENA_H

#include <stddef.h>

/* The most memory one command may use. */
#define TC_ARENA_MAX ((size_t)64 << 20)

struct tc_arena_block;

struct tc_arena {
	struct tc_arena_block *head; /* newest block first */
	size_t total;                /* bytes held in all blocks */
	size_t max;                  /* cap on total */
};

struct tc_arena_mark {
	struct tc_arena_block *block;
	size_t used;
};

void tc_arena_init(struct tc_arena *a, size_t max);
/* Aligned for any type. NULL when out of memory or over the cap. */
void *tc_arena_alloc(struct tc_arena *a, size_t n);
/* A NUL-terminated copy of len bytes. */
char *tc_arena_strndup(struct tc_arena *a, const char *s, size_t len);
/* Save/restore position so loops reclaim per-iteration allocations. */
struct tc_arena_mark tc_arena_mark(const struct tc_arena *a);
void tc_arena_rewind(struct tc_arena *a, struct tc_arena_mark m);
/* Free everything except one block, which is kept for the next command. */
void tc_arena_reset(struct tc_arena *a);
void tc_arena_finish(struct tc_arena *a);

#endif
