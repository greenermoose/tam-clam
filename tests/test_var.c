/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "check.h"
#include "tam-clam/arena.h"
#include "tam-clam/var.h"

static int envp_has(char **envp, const char *entry)
{
	if (!envp)
		return 0;
	for (size_t i = 0; envp[i]; i++) {
		if (strcmp(envp[i], entry) == 0)
			return 1;
	}
	return 0;
}

int main(void)
{
	CHECK(tc_var_valid_name("x", 1));
	CHECK(tc_var_valid_name("_a1", 3));
	CHECK(tc_var_valid_name("PATH", 4));
	CHECK(!tc_var_valid_name("", 0));
	CHECK(!tc_var_valid_name("1a", 2));
	CHECK(!tc_var_valid_name("a-b", 3));
	CHECK(!tc_var_valid_name("a=b", 3));

	struct tc_vars v;
	tc_var_init(&v);

	/* Basic get/set/unset. */
	CHECK(tc_var_get(&v, "FOO") == NULL);
	CHECK(tc_var_set(&v, "FOO", "bar", 0) == TC_VAR_OK);
	CHECK(strcmp(tc_var_get(&v, "FOO"), "bar") == 0);
	CHECK(tc_var_set(&v, "FOO", "baz", 0) == TC_VAR_OK);
	CHECK(strcmp(tc_var_get(&v, "FOO"), "baz") == 0);
	CHECK(tc_var_set(&v, "1BAD", "x", 0) == TC_VAR_ERR_NAME);

	/* Unexported vars do not appear in envp. */
	char **e1 = tc_var_envp(&v);
	CHECK(e1 != NULL && !envp_has(e1, "FOO=baz"));
	/* Cached envp pointer reused when not dirty. */
	CHECK(tc_var_envp(&v) == e1);

	/* Exporting marks dirty and adds to envp. */
	CHECK(tc_var_set(&v, "FOO", NULL, TC_VAR_EXPORT) == TC_VAR_OK);
	char **e2 = tc_var_envp(&v);
	CHECK(e2 != NULL && envp_has(e2, "FOO=baz"));

	/* Readonly blocks overwrite and unset. */
	CHECK(tc_var_set(&v, "RO", "fixed", TC_VAR_READONLY) == TC_VAR_OK);
	CHECK(tc_var_is_readonly(&v, "RO"));
	CHECK(tc_var_set(&v, "RO", "new", 0) == TC_VAR_ERR_READONLY);
	CHECK(tc_var_unset(&v, "RO") == TC_VAR_ERR_READONLY);
	CHECK(strcmp(tc_var_get(&v, "RO"), "fixed") == 0);

	/* Unset removes variable and updates envp. */
	CHECK(tc_var_unset(&v, "FOO") == TC_VAR_OK);
	CHECK(tc_var_get(&v, "FOO") == NULL);
	CHECK(!envp_has(tc_var_envp(&v), "FOO=baz"));

	/* Import environ skips invalid names. */
	char *fake_env[] = {
		"GOOD=1",
		"BASH_FUNC_foo%%=() { echo hi; }",
		"NOEQUALS",
		"EMPTY=",
		NULL,
	};
	CHECK(tc_var_import_environ(&v, fake_env) == TC_VAR_OK);
	CHECK(strcmp(tc_var_get(&v, "GOOD"), "1") == 0);
	CHECK(strcmp(tc_var_get(&v, "EMPTY"), "") == 0);
	CHECK(envp_has(tc_var_envp(&v), "GOOD=1"));
	CHECK(envp_has(tc_var_envp(&v), "EMPTY="));

	/* Override envp and lookup. */
	struct tc_arena a;
	tc_arena_init(&a, TC_ARENA_MAX);
	struct tc_var_kv kvs[] = {
		{"GOOD", "2"},
		{"TEMP", "hello"},
		{"TEMP", "world"},
	};
	CHECK(strcmp(tc_var_lookup_override(&v, kvs, 3, "GOOD"), "2") == 0);
	CHECK(strcmp(tc_var_lookup_override(&v, kvs, 3, "TEMP"), "world") == 0);
	char **eo = tc_var_envp_override(&v, kvs, 3, &a);
	CHECK(eo != NULL);
	CHECK(envp_has(eo, "GOOD=2"));
	CHECK(!envp_has(eo, "GOOD=1"));
	CHECK(envp_has(eo, "TEMP=world"));
	CHECK(!envp_has(eo, "TEMP=hello"));
	CHECK(strcmp(tc_var_get(&v, "GOOD"), "1") == 0);
	CHECK(tc_var_get(&v, "TEMP") == NULL);

	/* Push / pop overrides (regular builtin semantics). */
	struct tc_var_save saves[3];
	size_t nsaved = 0;
	CHECK(tc_var_push_overrides(&v, kvs, 3, saves, &nsaved) == TC_VAR_OK);
	CHECK(strcmp(tc_var_get(&v, "GOOD"), "2") == 0);
	CHECK(strcmp(tc_var_get(&v, "TEMP"), "world") == 0);
	tc_var_pop_overrides(&v, saves, nsaved);
	CHECK(strcmp(tc_var_get(&v, "GOOD"), "1") == 0);
	CHECK(tc_var_get(&v, "TEMP") == NULL);

	tc_arena_finish(&a);
	tc_var_finish(&v);

	CHECK_DONE("test_var");
}
