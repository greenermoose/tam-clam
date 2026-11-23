/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "tam-clam/alias.h"
#include "tam-clam/input.h"
#include "tam-clam/parse.h"
#include "tam-clam/shell.h"

static struct tc_node *parse_one(struct tc_shell *sh, const char *src, int *res)
{
	struct tc_input in;
	tc_input_from_string(&in, src, strlen(src));
	struct tc_parser p;
	CHECK(tc_parser_init(&p, sh, &in) == TC_OK);
	struct tc_node *node = NULL;
	*res = tc_parse_next(&p, &node);
	tc_parser_finish(&p);
	tc_input_finish(&in);
	return node;
}

int main(void)
{
	/* Name validation. */
	CHECK(tc_alias_valid_name("ll"));
	CHECK(tc_alias_valid_name("my-cmd_1"));
	CHECK(tc_alias_valid_name("a.b"));
	CHECK(!tc_alias_valid_name(""));
	CHECK(!tc_alias_valid_name("a/b"));
	CHECK(!tc_alias_valid_name("a=b"));
	CHECK(!tc_alias_valid_name("a b"));
	CHECK(!tc_alias_valid_name("a;b"));
	CHECK(!tc_alias_valid_name("$x"));

	/* Table operations. */
	struct tc_aliases al;
	tc_alias_init(&al);
	CHECK(tc_alias_get(&al, "ll") == NULL);
	CHECK(tc_alias_set(&al, "ll", "ls -l") == 0);
	CHECK(strcmp(tc_alias_get(&al, "ll"), "ls -l") == 0);
	CHECK(tc_alias_set(&al, "aa", "echo hi") == 0);
	CHECK(tc_alias_set(&al, "ll", "ls -la") == 0);
	CHECK(strcmp(tc_alias_get(&al, "ll"), "ls -la") == 0);

	const struct tc_alias_entry **list = NULL;
	size_t n = 0;
	CHECK(tc_alias_list(&al, &list, &n) == 0);
	CHECK(n == 2);
	CHECK(strcmp(list[0]->name, "aa") == 0);
	CHECK(strcmp(list[1]->name, "ll") == 0);
	free(list);

	CHECK(tc_alias_unset(&al, "aa"));
	CHECK(!tc_alias_unset(&al, "aa"));
	CHECK(tc_alias_get(&al, "aa") == NULL);
	tc_alias_clear(&al);
	CHECK(tc_alias_get(&al, "ll") == NULL);
	tc_alias_finish(&al);

	/* Parser alias expansion. */
	struct tc_shell sh;
	tc_shell_init(&sh, "test_alias");
	CHECK(tc_alias_set(&sh.aliases, "greet", "echo hello") == 0);
	CHECK(tc_alias_set(&sh.aliases, "echo", "echo -n") == 0);
	CHECK(tc_alias_set(&sh.aliases, "pfx", "echo 1 ") == 0);
	CHECK(tc_alias_set(&sh.aliases, "w", "world") == 0);
	CHECK(tc_alias_set(&sh.aliases, "cyc1", "cyc2") == 0);
	CHECK(tc_alias_set(&sh.aliases, "cyc2", "cyc1") == 0);

	/* Simple + chained self-referential alias (greet -> echo -> echo -n). */
	int r = 0;
	struct tc_node *n1 = parse_one(&sh, "greet there\n", &r);
	CHECK(r == TC_OK && n1 && n1->kind == TC_NODE_SIMPLE);
	CHECK(n1->u.simple.nwords == 4);
	CHECK(strcmp(n1->u.simple.words[0].text, "echo") == 0);
	CHECK(strcmp(n1->u.simple.words[1].text, "-n") == 0);
	CHECK(strcmp(n1->u.simple.words[2].text, "hello") == 0);
	CHECK(strcmp(n1->u.simple.words[3].text, "there") == 0);
	tc_arena_reset(&sh.arena);

	/* Quoted word suppresses alias expansion. */
	struct tc_node *n2 = parse_one(&sh, "'greet' x\n", &r);
	CHECK(r == TC_OK && n2 && n2->kind == TC_NODE_SIMPLE);
	CHECK(n2->u.simple.nwords == 2);
	CHECK(strcmp(n2->u.simple.words[0].text, "'greet'") == 0);
	tc_arena_reset(&sh.arena);

	/* Trailing-blank chaining: pfx ends with space, so w expands too. */
	struct tc_node *n3 = parse_one(&sh, "pfx w\n", &r);
	CHECK(r == TC_OK && n3 && n3->kind == TC_NODE_SIMPLE);
	CHECK(n3->u.simple.nwords == 4);
	CHECK(strcmp(n3->u.simple.words[0].text, "echo") == 0);
	CHECK(strcmp(n3->u.simple.words[1].text, "-n") == 0);
	CHECK(strcmp(n3->u.simple.words[2].text, "1") == 0);
	CHECK(strcmp(n3->u.simple.words[3].text, "world") == 0);
	tc_arena_reset(&sh.arena);

	/* Mutual recursion terminates without looping. */
	struct tc_node *n4 = parse_one(&sh, "cyc1 arg\n", &r);
	CHECK(r == TC_OK && n4 && n4->kind == TC_NODE_SIMPLE);
	CHECK(n4->u.simple.nwords == 2);
	CHECK(strcmp(n4->u.simple.words[0].text, "cyc1") == 0);
	CHECK(strcmp(n4->u.simple.words[1].text, "arg") == 0);
	tc_arena_reset(&sh.arena);

	tc_shell_finish(&sh);
	CHECK_DONE("test_alias");
}
