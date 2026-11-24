/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Static lint analyzer for `tam-clam --lint FILE...`.
 * Parses with the real parser, executes nothing, and reports risky
 * constructs with file:line:col and short check names. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tam-clam/ast.h"
#include "tam-clam/input.h"
#include "tam-clam/lex.h"
#include "tam-clam/lint.h"
#include "tam-clam/parse.h"
#include "tam-clam/var.h"

#define TC_LINT_FILE_MAX  ((size_t)16 << 20)
#define TC_LINT_DEPTH_MAX 256

struct lint_ctx {
	struct tc_shell *sh;
	const char *filename;
	const char *src;
	size_t src_len;
	size_t *line_starts;
	size_t nlines;
	int out_fd;
	char **defs;
	size_t ndefs;
	size_t def_cap;
	bool errexit_active;
	size_t warnings;
	unsigned depth;
};

static bool is_std_env_var(const char *name)
{
	static const char *const std_vars[] = {
		"CDPATH", "COLUMNS", "DISPLAY", "EDITOR", "ENV", "FUNCNEST",
		"HOME", "IFS", "LANG", "LC_ALL", "LC_COLLATE", "LC_CTYPE",
		"LC_MESSAGES", "LC_NUMERIC", "LC_TIME", "LINENO", "LINES",
		"LOGNAME", "MAIL", "MAILCHECK", "MAILPATH", "OLDPWD",
		"OPTARG", "OPTIND", "PAGER", "PATH", "PPID", "PS1", "PS2",
		"PS4", "PWD", "REPLY", "SHELL", "TERM", "TMPDIR", "TZ",
		"USER", "VISUAL", "WAYLAND_DISPLAY", "XDG_CACHE_HOME",
		"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_RUNTIME_DIR",
	};
	for (size_t i = 0; i < sizeof(std_vars) / sizeof(std_vars[0]); i++) {
		if (strcmp(name, std_vars[i]) == 0)
			return true;
	}
	return false;
}

static bool is_defined(const struct lint_ctx *ctx, const char *name)
{
	for (size_t i = 0; i < ctx->ndefs; i++) {
		if (strcmp(ctx->defs[i], name) == 0)
			return true;
	}
	return false;
}

static void add_def(struct lint_ctx *ctx, const char *name, size_t len)
{
	if (len == 0 || !tc_var_valid_name(name, len))
		return;
	for (size_t i = 0; i < ctx->ndefs; i++) {
		if (strlen(ctx->defs[i]) == len &&
		    memcmp(ctx->defs[i], name, len) == 0)
			return;
	}
	if (ctx->ndefs == ctx->def_cap) {
		size_t ncap = ctx->def_cap ? ctx->def_cap * 2 : 32;
		char **nd = realloc(ctx->defs, ncap * sizeof(*nd));
		if (!nd)
			return;
		ctx->defs = nd;
		ctx->def_cap = ncap;
	}
	char *copy = malloc(len + 1);
	if (!copy)
		return;
	memcpy(copy, name, len);
	copy[len] = '\0';
	ctx->defs[ctx->ndefs++] = copy;
}

/* ---- Suppression comment scanning (`# lint: allow <name>`) ------------ */

static const char *line_slice(const struct lint_ctx *ctx, unsigned long line,
			      size_t *out_len)
{
	if (line == 0 || line > ctx->nlines) {
		*out_len = 0;
		return "";
	}
	size_t start = ctx->line_starts[line - 1];
	size_t end = (line < ctx->nlines) ? ctx->line_starts[line] : ctx->src_len;
	while (end > start && (ctx->src[end - 1] == '\n' || ctx->src[end - 1] == '\r'))
		end--;
	*out_len = end - start;
	return ctx->src + start;
}

static bool comment_allows(const char *s, size_t len, const char *check_name)
{
	/* Find an unquoted '#' starting a comment on this line. */
	bool in_sq = false, in_dq = false;
	size_t hash = len;
	for (size_t i = 0; i < len; i++) {
		char c = s[i];
		if (c == '\\' && !in_sq && i + 1 < len) {
			i++;
			continue;
		}
		if (c == '\'' && !in_dq) {
			in_sq = !in_sq;
		} else if (c == '"' && !in_sq) {
			in_dq = !in_dq;
		} else if (c == '#' && !in_sq && !in_dq) {
			if (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t' ||
			    s[i - 1] == ';' || s[i - 1] == '&' || s[i - 1] == '|') {
				hash = i;
				break;
			}
		}
	}
	if (hash == len)
		return false;

	const char *p = s + hash + 1;
	const char *end = s + len;
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	static const char prefix[] = "lint:";
	if ((size_t)(end - p) < sizeof(prefix) - 1 ||
	    memcmp(p, prefix, sizeof(prefix) - 1) != 0)
		return false;
	p += sizeof(prefix) - 1;
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	static const char allow[] = "allow";
	if ((size_t)(end - p) < sizeof(allow) - 1 ||
	    memcmp(p, allow, sizeof(allow) - 1) != 0)
		return false;
	p += sizeof(allow) - 1;
	if (p < end && *p != ' ' && *p != '\t' && *p != ':')
		return false;
	if (p < end && *p == ':')
		p++;

	size_t clen = strlen(check_name);
	while (p < end) {
		while (p < end && (*p == ' ' || *p == '\t' || *p == ','))
			p++;
		const char *tok = p;
		while (p < end && *p != ' ' && *p != '\t' && *p != ',')
			p++;
		size_t tlen = (size_t)(p - tok);
		if ((tlen == clen && memcmp(tok, check_name, clen) == 0) ||
		    (tlen == 3 && memcmp(tok, "all", 3) == 0))
			return true;
	}
	return false;
}

static bool is_comment_only_line(const char *s, size_t len)
{
	size_t i = 0;
	while (i < len && (s[i] == ' ' || s[i] == '\t'))
		i++;
	return i < len && s[i] == '#';
}

static bool is_suppressed(const struct lint_ctx *ctx, unsigned long line,
			  const char *check_name)
{
	size_t len = 0;
	const char *cur = line_slice(ctx, line, &len);
	if (comment_allows(cur, len, check_name))
		return true;
	if (line > 1) {
		size_t plen = 0;
		const char *prev = line_slice(ctx, line - 1, &plen);
		if (is_comment_only_line(prev, plen) &&
		    comment_allows(prev, plen, check_name))
			return true;
	}
	return false;
}

static void emit_warn(struct lint_ctx *ctx, unsigned long line,
		      unsigned long col, const char *check_name,
		      const char *fmt, ...)
	__attribute__((format(printf, 5, 6)));

static void emit_warn(struct lint_ctx *ctx, unsigned long line,
		      unsigned long col, const char *check_name,
		      const char *fmt, ...)
{
	if (is_suppressed(ctx, line, check_name))
		return;
	ctx->warnings++;
	if (ctx->out_fd < 0)
		return;

	char buf[1024];
	int n = snprintf(buf, sizeof(buf), "%s:%lu:%lu: warning [%s]: ",
			 ctx->filename, line ? line : 1, col ? col : 1,
			 check_name);
	if (n < 0)
		return;
	size_t used = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;

	va_list ap;
	va_start(ap, fmt);
	n = vsnprintf(buf + used, sizeof(buf) - used, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	used += (size_t)n < sizeof(buf) - used ? (size_t)n : sizeof(buf) - used - 1;
	if (used == sizeof(buf) - 1)
		used--;
	buf[used++] = '\n';
	(void)tc_write_all(ctx->out_fd, buf, used);
}

/* ---- Pass 1: collect variable definitions ----------------------------- */

static void collect_defs_node(struct lint_ctx *ctx, const struct tc_node *n);

static void scan_arith_defs(struct lint_ctx *ctx, const char *s, size_t len)
{
	size_t i = 0;
	while (i < len) {
		if (isalpha((unsigned char)s[i]) || s[i] == '_') {
			size_t start = i;
			while (i < len && (isalnum((unsigned char)s[i]) || s[i] == '_'))
				i++;
			size_t nlen = i - start;
			size_t j = i;
			while (j < len && (s[j] == ' ' || s[j] == '\t'))
				j++;
			if (j < len) {
				bool is_asg = false;
				if (s[j] == '=' && (j + 1 >= len || s[j + 1] != '='))
					is_asg = true;
				else if (j + 1 < len && s[j + 1] == '=' &&
					 (s[j] == '+' || s[j] == '-' || s[j] == '*' ||
					  s[j] == '/' || s[j] == '%' || s[j] == '&' ||
					  s[j] == '^' || s[j] == '|'))
					is_asg = true;
				else if (j + 1 < len &&
					 ((s[j] == '+' && s[j + 1] == '+') ||
					  (s[j] == '-' && s[j + 1] == '-')))
					is_asg = true;
				if (is_asg)
					add_def(ctx, s + start, nlen);
			}
			continue;
		}
		if (i + 1 < len &&
		    ((s[i] == '+' && s[i + 1] == '+') ||
		     (s[i] == '-' && s[i + 1] == '-'))) {
			i += 2;
			while (i < len && (s[i] == ' ' || s[i] == '\t'))
				i++;
			if (i < len && (isalpha((unsigned char)s[i]) || s[i] == '_')) {
				size_t start = i;
				while (i < len && (isalnum((unsigned char)s[i]) || s[i] == '_'))
					i++;
				add_def(ctx, s + start, i - start);
			}
			continue;
		}
		i++;
	}
}

static void collect_defs_parts(struct lint_ctx *ctx, const struct tc_wpart *wp,
			       unsigned depth)
{
	if (depth > TC_LINT_DEPTH_MAX)
		return;
	for (; wp; wp = wp->next) {
		if (wp->kind == TC_WPART_PARAM) {
			if (wp->op == TC_PARAM_ASSIGN_DEFAULT && wp->text)
				add_def(ctx, wp->text, strlen(wp->text));
			if (wp->sub)
				collect_defs_parts(ctx, wp->sub, depth + 1);
			if (wp->arg)
				collect_defs_parts(ctx, wp->arg, depth + 1);
		} else if (wp->kind == TC_WPART_ARITH) {
			for (const struct tc_wpart *ap = wp->arg; ap; ap = ap->next) {
				if (ap->kind == TC_WPART_LITERAL || ap->kind == TC_WPART_QUOTED)
					scan_arith_defs(ctx, ap->text, ap->len);
			}
			if (wp->arg)
				collect_defs_parts(ctx, wp->arg, depth + 1);
		} else if (wp->kind == TC_WPART_CMDSUB && wp->cmd) {
			collect_defs_node(ctx, wp->cmd);
		}
	}
}

static bool word_is_literal(const struct tc_word *w, const char *lit)
{
	return !(w->flags & (TC_WORD_QUOTED | TC_WORD_DOLLAR | TC_WORD_CMDSUB |
			     TC_WORD_BACKQ | TC_WORD_ARITH)) &&
	       strcmp(w->text, lit) == 0;
}

static void collect_defs_redirs(struct lint_ctx *ctx, const struct tc_redir *r,
				size_t nr)
{
	for (size_t i = 0; i < nr; i++) {
		collect_defs_parts(ctx, r[i].target.parts, 0);
		if (!r[i].heredoc_quoted && r[i].heredoc_parts)
			collect_defs_parts(ctx, r[i].heredoc_parts, 0);
	}
}

static void collect_defs_assign(struct lint_ctx *ctx, const struct tc_assign *a)
{
	add_def(ctx, a->name, strlen(a->name));
	if (a->sub)
		collect_defs_parts(ctx, a->sub, 0);
	if (a->is_compound) {
		for (size_t j = 0; j < a->nitems; j++) {
			if (a->items[j].sub)
				collect_defs_parts(ctx, a->items[j].sub, 0);
			collect_defs_parts(ctx, a->items[j].value.parts, 0);
		}
	} else {
		collect_defs_parts(ctx, a->value.parts, 0);
	}
}

static void collect_defs_simple(struct lint_ctx *ctx, const struct tc_node *n)
{
	for (size_t i = 0; i < n->u.simple.nassigns; i++)
		collect_defs_assign(ctx, &n->u.simple.assigns[i]);
	for (size_t i = 0; i < n->u.simple.nwords; i++) {
		if (n->u.simple.words[i].decl_assign)
			collect_defs_assign(ctx, n->u.simple.words[i].decl_assign);
		else
			collect_defs_parts(ctx, n->u.simple.words[i].parts, 0);
	}
	collect_defs_redirs(ctx, n->u.simple.redirs, n->u.simple.nredirs);

	if (n->u.simple.nwords == 0)
		return;
	const struct tc_word *cmd = &n->u.simple.words[0];
	size_t nw = n->u.simple.nwords;

	if (word_is_literal(cmd, "export") || word_is_literal(cmd, "readonly") ||
	    word_is_literal(cmd, "declare") || word_is_literal(cmd, "typeset") ||
	    word_is_literal(cmd, "local")) {
		for (size_t i = 1; i < nw; i++) {
			if (n->u.simple.words[i].decl_assign)
				continue;
			const char *t = n->u.simple.words[i].text;
			if (t[0] == '-' || t[0] == '+')
				continue;
			const char *eq = strchr(t, '=');
			size_t nlen = eq ? (size_t)(eq - t) : strlen(t);
			if (nlen > 0 && t[nlen - 1] == '+')
				nlen--;
			add_def(ctx, t, nlen);
		}
	} else if (word_is_literal(cmd, "read")) {
		size_t i = 1;
		for (; i < nw; i++) {
			const char *t = n->u.simple.words[i].text;
			if (strcmp(t, "--") == 0) {
				i++;
				break;
			}
			if (t[0] != '-' || t[1] == '\0')
				break;
			if (strcmp(t, "-d") == 0 && i + 1 < nw)
				i++;
		}
		if (i >= nw) {
			add_def(ctx, "REPLY", 5);
		} else {
			for (; i < nw; i++)
				add_def(ctx, n->u.simple.words[i].text,
					n->u.simple.words[i].len);
		}
	} else if (word_is_literal(cmd, "getopts")) {
		if (nw >= 3) {
			add_def(ctx, n->u.simple.words[2].text,
				n->u.simple.words[2].len);
			add_def(ctx, "OPTARG", 6);
			add_def(ctx, "OPTIND", 6);
		}
	}
}

static void collect_defs_cond(struct lint_ctx *ctx, const struct tc_cond_expr *e,
			      unsigned depth)
{
	if (!e || depth > TC_LINT_DEPTH_MAX)
		return;
	if (e->lhs)
		collect_defs_cond(ctx, e->lhs, depth + 1);
	if (e->rhs)
		collect_defs_cond(ctx, e->rhs, depth + 1);
	if (e->kind == TC_COND_UNARY || e->kind == TC_COND_BINARY ||
	    e->kind == TC_COND_WORD)
		collect_defs_parts(ctx, e->w1.parts, 0);
	if (e->kind == TC_COND_BINARY) {
		collect_defs_parts(ctx, e->w2.parts, 0);
		if (e->op && strcmp(e->op, "=~") == 0)
			add_def(ctx, "BASH_REMATCH", 12);
	}
}

static void collect_defs_node(struct lint_ctx *ctx, const struct tc_node *n)
{
	if (!n || ctx->depth >= TC_LINT_DEPTH_MAX)
		return;
	ctx->depth++;
	while (n) {
		const struct tc_node *cur = n;
		n = NULL;
		if (cur->kind == TC_NODE_SEQ) {
			n = cur->u.seq.rest;
			cur = cur->u.seq.first;
		}
		switch (cur->kind) {
		case TC_NODE_SIMPLE:
			collect_defs_simple(ctx, cur);
			break;
		case TC_NODE_PIPELINE:
			for (size_t i = 0; i < cur->u.pipeline.ncmds; i++)
				collect_defs_node(ctx, cur->u.pipeline.cmds[i]);
			break;
		case TC_NODE_AND_OR:
			for (size_t i = 0; i < cur->u.and_or.nitems; i++)
				collect_defs_node(ctx, cur->u.and_or.items[i].cmd);
			break;
		case TC_NODE_REDIR:
			collect_defs_redirs(ctx, cur->u.redir.redirs,
					    cur->u.redir.nredirs);
			collect_defs_node(ctx, cur->u.redir.child);
			break;
		case TC_NODE_GROUP:
		case TC_NODE_SUBSHELL:
			collect_defs_node(ctx, cur->u.group.body);
			break;
		case TC_NODE_IF:
			for (size_t i = 0; i < cur->u.if_cmd.nbranches; i++) {
				collect_defs_node(ctx, cur->u.if_cmd.branches[i].cond);
				collect_defs_node(ctx, cur->u.if_cmd.branches[i].body);
			}
			collect_defs_node(ctx, cur->u.if_cmd.else_body);
			break;
		case TC_NODE_LOOP:
			collect_defs_node(ctx, cur->u.loop.cond);
			collect_defs_node(ctx, cur->u.loop.body);
			break;
		case TC_NODE_FOR:
			add_def(ctx, cur->u.for_cmd.name, strlen(cur->u.for_cmd.name));
			for (size_t i = 0; i < cur->u.for_cmd.nwords; i++)
				collect_defs_parts(ctx, cur->u.for_cmd.words[i].parts, 0);
			collect_defs_node(ctx, cur->u.for_cmd.body);
			break;
		case TC_NODE_CASE:
			collect_defs_parts(ctx, cur->u.case_cmd.word.parts, 0);
			for (size_t i = 0; i < cur->u.case_cmd.nitems; i++) {
				for (size_t j = 0; j < cur->u.case_cmd.items[i].npatterns; j++)
					collect_defs_parts(ctx,
						cur->u.case_cmd.items[i].patterns[j].parts, 0);
				collect_defs_node(ctx, cur->u.case_cmd.items[i].body);
			}
			break;
		case TC_NODE_FUNCDEF:
			collect_defs_node(ctx, cur->u.funcdef.body);
			break;
		case TC_NODE_BG:
			collect_defs_node(ctx, cur->u.bg.child);
			break;
		case TC_NODE_COND:
			collect_defs_cond(ctx, cur->u.cond.expr, 0);
			break;
		default:
			break;
		}
	}
	ctx->depth--;
}

/* ---- Pass 2: lint checks ---------------------------------------------- */

static void check_node(struct lint_ctx *ctx, const struct tc_node *n,
		       bool cond_checked, bool next_checks_status);

static bool word_has_unquoted_expand(const struct tc_word *w)
{
	for (const struct tc_wpart *wp = w->parts; wp; wp = wp->next) {
		if (!wp->quoted &&
		    (wp->kind == TC_WPART_PARAM || wp->kind == TC_WPART_CMDSUB))
			return true;
	}
	return false;
}

static bool cmdsub_runs_ls(const struct tc_node *cmd)
{
	if (!cmd)
		return false;
	if (cmd->kind == TC_NODE_SIMPLE && cmd->u.simple.nwords > 0)
		return word_is_literal(&cmd->u.simple.words[0], "ls");
	if (cmd->kind == TC_NODE_PIPELINE && cmd->u.pipeline.ncmds > 0)
		return cmdsub_runs_ls(cmd->u.pipeline.cmds[0]);
	return false;
}

static bool parts_use_qmark(const struct tc_wpart *wp, unsigned depth)
{
	if (depth > TC_LINT_DEPTH_MAX)
		return false;
	for (; wp; wp = wp->next) {
		if (wp->kind == TC_WPART_PARAM && wp->text &&
		    strcmp(wp->text, "?") == 0)
			return true;
		if (wp->arg && parts_use_qmark(wp->arg, depth + 1))
			return true;
	}
	return false;
}

static bool node_uses_qmark(const struct tc_node *n)
{
	if (!n)
		return false;
	if (n->kind == TC_NODE_SEQ)
		return node_uses_qmark(n->u.seq.first);
	if (n->kind == TC_NODE_AND_OR && n->u.and_or.nitems > 0)
		return node_uses_qmark(n->u.and_or.items[0].cmd);
	if (n->kind == TC_NODE_PIPELINE && n->u.pipeline.ncmds > 0)
		return node_uses_qmark(n->u.pipeline.cmds[0]);
	if (n->kind == TC_NODE_IF && n->u.if_cmd.nbranches > 0)
		return node_uses_qmark(n->u.if_cmd.branches[0].cond);
	if (n->kind == TC_NODE_SIMPLE) {
		for (size_t i = 0; i < n->u.simple.nassigns; i++)
			if (parts_use_qmark(n->u.simple.assigns[i].value.parts, 0))
				return true;
		for (size_t i = 0; i < n->u.simple.nwords; i++)
			if (parts_use_qmark(n->u.simple.words[i].parts, 0))
				return true;
	}
	return false;
}

static void check_parts(struct lint_ctx *ctx, const struct tc_wpart *wp,
			unsigned long line, unsigned long col,
			bool in_for_list, unsigned depth)
{
	if (depth > TC_LINT_DEPTH_MAX)
		return;
	for (; wp; wp = wp->next) {
		if (wp->kind == TC_WPART_PARAM && wp->text) {
			if (strcmp(wp->text, "*") == 0 &&
			    wp->op == TC_PARAM_PLAIN &&
			    (!wp->quoted || in_for_list)) {
				emit_warn(ctx, line, col, "unquoted-star",
					  "use \"$@\" instead of $* to preserve argument boundaries");
			}
			bool checks_unset =
				wp->op == TC_PARAM_PLAIN ||
				wp->op == TC_PARAM_LEN ||
				wp->op == TC_PARAM_TRIM_PREFIX_S ||
				wp->op == TC_PARAM_TRIM_PREFIX_L ||
				wp->op == TC_PARAM_TRIM_SUFFIX_S ||
				wp->op == TC_PARAM_TRIM_SUFFIX_L;
			if (checks_unset &&
			    tc_var_valid_name(wp->text, strlen(wp->text)) &&
			    !is_defined(ctx, wp->text) &&
			    !is_std_env_var(wp->text)) {
				emit_warn(ctx, line, col, "unset-var",
					  "variable '%s' is used but never set in this file",
					  wp->text);
			}
			if (wp->sub)
				check_parts(ctx, wp->sub, line, col, false, depth + 1);
			if (wp->arg)
				check_parts(ctx, wp->arg, line, col, false, depth + 1);
		} else if (wp->kind == TC_WPART_ARITH && wp->arg) {
			check_parts(ctx, wp->arg, line, col, false, depth + 1);
		} else if (wp->kind == TC_WPART_CMDSUB) {
			if (!wp->quoted && !in_for_list && cmdsub_runs_ls(wp->cmd)) {
				emit_warn(ctx, line, col, "split-cmdsub",
					  "unquoted $(ls ...) splits output on IFS and globs; use a glob instead");
			}
			if (wp->cmd)
				check_node(ctx, wp->cmd, false, false);
		}
	}
}

static void check_redirs(struct lint_ctx *ctx, const struct tc_redir *r, size_t nr)
{
	for (size_t i = 0; i < nr; i++) {
		check_parts(ctx, r[i].target.parts, r[i].target.line,
			    r[i].target.col, false, 0);
		if (!r[i].heredoc_quoted && r[i].heredoc_parts)
			check_parts(ctx, r[i].heredoc_parts, r[i].line, 1, false, 0);
	}
}

static void track_set_errexit(struct lint_ctx *ctx, const struct tc_node *n)
{
	if (n->u.simple.nwords < 2 || !word_is_literal(&n->u.simple.words[0], "set"))
		return;
	for (size_t i = 1; i < n->u.simple.nwords; i++) {
		const char *a = n->u.simple.words[i].text;
		if (strcmp(a, "--") == 0)
			break;
		if ((a[0] != '-' && a[0] != '+') || a[1] == '\0')
			break;
		bool enable = (a[0] == '-');
		for (const char *p = a + 1; *p; p++) {
			if (*p == 'e') {
				ctx->errexit_active = enable;
			} else if (*p == 'o') {
				const char *opt = p[1] ? p + 1
						       : (i + 1 < n->u.simple.nwords
							      ? n->u.simple.words[++i].text
							      : NULL);
				if (opt && strcmp(opt, "errexit") == 0)
					ctx->errexit_active = enable;
				break;
			}
		}
	}
}

static void check_simple(struct lint_ctx *ctx, const struct tc_node *n,
			 bool cond_checked, bool next_checks_status)
{
	for (size_t i = 0; i < n->u.simple.nassigns; i++) {
		const struct tc_assign *a = &n->u.simple.assigns[i];
		if (a->sub)
			check_parts(ctx, a->sub, a->line, a->col, false, 0);
		if (a->is_compound) {
			for (size_t j = 0; j < a->nitems; j++) {
				if (a->items[j].sub)
					check_parts(ctx, a->items[j].sub, a->items[j].line,
						    a->items[j].col, false, 0);
				check_parts(ctx, a->items[j].value.parts,
					    a->items[j].value.line, a->items[j].value.col,
					    false, 0);
			}
		} else {
			check_parts(ctx, a->value.parts, a->value.line, a->value.col, false, 0);
		}
	}

	size_t nw = n->u.simple.nwords;
	for (size_t i = 0; i < nw; i++) {
		const struct tc_word *w = &n->u.simple.words[i];
		if (w->decl_assign) {
			const struct tc_assign *a = w->decl_assign;
			for (size_t j = 0; j < a->nitems; j++) {
				if (a->items[j].sub)
					check_parts(ctx, a->items[j].sub, a->items[j].line,
						    a->items[j].col, false, 0);
				check_parts(ctx, a->items[j].value.parts,
					    a->items[j].value.line, a->items[j].value.col,
					    false, 0);
			}
		} else {
			check_parts(ctx, w->parts, w->line, w->col, false, 0);
		}
	}
	check_redirs(ctx, n->u.simple.redirs, n->u.simple.nredirs);

	if (nw == 0)
		return;

	const struct tc_word *cmd = &n->u.simple.words[0];
	track_set_errexit(ctx, n);

	if (word_is_literal(cmd, "[") || word_is_literal(cmd, "test")) {
		bool is_bracket = (cmd->text[0] == '[');
		size_t end = nw;
		if (is_bracket && nw > 1 &&
		    word_is_literal(&n->u.simple.words[nw - 1], "]"))
			end = nw - 1;
		for (size_t i = 1; i < end; i++) {
			const struct tc_word *w = &n->u.simple.words[i];
			if (word_has_unquoted_expand(w)) {
				emit_warn(ctx, w->line, w->col, "unquoted-test",
					  "unquoted expansion in %s; double-quote it to prevent splitting or empty-arg errors",
					  is_bracket ? "[ ]" : "test");
			}
		}
	} else if (word_is_literal(cmd, "cd")) {
		if (!cond_checked && !next_checks_status && !ctx->errexit_active) {
			emit_warn(ctx, n->line, n->col, "unchecked-cd",
				  "cd failure is not checked; use 'cd ... || exit' or 'cd ... || return'");
		}
	}
}

static void check_cond(struct lint_ctx *ctx, const struct tc_cond_expr *e,
		       unsigned depth)
{
	if (!e || depth > TC_LINT_DEPTH_MAX)
		return;
	if (e->lhs)
		check_cond(ctx, e->lhs, depth + 1);
	if (e->rhs)
		check_cond(ctx, e->rhs, depth + 1);
	if (e->kind == TC_COND_UNARY || e->kind == TC_COND_BINARY ||
	    e->kind == TC_COND_WORD)
		check_parts(ctx, e->w1.parts, e->w1.line, e->w1.col, false, 0);
	if (e->kind == TC_COND_BINARY)
		check_parts(ctx, e->w2.parts, e->w2.line, e->w2.col, false, 0);
}

static void check_node(struct lint_ctx *ctx, const struct tc_node *n,
		       bool cond_checked, bool next_checks_status)
{
	if (!n || ctx->depth >= TC_LINT_DEPTH_MAX)
		return;
	ctx->depth++;
	while (n) {
		const struct tc_node *cur = n;
		n = NULL;
		bool next_q = next_checks_status;
		if (cur->kind == TC_NODE_SEQ) {
			n = cur->u.seq.rest;
			cur = cur->u.seq.first;
			next_q = node_uses_qmark(n);
		}
		switch (cur->kind) {
		case TC_NODE_SIMPLE:
			check_simple(ctx, cur, cond_checked, next_q);
			break;
		case TC_NODE_PIPELINE: {
			bool neg = cur->u.pipeline.bang;
			for (size_t i = 0; i < cur->u.pipeline.ncmds; i++)
				check_node(ctx, cur->u.pipeline.cmds[i],
					   cond_checked || neg, false);
			break;
		}
		case TC_NODE_AND_OR:
			for (size_t i = 0; i < cur->u.and_or.nitems; i++) {
				bool not_last = (i + 1 < cur->u.and_or.nitems);
				check_node(ctx, cur->u.and_or.items[i].cmd,
					   cond_checked || not_last,
					   not_last ? false : next_q);
			}
			break;
		case TC_NODE_REDIR:
			check_redirs(ctx, cur->u.redir.redirs, cur->u.redir.nredirs);
			check_node(ctx, cur->u.redir.child, cond_checked, next_q);
			break;
		case TC_NODE_GROUP:
			check_node(ctx, cur->u.group.body, cond_checked, next_q);
			break;
		case TC_NODE_SUBSHELL: {
			bool saved_e = ctx->errexit_active;
			check_node(ctx, cur->u.group.body, false, false);
			ctx->errexit_active = saved_e;
			break;
		}
		case TC_NODE_IF:
			for (size_t i = 0; i < cur->u.if_cmd.nbranches; i++) {
				check_node(ctx, cur->u.if_cmd.branches[i].cond, true, false);
				check_node(ctx, cur->u.if_cmd.branches[i].body, false, false);
			}
			check_node(ctx, cur->u.if_cmd.else_body, false, false);
			break;
		case TC_NODE_LOOP:
			check_node(ctx, cur->u.loop.cond, true, false);
			check_node(ctx, cur->u.loop.body, false, false);
			break;
		case TC_NODE_FOR:
			for (size_t i = 0; i < cur->u.for_cmd.nwords; i++) {
				const struct tc_word *w = &cur->u.for_cmd.words[i];
				for (const struct tc_wpart *wp = w->parts; wp; wp = wp->next) {
					if (!wp->quoted && wp->kind == TC_WPART_CMDSUB) {
						emit_warn(ctx, w->line, w->col, "split-cmdsub",
							  "unquoted command substitution in 'for' splits output on IFS and globs; use a glob or 'while IFS= read -r'");
						break;
					}
				}
				check_parts(ctx, w->parts, w->line, w->col, true, 0);
			}
			check_node(ctx, cur->u.for_cmd.body, false, false);
			break;
		case TC_NODE_CASE:
			if (word_has_unquoted_expand(&cur->u.case_cmd.word)) {
				emit_warn(ctx, cur->u.case_cmd.word.line,
					  cur->u.case_cmd.word.col, "unquoted-case",
					  "unquoted expansion as case word; double-quote it for clarity and safety");
			}
			check_parts(ctx, cur->u.case_cmd.word.parts,
				    cur->u.case_cmd.word.line,
				    cur->u.case_cmd.word.col, false, 0);
			for (size_t i = 0; i < cur->u.case_cmd.nitems; i++) {
				for (size_t j = 0; j < cur->u.case_cmd.items[i].npatterns; j++) {
					const struct tc_word *pw =
						&cur->u.case_cmd.items[i].patterns[j];
					check_parts(ctx, pw->parts, pw->line, pw->col, false, 0);
				}
				check_node(ctx, cur->u.case_cmd.items[i].body, false, false);
			}
			break;
		case TC_NODE_FUNCDEF:
			check_node(ctx, cur->u.funcdef.body, false, false);
			break;
		case TC_NODE_BG:
			check_node(ctx, cur->u.bg.child, false, false);
			break;
		case TC_NODE_COND:
			check_cond(ctx, cur->u.cond.expr, 0);
			break;
		default:
			break;
		}
		cond_checked = false;
	}
	ctx->depth--;
}

/* ---- Entry points ----------------------------------------------------- */

static int build_line_index(const char *src, size_t len,
			    size_t **out_starts, size_t *out_nlines)
{
	size_t cap = 64, n = 0;
	size_t *starts = malloc(cap * sizeof(*starts));
	if (!starts)
		return -1;
	starts[n++] = 0;
	for (size_t i = 0; i < len; i++) {
		if (src[i] == '\n') {
			if (n == cap) {
				size_t ncap = cap * 2;
				size_t *ns = realloc(starts, ncap * sizeof(*ns));
				if (!ns) {
					free(starts);
					return -1;
				}
				starts = ns;
				cap = ncap;
			}
			starts[n++] = i + 1;
		}
	}
	*out_starts = starts;
	*out_nlines = n;
	return 0;
}

int tc_lint_string(struct tc_shell *sh, const char *filename,
		   const char *src, size_t len, int out_fd,
		   size_t *out_warnings)
{
	if (out_warnings)
		*out_warnings = 0;

	const char *saved_name = sh->name;
	sh->name = filename;

	struct lint_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.sh = sh;
	ctx.filename = filename;
	ctx.src = src;
	ctx.src_len = len;
	ctx.out_fd = out_fd;

	if (build_line_index(src, len, &ctx.line_starts, &ctx.nlines) < 0) {
		tc_error(sh, 0, "out of memory");
		sh->name = saved_name;
		return TC_STATUS_USAGE;
	}

	struct tc_input in;
	tc_input_from_string(&in, src, len);
	sh->in = &in;
	in.sh = sh;

	struct tc_parser p;
	if (tc_parser_init(&p, sh, &in) != TC_OK) {
		tc_error(sh, 0, "out of memory");
		free(ctx.line_starts);
		tc_input_finish(&in);
		sh->name = saved_name;
		return TC_STATUS_USAGE;
	}

	struct tc_node **roots = NULL;
	size_t nroots = 0, rcap = 0;
	int status = 0;

	for (;;) {
		struct tc_node *node = NULL;
		int r = tc_parse_next(&p, &node);
		if (r == TC_END)
			break;
		if (r != TC_OK) {
			status = TC_STATUS_USAGE;
			break;
		}
		if (!node)
			continue;
		if (nroots == rcap) {
			size_t ncap = rcap ? rcap * 2 : 32;
			struct tc_node **nr = realloc(roots, ncap * sizeof(*nr));
			if (!nr) {
				tc_error(sh, node->line, "out of memory");
				status = TC_STATUS_USAGE;
				break;
			}
			roots = nr;
			rcap = ncap;
		}
		roots[nroots++] = node;
	}

	tc_parser_finish(&p);
	tc_input_finish(&in);

	if (status == 0) {
		for (size_t i = 0; i < nroots; i++)
			collect_defs_node(&ctx, roots[i]);
		for (size_t i = 0; i < nroots; i++) {
			bool next_q = (i + 1 < nroots) && node_uses_qmark(roots[i + 1]);
			check_node(&ctx, roots[i], false, next_q);
		}
		if (ctx.warnings > 0)
			status = 1;
	}

	if (out_warnings)
		*out_warnings = ctx.warnings;

	for (size_t i = 0; i < ctx.ndefs; i++)
		free(ctx.defs[i]);
	free(ctx.defs);
	free(roots);
	free(ctx.line_starts);
	tc_arena_reset(&sh->arena);
	sh->name = saved_name;
	return status;
}

int tc_lint_file(struct tc_shell *sh, const char *path)
{
	int fd;
	bool close_fd = false;
	const char *label = path;

	if (strcmp(path, "-") == 0) {
		fd = STDIN_FILENO;
		label = "<stdin>";
	} else {
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			tc_error(sh, 0, "%s: %s", path, strerror(errno));
			return TC_STATUS_USAGE;
		}
		close_fd = true;
		struct stat st;
		if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
			tc_error(sh, 0, "%s: %s", path, strerror(EISDIR));
			close(fd);
			return TC_STATUS_USAGE;
		}
	}

	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (!buf) {
		if (close_fd)
			close(fd);
		tc_error(sh, 0, "out of memory");
		return TC_STATUS_USAGE;
	}

	for (;;) {
		if (len == cap) {
			if (cap >= TC_LINT_FILE_MAX) {
				tc_error(sh, 0, "%s: file exceeds %zu bytes",
					 label, TC_LINT_FILE_MAX);
				free(buf);
				if (close_fd)
					close(fd);
				return TC_STATUS_USAGE;
			}
			size_t ncap = cap * 2;
			if (ncap > TC_LINT_FILE_MAX)
				ncap = TC_LINT_FILE_MAX;
			char *nb = realloc(buf, ncap);
			if (!nb) {
				tc_error(sh, 0, "out of memory");
				free(buf);
				if (close_fd)
					close(fd);
				return TC_STATUS_USAGE;
			}
			buf = nb;
			cap = ncap;
		}
		ssize_t n = read(fd, buf + len, cap - len);
		if (n == 0)
			break;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			tc_error(sh, 0, "%s: %s", label, strerror(errno));
			free(buf);
			if (close_fd)
				close(fd);
			return TC_STATUS_USAGE;
		}
		len += (size_t)n;
	}
	if (close_fd)
		close(fd);

	int rc = tc_lint_string(sh, label, buf, len, STDOUT_FILENO, NULL);
	free(buf);
	return rc;
}
