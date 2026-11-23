/* SPDX-License-Identifier: GPL-3.0-or-later */
/* POSIX.1-2024 `test` and `[` builtins. */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tam-clam/builtins.h"
#include "tam-clam/shell.h"

#define TC_TEST_DEPTH_MAX 128

struct test_ctx {
	struct tc_shell *sh;
	const char *cmd;
	unsigned long line;
	char **argv;
	int argc;
	int pos;
	unsigned depth;
	bool err;
};

static int parse_int64(const char *s, int64_t *out)
{
	if (!s)
		return -1;
	while (*s == ' ' || *s == '\t' || *s == '\n')
		s++;
	if (*s == '\0')
		return -1;
	char *end = NULL;
	errno = 0;
	long long v = strtoll(s, &end, 10);
	if (end == s || errno == ERANGE)
		return -1;
	while (*end == ' ' || *end == '\t' || *end == '\n')
		end++;
	if (*end != '\0')
		return -1;
	*out = (int64_t)v;
	return 0;
}

static bool is_unary_op(const char *s)
{
	if (!s || s[0] != '-' || s[1] == '\0' || s[2] != '\0')
		return false;
	switch (s[1]) {
	case 'b': case 'c': case 'd': case 'e': case 'f': case 'g':
	case 'h': case 'k': case 'L': case 'n': case 'O': case 'G':
	case 'p': case 'r': case 'S': case 's': case 't': case 'u':
	case 'w': case 'x': case 'z':
		return true;
	default:
		return false;
	}
}

static bool is_binary_op(const char *s)
{
	if (!s)
		return false;
	return strcmp(s, "=") == 0 || strcmp(s, "!=") == 0 ||
	       strcmp(s, "-eq") == 0 || strcmp(s, "-ne") == 0 ||
	       strcmp(s, "-gt") == 0 || strcmp(s, "-ge") == 0 ||
	       strcmp(s, "-lt") == 0 || strcmp(s, "-le") == 0 ||
	       strcmp(s, "-ef") == 0 || strcmp(s, "-nt") == 0 ||
	       strcmp(s, "-ot") == 0;
}

static int eval_unary(struct test_ctx *ctx, const char *op, const char *arg)
{
	struct stat st;
	switch (op[1]) {
	case 'z': return arg[0] == '\0' ? 1 : 0;
	case 'n': return arg[0] != '\0' ? 1 : 0;
	case 'e': return stat(arg, &st) == 0 ? 1 : 0;
	case 'f': return stat(arg, &st) == 0 && S_ISREG(st.st_mode) ? 1 : 0;
	case 'd': return stat(arg, &st) == 0 && S_ISDIR(st.st_mode) ? 1 : 0;
	case 'b': return stat(arg, &st) == 0 && S_ISBLK(st.st_mode) ? 1 : 0;
	case 'c': return stat(arg, &st) == 0 && S_ISCHR(st.st_mode) ? 1 : 0;
	case 'p': return stat(arg, &st) == 0 && S_ISFIFO(st.st_mode) ? 1 : 0;
	case 'S': return stat(arg, &st) == 0 && S_ISSOCK(st.st_mode) ? 1 : 0;
	case 'h':
	case 'L': return lstat(arg, &st) == 0 && S_ISLNK(st.st_mode) ? 1 : 0;
	case 's': return stat(arg, &st) == 0 && st.st_size > 0 ? 1 : 0;
	case 'r': return access(arg, R_OK) == 0 ? 1 : 0;
	case 'w': return access(arg, W_OK) == 0 ? 1 : 0;
	case 'x': return access(arg, X_OK) == 0 ? 1 : 0;
	case 'u': return stat(arg, &st) == 0 && (st.st_mode & S_ISUID) ? 1 : 0;
	case 'g': return stat(arg, &st) == 0 && (st.st_mode & S_ISGID) ? 1 : 0;
	case 'k':
#ifdef S_ISVTX
		return stat(arg, &st) == 0 && (st.st_mode & S_ISVTX) ? 1 : 0;
#else
		return 0;
#endif
	case 'O': return stat(arg, &st) == 0 && st.st_uid == geteuid() ? 1 : 0;
	case 'G': return stat(arg, &st) == 0 && st.st_gid == getegid() ? 1 : 0;
	case 't': {
		int64_t fd;
		if (parse_int64(arg, &fd) < 0 || fd < 0 || fd > INT_MAX) {
			tc_error(ctx->sh, ctx->line, "%s: %s: integer expression expected",
				 ctx->cmd, arg);
			ctx->err = true;
			return 0;
		}
		return isatty((int)fd) ? 1 : 0;
	}
	default:
		ctx->err = true;
		return 0;
	}
}

static int eval_binary(struct test_ctx *ctx, const char *lhs, const char *op,
		       const char *rhs)
{
	if (strcmp(op, "=") == 0)
		return strcmp(lhs, rhs) == 0 ? 1 : 0;
	if (strcmp(op, "!=") == 0)
		return strcmp(lhs, rhs) != 0 ? 1 : 0;

	if (strcmp(op, "-ef") == 0) {
		struct stat sa, sb;
		if (stat(lhs, &sa) != 0 || stat(rhs, &sb) != 0)
			return 0;
		return (sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino) ? 1 : 0;
	}
	if (strcmp(op, "-nt") == 0 || strcmp(op, "-ot") == 0) {
		struct stat sa, sb;
		bool ha = (stat(lhs, &sa) == 0);
		bool hb = (stat(rhs, &sb) == 0);
		if (strcmp(op, "-nt") == 0) {
			if (!ha)
				return 0;
			if (!hb)
				return 1;
			if (sa.st_mtim.tv_sec != sb.st_mtim.tv_sec)
				return sa.st_mtim.tv_sec > sb.st_mtim.tv_sec ? 1 : 0;
			return sa.st_mtim.tv_nsec > sb.st_mtim.tv_nsec ? 1 : 0;
		} else {
			if (!hb)
				return 0;
			if (!ha)
				return 1;
			if (sa.st_mtim.tv_sec != sb.st_mtim.tv_sec)
				return sa.st_mtim.tv_sec < sb.st_mtim.tv_sec ? 1 : 0;
			return sa.st_mtim.tv_nsec < sb.st_mtim.tv_nsec ? 1 : 0;
		}
	}

	int64_t a, b;
	if (parse_int64(lhs, &a) < 0) {
		tc_error(ctx->sh, ctx->line, "%s: %s: integer expression expected",
			 ctx->cmd, lhs);
		ctx->err = true;
		return 0;
	}
	if (parse_int64(rhs, &b) < 0) {
		tc_error(ctx->sh, ctx->line, "%s: %s: integer expression expected",
			 ctx->cmd, rhs);
		ctx->err = true;
		return 0;
	}
	if (strcmp(op, "-eq") == 0) return a == b ? 1 : 0;
	if (strcmp(op, "-ne") == 0) return a != b ? 1 : 0;
	if (strcmp(op, "-gt") == 0) return a > b ? 1 : 0;
	if (strcmp(op, "-ge") == 0) return a >= b ? 1 : 0;
	if (strcmp(op, "-lt") == 0) return a < b ? 1 : 0;
	if (strcmp(op, "-le") == 0) return a <= b ? 1 : 0;
	ctx->err = true;
	return 0;
}

static int eval_or(struct test_ctx *ctx);

static int eval_primary(struct test_ctx *ctx)
{
	if (ctx->pos >= ctx->argc) {
		tc_error(ctx->sh, ctx->line, "%s: argument expected", ctx->cmd);
		ctx->err = true;
		return 0;
	}

	int rem = ctx->argc - ctx->pos;
	const char *cur = ctx->argv[ctx->pos];

	if (strcmp(cur, "(") == 0 && rem >= 2) {
		if (ctx->depth >= TC_TEST_DEPTH_MAX) {
			tc_error(ctx->sh, ctx->line, "%s: expression too deeply nested",
				 ctx->cmd);
			ctx->err = true;
			return 0;
		}
		ctx->pos++;
		ctx->depth++;
		int v = eval_or(ctx);
		ctx->depth--;
		if (ctx->err)
			return 0;
		if (ctx->pos >= ctx->argc || strcmp(ctx->argv[ctx->pos], ")") != 0) {
			tc_error(ctx->sh, ctx->line, "%s: missing `)'", ctx->cmd);
			ctx->err = true;
			return 0;
		}
		ctx->pos++;
		return v;
	}

	if (rem >= 3 && is_binary_op(ctx->argv[ctx->pos + 1])) {
		const char *lhs = ctx->argv[ctx->pos];
		const char *op = ctx->argv[ctx->pos + 1];
		const char *rhs = ctx->argv[ctx->pos + 2];
		ctx->pos += 3;
		return eval_binary(ctx, lhs, op, rhs);
	}

	if (rem >= 2 && is_unary_op(cur)) {
		const char *arg = ctx->argv[ctx->pos + 1];
		ctx->pos += 2;
		return eval_unary(ctx, cur, arg);
	}

	ctx->pos++;
	return cur[0] != '\0' ? 1 : 0;
}

static int eval_not(struct test_ctx *ctx)
{
	if (ctx->pos < ctx->argc && strcmp(ctx->argv[ctx->pos], "!") == 0) {
		/* If '!' is followed by a binary operator (e.g. ! = x), it is
		 * the left operand, not negation. */
		if (ctx->pos + 2 < ctx->argc && is_binary_op(ctx->argv[ctx->pos + 1]))
			return eval_primary(ctx);
		if (ctx->depth >= TC_TEST_DEPTH_MAX) {
			tc_error(ctx->sh, ctx->line, "%s: expression too deeply nested",
				 ctx->cmd);
			ctx->err = true;
			return 0;
		}
		ctx->pos++;
		ctx->depth++;
		int v = eval_not(ctx);
		ctx->depth--;
		return ctx->err ? 0 : !v;
	}
	return eval_primary(ctx);
}

static int eval_and(struct test_ctx *ctx)
{
	int v = eval_not(ctx);
	while (!ctx->err && ctx->pos < ctx->argc &&
	       strcmp(ctx->argv[ctx->pos], "-a") == 0) {
		ctx->pos++;
		int rhs = eval_not(ctx);
		v = v && rhs;
	}
	return v;
}

static int eval_or(struct test_ctx *ctx)
{
	int v = eval_and(ctx);
	while (!ctx->err && ctx->pos < ctx->argc &&
	       strcmp(ctx->argv[ctx->pos], "-o") == 0) {
		ctx->pos++;
		int rhs = eval_and(ctx);
		v = v || rhs;
	}
	return v;
}

/* POSIX 2-, 3-, and 4-argument exact disambiguation (XCU `test`). */
static int eval_2(struct test_ctx *ctx, char **a)
{
	if (strcmp(a[0], "!") == 0)
		return a[1][0] == '\0' ? 1 : 0;
	if (is_unary_op(a[0]))
		return eval_unary(ctx, a[0], a[1]);
	tc_error(ctx->sh, ctx->line, "%s: %s: unary operator expected",
		 ctx->cmd, a[0]);
	ctx->err = true;
	return 0;
}

static int eval_3(struct test_ctx *ctx, char **a)
{
	if (is_binary_op(a[1]))
		return eval_binary(ctx, a[0], a[1], a[2]);
	if (strcmp(a[0], "!") == 0) {
		int v = eval_2(ctx, a + 1);
		return ctx->err ? 0 : !v;
	}
	if (strcmp(a[0], "(") == 0 && strcmp(a[2], ")") == 0)
		return a[1][0] != '\0' ? 1 : 0;
	if (strcmp(a[1], "-a") == 0)
		return (a[0][0] != '\0' && a[2][0] != '\0') ? 1 : 0;
	if (strcmp(a[1], "-o") == 0)
		return (a[0][0] != '\0' || a[2][0] != '\0') ? 1 : 0;
	tc_error(ctx->sh, ctx->line, "%s: %s: binary operator expected",
		 ctx->cmd, a[1]);
	ctx->err = true;
	return 0;
}

int tc_bi_test(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	const char *cmd = argv[0];
	bool bracket = (strcmp(cmd, "[") == 0);
	int n = argc - 1;
	char **args = argv + 1;

	if (bracket) {
		if (n < 1 || strcmp(args[n - 1], "]") != 0) {
			tc_error(sh, line, "[: missing `]'");
			return TC_STATUS_USAGE;
		}
		n--;
	}

	if (n == 0)
		return 1;
	if (n == 1)
		return args[0][0] != '\0' ? 0 : 1;

	struct test_ctx ctx = {
		.sh = sh,
		.cmd = cmd,
		.line = line,
		.argv = args,
		.argc = n,
		.pos = 0,
		.depth = 0,
		.err = false,
	};

	int res = 0;
	if (n == 2) {
		res = eval_2(&ctx, args);
	} else if (n == 3) {
		res = eval_3(&ctx, args);
	} else if (n == 4 && strcmp(args[0], "!") == 0) {
		res = eval_3(&ctx, args + 1);
		if (!ctx.err)
			res = !res;
	} else if (n == 4 && strcmp(args[0], "(") == 0 && strcmp(args[3], ")") == 0) {
		res = eval_2(&ctx, args + 1);
	} else {
		res = eval_or(&ctx);
		if (!ctx.err && ctx.pos < ctx.argc) {
			tc_error(sh, line, "%s: unexpected argument `%s'",
				 cmd, ctx.argv[ctx.pos]);
			ctx.err = true;
		}
	}

	if (ctx.err)
		return TC_STATUS_USAGE;
	return res ? 0 : 1;
}
