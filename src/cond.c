/* SPDX-License-Identifier: GPL-3.0-or-later */
/* [[ ... ]] conditional compound command evaluator (0.3.1).
 * Supports &&, ||, !, (), unary/binary file/string/arithmetic tests,
 * == / != glob pattern matching, < / > lexicographical comparison,
 * -v variable/element checks, -o option checks, and =~ POSIX ERE matching
 * with BASH_REMATCH population. */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tam-clam/arith.h"
#include "tam-clam/cond.h"
#include "tam-clam/expand.h"
#include "tam-clam/options.h"
#include "tam-clam/pattern.h"
#include "tam-clam/regex.h"
#include "tam-clam/var.h"

#define TC_COND_DEPTH_MAX 256

static int eval_expr(struct tc_shell *sh, const struct tc_cond_expr *e,
		     unsigned depth, bool *out_val);

static int eval_var_exists(struct tc_shell *sh, const char *arg,
			   unsigned long line, bool *out_val)
{
	*out_val = false;
	const char *lb = strchr(arg, '[');
	size_t alen = strlen(arg);
	if (lb && alen > 2 && arg[alen - 1] == ']') {
		size_t nlen = (size_t)(lb - arg);
		size_t slen = alen - nlen - 2;
		if (!tc_var_valid_name(arg, nlen) || slen == 0)
			return 0;
		char *name = tc_arena_strndup(&sh->arena, arg, nlen);
		char *sub = tc_arena_strndup(&sh->arena, lb + 1, slen);
		if (!name || !sub) {
			tc_error(sh, line, "out of memory");
			return TC_STATUS_USAGE;
		}
		const struct tc_var_entry *e = tc_var_find(&sh->vars, name);
		if (!e)
			return 0;
		if (strcmp(sub, "@") == 0 || strcmp(sub, "*") == 0) {
			if (e->flags & TC_VAR_ARRAY)
				*out_val = (e->arr && e->arr->len > 0);
			else if (e->flags & TC_VAR_ASSOC)
				*out_val = (e->assoc && e->assoc->count > 0);
			else
				*out_val = (e->value != NULL);
			return 0;
		}
		if (e->flags & TC_VAR_ASSOC) {
			*out_val = (e->assoc && tc_assoc_get(e->assoc, sub) != NULL);
			return 0;
		}
		int64_t idx = 0;
		if (tc_arith_eval(sh, sub, line, &idx) != TC_ARITH_OK)
			return TC_STATUS_USAGE;
		if (e->flags & TC_VAR_ARRAY) {
			if (!e->arr)
				return 0;
			int64_t ridx = 0;
			if (tc_array_resolve_idx(e->arr, idx, &ridx) != TC_ARR_OK)
				return 0;
			const char *v = NULL;
			*out_val = (tc_array_get(e->arr, ridx, &v) == TC_ARR_OK && v != NULL);
			return 0;
		}
		*out_val = (e->value != NULL && (idx == 0 || idx == -1));
		return 0;
	}

	if (!tc_var_valid_name(arg, alen))
		return 0;
	const struct tc_var_entry *e = tc_var_find(&sh->vars, arg);
	if (!e)
		return 0;
	if (e->flags & TC_VAR_ARRAY) {
		const char *v = NULL;
		*out_val = (e->arr && tc_array_get(e->arr, 0, &v) == TC_ARR_OK && v != NULL);
		return 0;
	}
	if (e->flags & TC_VAR_ASSOC) {
		*out_val = (e->assoc && tc_assoc_get(e->assoc, "0") != NULL);
		return 0;
	}
	*out_val = (e->value != NULL);
	return 0;
}

static int eval_unary(struct tc_shell *sh, const char *op, const char *arg,
		      unsigned long line, bool *out_val)
{
	*out_val = false;
	if (!op || op[0] != '-' || op[1] == '\0' || op[2] != '\0') {
		tc_error(sh, line, "[[ ]]: %s: unknown unary operator", op ? op : "");
		return TC_STATUS_USAGE;
	}

	struct stat st;
	switch (op[1]) {
	case 'z': *out_val = (arg[0] == '\0'); return 0;
	case 'n': *out_val = (arg[0] != '\0'); return 0;
	case 'a':
	case 'e': *out_val = (stat(arg, &st) == 0); return 0;
	case 'f': *out_val = (stat(arg, &st) == 0 && S_ISREG(st.st_mode)); return 0;
	case 'd': *out_val = (stat(arg, &st) == 0 && S_ISDIR(st.st_mode)); return 0;
	case 'b': *out_val = (stat(arg, &st) == 0 && S_ISBLK(st.st_mode)); return 0;
	case 'c': *out_val = (stat(arg, &st) == 0 && S_ISCHR(st.st_mode)); return 0;
	case 'p': *out_val = (stat(arg, &st) == 0 && S_ISFIFO(st.st_mode)); return 0;
	case 'S': *out_val = (stat(arg, &st) == 0 && S_ISSOCK(st.st_mode)); return 0;
	case 'h':
	case 'L': *out_val = (lstat(arg, &st) == 0 && S_ISLNK(st.st_mode)); return 0;
	case 's': *out_val = (stat(arg, &st) == 0 && st.st_size > 0); return 0;
	case 'r': *out_val = (access(arg, R_OK) == 0); return 0;
	case 'w': *out_val = (access(arg, W_OK) == 0); return 0;
	case 'x': *out_val = (access(arg, X_OK) == 0); return 0;
	case 'u': *out_val = (stat(arg, &st) == 0 && (st.st_mode & S_ISUID) != 0); return 0;
	case 'g': *out_val = (stat(arg, &st) == 0 && (st.st_mode & S_ISGID) != 0); return 0;
	case 'k':
#ifdef S_ISVTX
		*out_val = (stat(arg, &st) == 0 && (st.st_mode & S_ISVTX) != 0);
#endif
		return 0;
	case 'O': *out_val = (stat(arg, &st) == 0 && st.st_uid == geteuid()); return 0;
	case 'G': *out_val = (stat(arg, &st) == 0 && st.st_gid == getegid()); return 0;
	case 't': {
		int64_t fd = 0;
		if (tc_arith_eval(sh, arg, line, &fd) != TC_ARITH_OK)
			return TC_STATUS_USAGE;
		if (fd >= 0 && fd <= INT_MAX)
			*out_val = (isatty((int)fd) != 0);
		return 0;
	}
	case 'v':
		return eval_var_exists(sh, arg, line, out_val);
	case 'o': {
		bool valid = false;
		*out_val = tc_opt_get_name(sh, arg, &valid);
		return 0;
	}
	default:
		tc_error(sh, line, "[[ ]]: %s: unknown unary operator", op);
		return TC_STATUS_USAGE;
	}
}

static int eval_regex(struct tc_shell *sh, const char *lhs,
		      const struct tc_word *rhs_word, unsigned long line,
		      bool *out_val)
{
	*out_val = false;
	char *re_str = NULL;
	size_t re_len = 0;
	int er = tc_expand_regex(sh, rhs_word, &re_str, &re_len);
	if (er != TC_EXP_OK)
		return TC_STATUS_USAGE;

	struct tc_re_match groups[TC_RE_GROUPS_MAX];
	size_t ngroups = 0;
	size_t lhs_len = strlen(lhs);
	int mr = tc_re_exec(re_str, re_len, lhs, lhs_len,
			    groups, TC_RE_GROUPS_MAX, &ngroups);
	if (mr == TC_RE_ERR_SYNTAX) {
		tc_error(sh, line, "[[ =~ ]]: invalid regular expression `%s'", re_str);
		return TC_STATUS_USAGE;
	}
	if (mr == TC_RE_ERR_LIMIT) {
		tc_error(sh, line, "[[ =~ ]]: regular expression exceeds complexity limit");
		return TC_STATUS_USAGE;
	}
	if (mr == TC_RE_ERR_NOMEM) {
		tc_error(sh, line, "out of memory");
		return TC_STATUS_USAGE;
	}

	/* Update BASH_REMATCH: reset to empty array on mismatch, populate on match. */
	if (!tc_var_is_readonly(&sh->vars, "BASH_REMATCH")) {
		(void)tc_var_reset_array(&sh->vars, "BASH_REMATCH", 0);
		if (mr == TC_RE_MATCH) {
			for (size_t g = 0; g < ngroups; g++) {
				if (groups[g].start == SIZE_MAX || groups[g].end < groups[g].start) {
					(void)tc_var_set_elem_idx(&sh->vars, "BASH_REMATCH",
								  (int64_t)g, "", false, 0);
				} else {
					size_t gl = groups[g].end - groups[g].start;
					char *sub = tc_arena_strndup(&sh->arena,
								     lhs + groups[g].start, gl);
					if (!sub) {
						tc_error(sh, line, "out of memory");
						return TC_STATUS_USAGE;
					}
					(void)tc_var_set_elem_idx(&sh->vars, "BASH_REMATCH",
								  (int64_t)g, sub, false, 0);
				}
			}
		}
	}

	*out_val = (mr == TC_RE_MATCH);
	return 0;
}

static int eval_binary(struct tc_shell *sh, const struct tc_cond_expr *e,
		       bool *out_val)
{
	*out_val = false;
	char *lhs = NULL;
	int er = tc_expand_single(sh, &e->w1, false, &lhs);
	if (er != TC_EXP_OK)
		return TC_STATUS_USAGE;

	const char *op = e->op;
	if (strcmp(op, "==") == 0 || strcmp(op, "=") == 0 || strcmp(op, "!=") == 0) {
		struct tc_pat pat;
		er = tc_expand_pat(sh, &e->w2, &pat);
		if (er != TC_EXP_OK)
			return TC_STATUS_USAGE;
		bool m = tc_pat_match(&pat, lhs, strlen(lhs));
		*out_val = (op[0] == '!') ? !m : m;
		return 0;
	}

	if (strcmp(op, "=~") == 0)
		return eval_regex(sh, lhs, &e->w2, e->line, out_val);

	char *rhs = NULL;
	er = tc_expand_single(sh, &e->w2, false, &rhs);
	if (er != TC_EXP_OK)
		return TC_STATUS_USAGE;

	if (strcmp(op, "<") == 0) {
		*out_val = (strcmp(lhs, rhs) < 0);
		return 0;
	}
	if (strcmp(op, ">") == 0) {
		*out_val = (strcmp(lhs, rhs) > 0);
		return 0;
	}
	if (strcmp(op, "-ef") == 0) {
		struct stat sa, sb;
		if (stat(lhs, &sa) == 0 && stat(rhs, &sb) == 0)
			*out_val = (sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino);
		return 0;
	}
	if (strcmp(op, "-nt") == 0 || strcmp(op, "-ot") == 0) {
		struct stat sa, sb;
		bool ha = (stat(lhs, &sa) == 0);
		bool hb = (stat(rhs, &sb) == 0);
		if (op[1] == 'n') {
			if (!ha)
				*out_val = false;
			else if (!hb)
				*out_val = true;
			else if (sa.st_mtim.tv_sec != sb.st_mtim.tv_sec)
				*out_val = (sa.st_mtim.tv_sec > sb.st_mtim.tv_sec);
			else
				*out_val = (sa.st_mtim.tv_nsec > sb.st_mtim.tv_nsec);
		} else {
			if (!hb)
				*out_val = false;
			else if (!ha)
				*out_val = true;
			else if (sa.st_mtim.tv_sec != sb.st_mtim.tv_sec)
				*out_val = (sa.st_mtim.tv_sec < sb.st_mtim.tv_sec);
			else
				*out_val = (sa.st_mtim.tv_nsec < sb.st_mtim.tv_nsec);
		}
		return 0;
	}

	/* Arithmetic integer comparisons: -eq, -ne, -lt, -le, -gt, -ge. */
	int64_t a = 0, b = 0;
	if (tc_arith_eval(sh, lhs, e->line, &a) != TC_ARITH_OK ||
	    tc_arith_eval(sh, rhs, e->line, &b) != TC_ARITH_OK)
		return TC_STATUS_USAGE;

	if (strcmp(op, "-eq") == 0) *out_val = (a == b);
	else if (strcmp(op, "-ne") == 0) *out_val = (a != b);
	else if (strcmp(op, "-lt") == 0) *out_val = (a < b);
	else if (strcmp(op, "-le") == 0) *out_val = (a <= b);
	else if (strcmp(op, "-gt") == 0) *out_val = (a > b);
	else if (strcmp(op, "-ge") == 0) *out_val = (a >= b);
	else {
		tc_error(sh, e->line, "[[ ]]: %s: unknown binary operator", op);
		return TC_STATUS_USAGE;
	}
	return 0;
}

static int eval_expr(struct tc_shell *sh, const struct tc_cond_expr *e,
		     unsigned depth, bool *out_val)
{
	if (!e) {
		*out_val = false;
		return 0;
	}
	if (depth >= TC_COND_DEPTH_MAX) {
		tc_error(sh, e->line, "[[ ]]: expression nested too deeply");
		return TC_STATUS_USAGE;
	}

	switch (e->kind) {
	case TC_COND_OR: {
		bool lv = false;
		int r = eval_expr(sh, e->lhs, depth + 1, &lv);
		if (r != 0)
			return r;
		if (lv) {
			*out_val = true;
			return 0;
		}
		return eval_expr(sh, e->rhs, depth + 1, out_val);
	}
	case TC_COND_AND: {
		bool lv = false;
		int r = eval_expr(sh, e->lhs, depth + 1, &lv);
		if (r != 0)
			return r;
		if (!lv) {
			*out_val = false;
			return 0;
		}
		return eval_expr(sh, e->rhs, depth + 1, out_val);
	}
	case TC_COND_NOT: {
		bool v = false;
		int r = eval_expr(sh, e->lhs, depth + 1, &v);
		if (r != 0)
			return r;
		*out_val = !v;
		return 0;
	}
	case TC_COND_UNARY: {
		char *arg = NULL;
		int er = tc_expand_single(sh, &e->w1, false, &arg);
		if (er != TC_EXP_OK)
			return TC_STATUS_USAGE;
		return eval_unary(sh, e->op, arg, e->line, out_val);
	}
	case TC_COND_BINARY:
		return eval_binary(sh, e, out_val);
	case TC_COND_WORD: {
		char *arg = NULL;
		int er = tc_expand_single(sh, &e->w1, false, &arg);
		if (er != TC_EXP_OK)
			return TC_STATUS_USAGE;
		*out_val = (arg[0] != '\0');
		return 0;
	}
	}
	return TC_STATUS_USAGE;
}

int tc_exec_cond(struct tc_shell *sh, const struct tc_node *node)
{
	bool val = false;
	int r = eval_expr(sh, node->u.cond.expr, 0, &val);
	if (r != 0)
		return r;
	return val ? 0 : 1;
}
