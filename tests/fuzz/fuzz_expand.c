/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fuzz target for word expansion, pattern matching, and arithmetic (no fork). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "tam-clam/arith.h"
#include "tam-clam/ast.h"
#include "tam-clam/expand.h"
#include "tam-clam/input.h"
#include "tam-clam/lex.h"
#include "tam-clam/pattern.h"
#include "tam-clam/shell.h"

#define TC_FUZZ_EXPAND_MAX 16384

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static bool parts_have_cmdsub(const struct tc_wpart *wp, int depth)
{
	if (depth > 128)
		return true;
	for (; wp; wp = wp->next) {
		if (wp->kind == TC_WPART_CMDSUB)
			return true;
		if (wp->arg && parts_have_cmdsub(wp->arg, depth + 1))
			return true;
	}
	return false;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size > TC_FUZZ_EXPAND_MAX)
		return 0;

	struct tc_shell sh;
	tc_shell_init(&sh, "fuzz");
	sh.opt_noglob = true;
	(void)tc_var_set(&sh.vars, "X", "hello world", 0);
	(void)tc_var_set(&sh.vars, "EMPTY", "", 0);
	(void)tc_var_set(&sh.vars, "PATHLIKE", "a/b/c.tar.gz", 0);
	(void)tc_var_set(&sh.vars, "NUM", "42", 0);
	(void)tc_var_set(&sh.vars, "IFS", " \t\n:", 0);
	char *params[] = {"one", "two three", "four"};
	(void)tc_shell_set_params(&sh, 3, params);

	/* 1. Lex words and run single, pattern, and field-splitting expansion. */
	struct tc_input in;
	tc_input_from_string(&in, (const char *)data, size);
	sh.in = &in;
	in.sh = &sh;

	struct tc_lexer lx;
	if (tc_lexer_init(&lx, &sh, &in) == TC_OK) {
		for (;;) {
			struct tc_token tok;
			int r = tc_lex_next(&lx, &tok);
			if (r != TC_OK || tok.kind == TC_TOK_END)
				break;
			if (tok.kind == TC_TOK_WORD &&
			    !parts_have_cmdsub(tok.parts, 0)) {
				struct tc_word w = {
					tok.text, tok.len, tok.parts,
					tok.flags, NULL, tok.line, tok.col,
				};
				char *single = NULL;
				(void)tc_expand_single(&sh, &w, false, &single);

				struct tc_pat pat;
				if (tc_expand_pat(&sh, &w, &pat) == TC_EXP_OK)
					(void)tc_pat_match(&pat, "a/b/c.tar.gz", 12);

				char **argv = NULL;
				size_t argc = 0;
				(void)tc_expand_words(&sh, &w, 1, &argv, &argc);
			}
			tc_arena_reset(&sh.arena);
		}
		tc_lexer_finish(&lx);
	}
	tc_input_finish(&in);
	tc_arena_reset(&sh.arena);

	/* 2. Here-document / PS4 expansion path (skip if cmdsub present). */
	struct tc_wpart *hparts = NULL;
	if (tc_lex_expand_parts(&sh, (const char *)data, size, 1, &hparts) == TC_OK &&
	    !parts_have_cmdsub(hparts, 0)) {
		char *hout = NULL;
		size_t hlen = 0;
		(void)tc_expand_heredoc(&sh, hparts, 1, &hout, &hlen);
	}
	tc_arena_reset(&sh.arena);

	/* 3. Direct arithmetic evaluator on raw bytes (NULs mapped to spaces). */
	if (size <= 4096) {
		char *expr = malloc(size + 1);
		if (expr) {
			for (size_t i = 0; i < size; i++)
				expr[i] = data[i] ? (char)data[i] : ' ';
			expr[size] = '\0';
			int64_t val = 0;
			(void)tc_arith_eval(&sh, expr, 1, &val);
			free(expr);
		}
	}

	tc_shell_finish(&sh);
	return 0;
}
