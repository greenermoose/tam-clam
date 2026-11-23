/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "tam-clam/hash.h"
#include "tam-clam/var.h"

int main(void)
{
	struct tc_hash_table ht;
	tc_hash_init(&ht);

	CHECK(tc_hash_get(&ht, "ls", false) == NULL);
	CHECK(tc_hash_put(&ht, "ls", "/usr/bin/ls", true) == 0);
	CHECK(tc_hash_put(&ht, "cat", "/usr/bin/cat", false) == 0);
	CHECK(strcmp(tc_hash_get(&ht, "ls", true), "/usr/bin/ls") == 0);
	CHECK(strcmp(tc_hash_get(&ht, "cat", false), "/usr/bin/cat") == 0);

	const struct tc_hash_entry **list = NULL;
	size_t n = 0;
	CHECK(tc_hash_list(&ht, &list, &n) == 0);
	CHECK(n == 2);
	CHECK(strcmp(list[0]->name, "cat") == 0 && list[0]->hits == 0);
	CHECK(strcmp(list[1]->name, "ls") == 0 && list[1]->hits == 2);
	free(list);

	/* PATH generation sync invalidates cached entries. */
	struct tc_vars v;
	tc_var_init(&v);
	CHECK(tc_var_set(&v, "PATH", "/usr/bin", TC_VAR_EXPORT) == TC_VAR_OK);
	tc_hash_sync(&ht, v.path_gen);
	CHECK(tc_hash_get(&ht, "ls", false) == NULL);

	CHECK(tc_hash_put(&ht, "ls", "/usr/bin/ls", false) == 0);
	/* Unrelated variable change does NOT invalidate hash. */
	CHECK(tc_var_set(&v, "FOO", "bar", 0) == TC_VAR_OK);
	tc_hash_sync(&ht, v.path_gen);
	CHECK(tc_hash_get(&ht, "ls", false) != NULL);

	/* Changing PATH invalidates hash. */
	CHECK(tc_var_set(&v, "PATH", "/bin:/usr/bin", TC_VAR_EXPORT) == TC_VAR_OK);
	tc_hash_sync(&ht, v.path_gen);
	CHECK(tc_hash_get(&ht, "ls", false) == NULL);

	tc_var_finish(&v);
	tc_hash_finish(&ht);
	CHECK_DONE("test_hash");
}
