/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "tam-clam/arith.h"
#include "tam-clam/shell.h"

static bool eval_ok(struct tc_shell *sh, const char *expr, int64_t want)
{
	int64_t got = 0;
	int r = tc_arith_eval(sh, expr, 1, &got);
	tc_arena_reset(&sh->arena);
	return r == TC_ARITH_OK && got == want;
}

static bool eval_err(struct tc_shell *sh, const char *expr)
{
	int64_t got = 0;
	int r = tc_arith_eval(sh, expr, 1, &got);
	tc_arena_reset(&sh->arena);
	return r == TC_ARITH_ERR;
}

int main(void)
{
	struct tc_shell sh;
	tc_shell_init(&sh, "test_arith");

	/* Empty, constants, bases */
	CHECK(eval_ok(&sh, "", 0));
	CHECK(eval_ok(&sh, "   \t\n ", 0));
	CHECK(eval_ok(&sh, "42", 42));
	CHECK(eval_ok(&sh, "0755", 0755));
	CHECK(eval_ok(&sh, "0x1f", 31));
	CHECK(eval_ok(&sh, "0XFF", 255));

	/* Precedence and associativity */
	CHECK(eval_ok(&sh, "1 + 2 * 3", 7));
	CHECK(eval_ok(&sh, "(1 + 2) * 3", 9));
	CHECK(eval_ok(&sh, "20 / 3", 6));
	CHECK(eval_ok(&sh, "20 % 3", 2));
	CHECK(eval_ok(&sh, "-5 + 2", -3));
	CHECK(eval_ok(&sh, "+5 - -3", 8));
	CHECK(eval_ok(&sh, "!0", 1));
	CHECK(eval_ok(&sh, "!42", 0));
	CHECK(eval_ok(&sh, "~0", -1));
	CHECK(eval_ok(&sh, "1 << 4", 16));
	CHECK(eval_ok(&sh, "32 >> 2", 8));
	CHECK(eval_ok(&sh, "-8 >> 1", -4));
	CHECK(eval_ok(&sh, "5 & 3", 1));
	CHECK(eval_ok(&sh, "5 | 2", 7));
	CHECK(eval_ok(&sh, "5 ^ 3", 6));
	CHECK(eval_ok(&sh, "3 < 4 && 4 <= 4 && 5 > 4 && 5 >= 5", 1));
	CHECK(eval_ok(&sh, "3 == 3 && 3 != 4", 1));
	CHECK(eval_ok(&sh, "0 || 1", 1));
	CHECK(eval_ok(&sh, "1 ? 10 : 20", 10));
	CHECK(eval_ok(&sh, "0 ? 10 : 20", 20));
	CHECK(eval_ok(&sh, "1, 2, 3", 3));

	/* Variables, assignments, compound assignments, ++/-- */
	CHECK(eval_ok(&sh, "unset_var + 5", 5));
	CHECK(eval_ok(&sh, "x = 10", 10));
	CHECK(strcmp(tc_var_get(&sh.vars, "x"), "10") == 0);
	CHECK(eval_ok(&sh, "x += 5", 15));
	CHECK(eval_ok(&sh, "x -= 3", 12));
	CHECK(eval_ok(&sh, "x *= 2", 24));
	CHECK(eval_ok(&sh, "x /= 4", 6));
	CHECK(eval_ok(&sh, "x %= 4", 2));
	CHECK(eval_ok(&sh, "x <<= 3", 16));
	CHECK(eval_ok(&sh, "x >>= 1", 8));
	CHECK(eval_ok(&sh, "x |= 3", 11));
	CHECK(eval_ok(&sh, "x &= 6", 2));
	CHECK(eval_ok(&sh, "x ^= 7", 5));
	CHECK(eval_ok(&sh, "++x", 6));
	CHECK(eval_ok(&sh, "x++", 6));
	CHECK(eval_ok(&sh, "x", 7));
	CHECK(eval_ok(&sh, "--x", 6));
	CHECK(eval_ok(&sh, "x--", 6));
	CHECK(eval_ok(&sh, "x", 5));

	/* Short-circuiting must suppress side effects and div-by-zero */
	CHECK(eval_ok(&sh, "0 && (1 / 0)", 0));
	CHECK(eval_ok(&sh, "1 || (1 / 0)", 1));
	CHECK(eval_ok(&sh, "1 ? 99 : (1 / 0)", 99));
	CHECK(eval_ok(&sh, "0 ? (x = 999) : 42", 42));
	CHECK(strcmp(tc_var_get(&sh.vars, "x"), "5") == 0);

	/* Readonly variable assignment error */
	tc_var_set(&sh.vars, "RO", "7", TC_VAR_READONLY);
	CHECK(eval_ok(&sh, "RO + 1", 8));
	CHECK(eval_err(&sh, "RO = 9"));

	/* Division by zero and checked 64-bit signed overflow */
	CHECK(eval_err(&sh, "1 / 0"));
	CHECK(eval_err(&sh, "1 % 0"));
	CHECK(eval_err(&sh, "9223372036854775807 + 1"));
	CHECK(eval_err(&sh, "-9223372036854775807 - 2"));
	CHECK(eval_err(&sh, "9223372036854775807 * 2"));
	CHECK(eval_err(&sh, "(-9223372036854775807 - 1) / -1"));
	CHECK(eval_err(&sh, "(-9223372036854775807 - 1) % -1"));
	CHECK(eval_err(&sh, "-(-9223372036854775807 - 1)"));
	CHECK(eval_err(&sh, "1 << 64"));
	CHECK(eval_err(&sh, "1 << -1"));
	CHECK(eval_err(&sh, "1 >> 64"));

	/* Syntax errors and nesting limit */
	CHECK(eval_err(&sh, "1 +"));
	CHECK(eval_err(&sh, "(1 + 2"));
	CHECK(eval_err(&sh, "1 2"));
	CHECK(eval_err(&sh, "08"));
	CHECK(eval_err(&sh, "0x"));
	{
		char deep[512];
		size_t pos = 0;
		for (int i = 0; i < 140; i++)
			deep[pos++] = '(';
		deep[pos++] = '1';
		for (int i = 0; i < 140; i++)
			deep[pos++] = ')';
		deep[pos] = '\0';
		CHECK(eval_err(&sh, deep));
	}

	tc_shell_finish(&sh);
	CHECK_DONE("test_arith");
}
