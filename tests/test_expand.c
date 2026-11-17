/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "check.h"
#include "tam-clam/expand.h"

static int removes(struct tc_arena *a, const char *raw, const char *want)
{
	struct tc_word w = {raw, strlen(raw), 0, 1};
	char *got = tc_expand_word(a, &w);
	return got && strcmp(got, want) == 0;
}

int main(void)
{
	struct tc_arena a;
	tc_arena_init(&a, TC_ARENA_MAX);

	CHECK(removes(&a, "plain", "plain"));
	CHECK(removes(&a, "'a b'", "a b"));
	CHECK(removes(&a, "'a\\b'", "a\\b"));
	CHECK(removes(&a, "\"a b\"", "a b"));
	CHECK(removes(&a, "\"a\\$b\"", "a$b"));
	CHECK(removes(&a, "\"a\\\\b\"", "a\\b"));
	CHECK(removes(&a, "\"a\\\"b\"", "a\"b"));
	CHECK(removes(&a, "\"a\\xb\"", "a\\xb"));
	CHECK(removes(&a, "a\\ b", "a b"));
	CHECK(removes(&a, "\\\\", "\\"));
	CHECK(removes(&a, "x'y'\"z\"w", "xyzw"));
	CHECK(removes(&a, "''", ""));
	CHECK(removes(&a, "\"'\"", "'"));
	CHECK(removes(&a, "'\"'", "\""));

	/* The arena: a reset keeps one block; the cap is enforced. */
	tc_arena_reset(&a);
	CHECK(tc_arena_alloc(&a, 100) != NULL);
	struct tc_arena small;
	tc_arena_init(&small, 1000);
	CHECK(tc_arena_alloc(&small, 2000) == NULL);
	tc_arena_finish(&small);
	tc_arena_finish(&a);

	CHECK_DONE("test_expand");
}
