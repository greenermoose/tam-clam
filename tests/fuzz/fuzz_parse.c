/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fuzz target for the lexer and parser (no execution, no forking). */
#include <stddef.h>
#include <stdint.h>

#include "tam-clam/input.h"
#include "tam-clam/parse.h"
#include "tam-clam/shell.h"

#define TC_FUZZ_PARSE_MAX 65536

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size > TC_FUZZ_PARSE_MAX)
		return 0;

	struct tc_shell sh;
	tc_shell_init(&sh, "fuzz");

	struct tc_input in;
	tc_input_from_string(&in, (const char *)data, size);
	sh.in = &in;
	in.sh = &sh;

	struct tc_parser p;
	if (tc_parser_init(&p, &sh, &in) == TC_OK) {
		for (;;) {
			struct tc_node *node = NULL;
			int r = tc_parse_next(&p, &node);
			tc_arena_reset(&sh.arena);
			if (r != TC_OK)
				break;
		}
		tc_parser_finish(&p);
	}

	tc_input_finish(&in);
	tc_shell_finish(&sh);
	return 0;
}
