/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "check.h"
#include "tam-clam/input.h"
#include "tam-clam/parse.h"

int main(void)
{
	struct tc_shell sh;
	struct tc_input in;
	struct tc_parser p;
	struct tc_node *n = NULL;

	tc_shell_init(&sh, "test");

	/* Parse a function definition, store it, reset the command arena, and
	 * verify the cloned AST survives independently. */
	const char *src = "greet() { echo hello; if true; then echo ok; fi; }\n";
	tc_input_from_string(&in, src, strlen(src));
	CHECK(tc_parser_init(&p, &sh, &in) == TC_OK);
	CHECK(tc_parse_next(&p, &n) == TC_OK && n && n->kind == TC_NODE_FUNCDEF);
	CHECK(strcmp(n->u.funcdef.name, "greet") == 0);
	CHECK(tc_func_define(&sh.funcs, n->u.funcdef.name, n->u.funcdef.body) == 0);
	tc_parser_finish(&p);
	tc_arena_reset(&sh.arena);

	struct tc_func *fn = tc_func_find(&sh.funcs, "greet");
	CHECK(fn != NULL);
	CHECK(fn->body != NULL && fn->body->kind == TC_NODE_GROUP);
	CHECK(fn->refcount == 1);

	/* Holding an extra ref keeps the old definition alive across overwrite. */
	tc_func_ref(fn);
	CHECK(fn->refcount == 2);

	const char *src2 = "greet() { echo replaced; }\n";
	tc_input_from_string(&in, src2, strlen(src2));
	CHECK(tc_parser_init(&p, &sh, &in) == TC_OK);
	CHECK(tc_parse_next(&p, &n) == TC_OK && n && n->kind == TC_NODE_FUNCDEF);
	CHECK(tc_func_define(&sh.funcs, n->u.funcdef.name, n->u.funcdef.body) == 0);
	tc_parser_finish(&p);
	tc_arena_reset(&sh.arena);

	struct tc_func *fn2 = tc_func_find(&sh.funcs, "greet");
	CHECK(fn2 != NULL && fn2 != fn);
	CHECK(fn->refcount == 1);
	CHECK(fn->body->kind == TC_NODE_GROUP);
	tc_func_unref(fn);

	/* Unset removes from table; lookup returns NULL. */
	tc_func_unset(&sh.funcs, "greet");
	CHECK(tc_func_find(&sh.funcs, "greet") == NULL);

	/* Arena mark/rewind reclaims scratch allocations without disturbing
	 * earlier data. */
	char *keep = tc_arena_strndup(&sh.arena, "keep", 4);
	CHECK(keep != NULL);
	struct tc_arena_mark m = tc_arena_mark(&sh.arena);
	char *scratch = tc_arena_alloc(&sh.arena, 32768);
	CHECK(scratch != NULL);
	size_t big_total = sh.arena.total;
	tc_arena_rewind(&sh.arena, m);
	CHECK(sh.arena.total < big_total);
	CHECK(strcmp(keep, "keep") == 0);

	tc_shell_finish(&sh);
	CHECK_DONE("test_func");
}
