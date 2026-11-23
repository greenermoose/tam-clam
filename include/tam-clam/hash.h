/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Cached PATH command lookup table and the hash builtin. */
#ifndef TAM_CLAM_HASH_H
#define TAM_CLAM_HASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct tc_shell;

struct tc_hash_entry {
	char *name;
	char *path;
	unsigned hits;
};

struct tc_hash_table {
	struct tc_hash_entry *slots;
	size_t cap;
	size_t count;
	uint64_t path_gen;
};

void tc_hash_init(struct tc_hash_table *ht);
void tc_hash_finish(struct tc_hash_table *ht);
void tc_hash_clear(struct tc_hash_table *ht);

/* Invalidate the table if PATH's generation counter changed. */
void tc_hash_sync(struct tc_hash_table *ht, uint64_t path_gen);

const char *tc_hash_get(struct tc_hash_table *ht, const char *name,
			bool count_hit);
int tc_hash_put(struct tc_hash_table *ht, const char *name, const char *path,
		bool count_hit);

/* Return a heap-allocated, name-sorted array of pointers to hash entries.
 * Caller frees *out. Returns 0 on success, -1 on OOM. */
int tc_hash_list(const struct tc_hash_table *ht,
		 const struct tc_hash_entry ***out, size_t *out_n);

int tc_bi_hash(struct tc_shell *sh, int argc, char **argv, unsigned long line);

#endif
