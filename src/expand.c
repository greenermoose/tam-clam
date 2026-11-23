/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Word expansion, XCU 2.6: tilde expansion, parameter expansion, command
 * substitution, arithmetic expansion, IFS field splitting, pathname
 * expansion (globbing), and quote removal. */
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tam.h>

#include "tam-clam/arith.h"
#include "tam-clam/exec.h"
#include "tam-clam/expand.h"
#include "tam-clam/glob.h"
#include "tam-clam/lex.h"
#include "tam-clam/pattern.h"

/* Quote-removal only, used for here-document delimiters (XCU 2.7.4). */
char *tc_expand_delim(struct tc_arena *a, const struct tc_word *w)
{
	if ((w->flags & TC_WORD_ANSI_Q) && w->parts) {
		size_t total = 0;
		for (const struct tc_wpart *wp = w->parts; wp; wp = wp->next) {
			if (wp->kind == TC_WPART_LITERAL || wp->kind == TC_WPART_QUOTED)
				total += wp->len;
		}
		char *out = tc_arena_alloc(a, total + 1);
		if (!out)
			return NULL;
		char *o = out;
		for (const struct tc_wpart *wp = w->parts; wp; wp = wp->next) {
			if (wp->kind == TC_WPART_LITERAL || wp->kind == TC_WPART_QUOTED) {
				memcpy(o, wp->text, wp->len);
				o += wp->len;
			}
		}
		*o = '\0';
		return out;
	}

	char *out = tc_arena_alloc(a, w->len + 1);
	if (!out)
		return NULL;
	const char *s = w->text, *end = w->text + w->len;
	char *o = out;
	while (s < end) {
		char c = *s++;
		if (c == '\\') {
			if (s < end)
				*o++ = *s++;
		} else if (c == '\'') {
			while (s < end && *s != '\'')
				*o++ = *s++;
			if (s < end)
				s++;
		} else if (c == '"') {
			while (s < end && *s != '"') {
				if (*s == '\\' && s + 1 < end &&
				    (s[1] == '$' || s[1] == '`' || s[1] == '"' ||
				     s[1] == '\\' || s[1] == '\n'))
					s++;
				*o++ = *s++;
			}
			if (s < end)
				s++;
		} else {
			*o++ = c;
		}
	}
	*o = '\0';
	return out;
}

static int parse_pos_index(const char *s, size_t *out)
{
	if (!s || *s < '0' || *s > '9')
		return -1;
	size_t v = 0;
	for (; *s; s++) {
		if (*s < '0' || *s > '9')
			return -1;
		unsigned d = (unsigned)(*s - '0');
		if (v > (SIZE_MAX - d) / 10)
			return -1;
		v = v * 10 + d;
	}
	*out = v;
	return 0;
}

static bool is_special_or_pos(const char *name)
{
	if (!name || !name[0])
		return false;
	if (name[1] == '\0') {
		char c = name[0];
		if (c == '@' || c == '*' || c == '#' || c == '?' ||
		    c == '-' || c == '$' || c == '!')
			return true;
	}
	size_t idx;
	return parse_pos_index(name, &idx) == 0;
}

static int append_str(struct tc_shell *sh, unsigned long line,
		      tam_str_t *buf, const char *s, size_t len)
{
	if (len == 0)
		return TC_EXP_OK;
	tam_str_t next = tam_str_cat_len(*buf, s, len);
	if (!next) {
		tc_error(sh, line, "out of memory");
		return TC_EXP_NOMEM;
	}
	*buf = next;
	return TC_EXP_OK;
}

/* Join positional parameters with IFS[0] (or ' ' if IFS unset, "" if empty). */
static int join_star(struct tc_shell *sh, unsigned long line, tam_str_t *out)
{
	const char *ifs = tc_var_get(&sh->vars, "IFS");
	char sep = ' ';
	bool has_sep = true;
	if (ifs) {
		if (ifs[0] == '\0')
			has_sep = false;
		else
			sep = ifs[0];
	}
	for (size_t i = 0; i < sh->nparams; i++) {
		if (i > 0 && has_sep) {
			int r = append_str(sh, line, out, &sep, 1);
			if (r != TC_EXP_OK)
				return r;
		}
		int r = append_str(sh, line, out, sh->params[i], strlen(sh->params[i]));
		if (r != TC_EXP_OK)
			return r;
	}
	return TC_EXP_OK;
}

/* Lookup a parameter's value into *out_val (NULL if unset). Returns
 * TC_EXP_OK or TC_EXP_NOMEM. Strings are allocated in sh->arena when needed. */
static int lookup_param(struct tc_shell *sh, const char *name,
			unsigned long line, const char **out_val)
{
	*out_val = NULL;
	if (name[0] != '\0' && name[1] == '\0') {
		char num[32];
		switch (name[0]) {
		case '?':
			snprintf(num, sizeof(num), "%d", sh->status);
			*out_val = tc_arena_strndup(&sh->arena, num, strlen(num));
			return *out_val ? TC_EXP_OK : (tc_error(sh, line, "out of memory"), TC_EXP_NOMEM);
		case '$':
			snprintf(num, sizeof(num), "%ld", (long)sh->root_pid);
			*out_val = tc_arena_strndup(&sh->arena, num, strlen(num));
			return *out_val ? TC_EXP_OK : (tc_error(sh, line, "out of memory"), TC_EXP_NOMEM);
		case '!':
			if (sh->last_bg_pid <= 0)
				return TC_EXP_OK;
			snprintf(num, sizeof(num), "%ld", (long)sh->last_bg_pid);
			*out_val = tc_arena_strndup(&sh->arena, num, strlen(num));
			return *out_val ? TC_EXP_OK : (tc_error(sh, line, "out of memory"), TC_EXP_NOMEM);
		case '#':
			snprintf(num, sizeof(num), "%zu", sh->nparams);
			*out_val = tc_arena_strndup(&sh->arena, num, strlen(num));
			return *out_val ? TC_EXP_OK : (tc_error(sh, line, "out of memory"), TC_EXP_NOMEM);
		case '-':
			*out_val = sh->interactive ? "i" : "";
			return TC_EXP_OK;
		case '0':
			*out_val = sh->name ? sh->name : "";
			return TC_EXP_OK;
		case '@':
		case '*': {
			if (sh->nparams == 0)
				return TC_EXP_OK;
			tam_str_t tmp = tam_str_new("");
			if (!tmp) {
				tc_error(sh, line, "out of memory");
				return TC_EXP_NOMEM;
			}
			int r = join_star(sh, line, &tmp);
			if (r != TC_EXP_OK) {
				tam_str_free(tmp);
				return r;
			}
			*out_val = tc_arena_strndup(&sh->arena, tmp, tam_str_len(tmp));
			tam_str_free(tmp);
			if (!*out_val) {
				tc_error(sh, line, "out of memory");
				return TC_EXP_NOMEM;
			}
			return TC_EXP_OK;
		}
		default:
			break;
		}
	}

	size_t idx;
	if (parse_pos_index(name, &idx) == 0) {
		if (idx == 0)
			*out_val = sh->name ? sh->name : "";
		else if (idx <= sh->nparams)
			*out_val = sh->params[idx - 1];
		return TC_EXP_OK;
	}

	*out_val = tc_var_get(&sh->vars, name);
	return TC_EXP_OK;
}

static int expand_parts_single(struct tc_shell *sh, const struct tc_wpart *parts,
			       unsigned long line, bool is_assign,
			       bool allow_tilde, int depth, char **out);

static int expand_parts_pat(struct tc_shell *sh, const struct tc_wpart *parts,
			    unsigned long line, bool allow_tilde, int depth,
			    struct tc_pat *out);

static const char *tilde_lookup(struct tc_shell *sh, const char *user, size_t ulen)
{
	if (ulen == 0)
		return tc_var_get(&sh->vars, "HOME");
	char *uname = tc_arena_strndup(&sh->arena, user, ulen);
	if (!uname)
		return NULL;
	struct passwd *pw = getpwnam(uname);
	if (!pw || !pw->pw_dir)
		return NULL;
	return tc_arena_strndup(&sh->arena, pw->pw_dir, strlen(pw->pw_dir));
}

/* Expand a literal segment with optional tilde expansion at start (and after
 * ':' when is_assign is true). */
static int append_literal_tilde(struct tc_shell *sh, unsigned long line,
				const struct tc_wpart *wp, bool at_word_start,
				bool is_assign, tam_str_t *buf)
{
	const char *s = wp->text;
	size_t len = wp->len;
	size_t i = 0;

	while (i < len) {
		bool can_tilde = (i == 0 && at_word_start) ||
				 (is_assign && i > 0 && s[i - 1] == ':');
		if (can_tilde && s[i] == '~') {
			size_t j = i + 1;
			while (j < len && s[j] != '/' && !(is_assign && s[j] == ':'))
				j++;
			bool blocked = (j == len && wp->next != NULL &&
					wp->next->kind != TC_WPART_LITERAL);
			if (!blocked) {
				const char *dir = tilde_lookup(sh, s + i + 1, j - (i + 1));
				if (dir) {
					int r = append_str(sh, line, buf, dir, strlen(dir));
					if (r != TC_EXP_OK)
						return r;
					i = j;
					continue;
				}
			}
		}
		int r = append_str(sh, line, buf, &s[i], 1);
		if (r != TC_EXP_OK)
			return r;
		i++;
	}
	return TC_EXP_OK;
}

static int eval_cmdsub_part(struct tc_shell *sh, const struct tc_wpart *wp,
			    unsigned long line, char **out)
{
	size_t len = 0;
	int r = tc_exec_cmdsub(sh, wp->cmd, line, out, &len);
	if (r == TC_OK)
		return TC_EXP_OK;
	return r == TC_ERR_NOMEM ? TC_EXP_NOMEM : TC_EXP_ERR;
}

static int eval_arith_part(struct tc_shell *sh, const struct tc_wpart *wp,
			   unsigned long line, int depth, char **out)
{
	if (depth > TC_PARAM_DEPTH_MAX) {
		tc_error(sh, line, "arithmetic expansion nested too deeply");
		return TC_EXP_ERR;
	}
	char *expr = NULL;
	int r = expand_parts_single(sh, wp->arg, line, false, false, depth + 1, &expr);
	if (r != TC_EXP_OK)
		return r;
	int64_t val = 0;
	int ar = tc_arith_eval(sh, expr, line, &val);
	if (ar == TC_ARITH_NOMEM)
		return TC_EXP_NOMEM;
	if (ar != TC_ARITH_OK)
		return TC_EXP_ERR;
	char num[32];
	snprintf(num, sizeof(num), "%" PRId64, val);
	*out = tc_arena_strndup(&sh->arena, num, strlen(num));
	if (!*out) {
		tc_error(sh, line, "out of memory");
		return TC_EXP_NOMEM;
	}
	return TC_EXP_OK;
}

static int eval_param_single(struct tc_shell *sh, const struct tc_wpart *wp,
			     unsigned long line, int depth, char **out)
{
	if (depth > TC_PARAM_DEPTH_MAX) {
		tc_error(sh, line, "parameter expansion nested too deeply");
		return TC_EXP_ERR;
	}

	const char *val = NULL;
	int r = lookup_param(sh, wp->text, line, &val);
	if (r != TC_EXP_OK)
		return r;

	if (wp->op == TC_PARAM_LEN) {
		size_t n;
		if (strcmp(wp->text, "@") == 0 || strcmp(wp->text, "*") == 0)
			n = sh->nparams;
		else
			n = val ? strlen(val) : 0;
		char num[32];
		snprintf(num, sizeof(num), "%zu", n);
		*out = tc_arena_strndup(&sh->arena, num, strlen(num));
		if (!*out) {
			tc_error(sh, line, "out of memory");
			return TC_EXP_NOMEM;
		}
		return TC_EXP_OK;
	}

	if (wp->op == TC_PARAM_PLAIN) {
		const char *s = val ? val : "";
		*out = tc_arena_strndup(&sh->arena, s, strlen(s));
		if (!*out) {
			tc_error(sh, line, "out of memory");
			return TC_EXP_NOMEM;
		}
		return TC_EXP_OK;
	}

	bool is_set = (val != NULL);
	bool is_nonempty = (val != NULL && val[0] != '\0');
	bool active = wp->colon ? is_nonempty : is_set;

	switch (wp->op) {
	case TC_PARAM_USE_DEFAULT:
		if (active) {
			*out = tc_arena_strndup(&sh->arena, val, strlen(val));
			return *out ? TC_EXP_OK : (tc_error(sh, line, "out of memory"), TC_EXP_NOMEM);
		}
		return expand_parts_single(sh, wp->arg, line, false, !wp->quoted, depth + 1, out);

	case TC_PARAM_ASSIGN_DEFAULT:
		if (active) {
			*out = tc_arena_strndup(&sh->arena, val, strlen(val));
			return *out ? TC_EXP_OK : (tc_error(sh, line, "out of memory"), TC_EXP_NOMEM);
		}
		if (is_special_or_pos(wp->text)) {
			tc_error(sh, line, "%s: cannot assign to special or positional parameter",
				 wp->text);
			return TC_EXP_ERR;
		}
		{
			char *def = NULL;
			r = expand_parts_single(sh, wp->arg, line, false, !wp->quoted, depth + 1, &def);
			if (r != TC_EXP_OK)
				return r;
			int sr = tc_var_set(&sh->vars, wp->text, def, 0);
			if (sr == TC_VAR_ERR_READONLY) {
				tc_error(sh, line, "%s: readonly variable", wp->text);
				return TC_EXP_ERR;
			}
			if (sr != TC_VAR_OK) {
				tc_error(sh, line, "out of memory");
				return TC_EXP_NOMEM;
			}
			*out = def;
			return TC_EXP_OK;
		}

	case TC_PARAM_ERROR:
		if (active) {
			*out = tc_arena_strndup(&sh->arena, val, strlen(val));
			return *out ? TC_EXP_OK : (tc_error(sh, line, "out of memory"), TC_EXP_NOMEM);
		}
		{
			char *msg = NULL;
			r = expand_parts_single(sh, wp->arg, line, false, !wp->quoted, depth + 1, &msg);
			if (r != TC_EXP_OK)
				return r;
			if (msg && msg[0] != '\0')
				tc_error(sh, line, "%s: %s", wp->text, msg);
			else
				tc_error(sh, line, "%s: parameter %s", wp->text,
					 is_set ? "null or not set" : "not set");
			return TC_EXP_ERR;
		}

	case TC_PARAM_USE_ALT:
		if (!active) {
			*out = tc_arena_strndup(&sh->arena, "", 0);
			return *out ? TC_EXP_OK : (tc_error(sh, line, "out of memory"), TC_EXP_NOMEM);
		}
		return expand_parts_single(sh, wp->arg, line, false, !wp->quoted, depth + 1, out);

	case TC_PARAM_TRIM_SUFFIX_S:
	case TC_PARAM_TRIM_SUFFIX_L:
	case TC_PARAM_TRIM_PREFIX_S:
	case TC_PARAM_TRIM_PREFIX_L: {
		if (!val) {
			*out = tc_arena_strndup(&sh->arena, "", 0);
			return *out ? TC_EXP_OK : (tc_error(sh, line, "out of memory"), TC_EXP_NOMEM);
		}
		struct tc_pat pat;
		r = expand_parts_pat(sh, wp->arg, line, !wp->quoted, depth + 1, &pat);
		if (r != TC_EXP_OK)
			return r;
		size_t vlen = strlen(val);
		if (wp->op == TC_PARAM_TRIM_PREFIX_S || wp->op == TC_PARAM_TRIM_PREFIX_L) {
			size_t cut = tc_pat_trim_prefix(&pat, val, vlen,
							wp->op == TC_PARAM_TRIM_PREFIX_L);
			*out = tc_arena_strndup(&sh->arena, val + cut, vlen - cut);
		} else {
			size_t cut = tc_pat_trim_suffix(&pat, val, vlen,
							wp->op == TC_PARAM_TRIM_SUFFIX_L);
			*out = tc_arena_strndup(&sh->arena, val, vlen - cut);
		}
		if (!*out) {
			tc_error(sh, line, "out of memory");
			return TC_EXP_NOMEM;
		}
		return TC_EXP_OK;
	}

	default:
		*out = tc_arena_strndup(&sh->arena, "", 0);
		return *out ? TC_EXP_OK : TC_EXP_NOMEM;
	}
}

static int expand_parts_single(struct tc_shell *sh, const struct tc_wpart *parts,
			       unsigned long line, bool is_assign,
			       bool allow_tilde, int depth, char **out)
{
	tam_str_t buf = tam_str_new("");
	if (!buf) {
		tc_error(sh, line, "out of memory");
		return TC_EXP_NOMEM;
	}
	int r = TC_EXP_OK;
	bool at_start = allow_tilde;

	for (const struct tc_wpart *wp = parts; wp; wp = wp->next) {
		if (wp->kind == TC_WPART_LITERAL) {
			if (allow_tilde)
				r = append_literal_tilde(sh, line, wp, at_start, is_assign, &buf);
			else
				r = append_str(sh, line, &buf, wp->text, wp->len);
		} else if (wp->kind == TC_WPART_QUOTED) {
			r = append_str(sh, line, &buf, wp->text, wp->len);
		} else if (wp->kind == TC_WPART_PARAM) {
			char *pv = NULL;
			r = eval_param_single(sh, wp, line, depth, &pv);
			if (r == TC_EXP_OK)
				r = append_str(sh, line, &buf, pv, strlen(pv));
		} else if (wp->kind == TC_WPART_CMDSUB) {
			char *cv = NULL;
			r = eval_cmdsub_part(sh, wp, line, &cv);
			if (r == TC_EXP_OK)
				r = append_str(sh, line, &buf, cv, strlen(cv));
		} else if (wp->kind == TC_WPART_ARITH) {
			char *av = NULL;
			r = eval_arith_part(sh, wp, line, depth, &av);
			if (r == TC_EXP_OK)
				r = append_str(sh, line, &buf, av, strlen(av));
		}
		if (r != TC_EXP_OK) {
			tam_str_free(buf);
			return r;
		}
		at_start = false;
	}

	*out = tc_arena_strndup(&sh->arena, buf, tam_str_len(buf));
	tam_str_free(buf);
	if (!*out) {
		tc_error(sh, line, "out of memory");
		return TC_EXP_NOMEM;
	}
	return TC_EXP_OK;
}

/* Growable (text, quoted) buffer for building patterns and IFS fields. */
struct pat_buf {
	char *text;
	uint8_t *quoted;
	size_t len;
	size_t cap;
};

static void pb_init(struct pat_buf *pb)
{
	pb->text = NULL;
	pb->quoted = NULL;
	pb->len = 0;
	pb->cap = 0;
}

static void pb_finish(struct pat_buf *pb)
{
	free(pb->text);
	free(pb->quoted);
	pb->text = NULL;
	pb->quoted = NULL;
	pb->len = pb->cap = 0;
}

static int pb_append(struct tc_shell *sh, unsigned long line,
		     struct pat_buf *pb, const char *s, size_t len, uint8_t q)
{
	if (len == 0)
		return TC_EXP_OK;
	if (pb->len + len > pb->cap) {
		size_t ncap = pb->cap ? pb->cap * 2 : 64;
		while (ncap < pb->len + len)
			ncap *= 2;
		char *nt = realloc(pb->text, ncap);
		if (!nt) {
			tc_error(sh, line, "out of memory");
			return TC_EXP_NOMEM;
		}
		pb->text = nt;
		uint8_t *nq = realloc(pb->quoted, ncap);
		if (!nq) {
			tc_error(sh, line, "out of memory");
			return TC_EXP_NOMEM;
		}
		pb->quoted = nq;
		pb->cap = ncap;
	}
	memcpy(pb->text + pb->len, s, len);
	memset(pb->quoted + pb->len, q, len);
	pb->len += len;
	return TC_EXP_OK;
}

static int pb_to_arena(struct tc_shell *sh, unsigned long line,
		       const struct pat_buf *pb, struct tc_pat *out)
{
	char *t = tc_arena_strndup(&sh->arena, pb->text ? pb->text : "", pb->len);
	uint8_t *q = NULL;
	if (pb->len > 0) {
		q = tc_arena_alloc(&sh->arena, pb->len);
		if (q)
			memcpy(q, pb->quoted, pb->len);
	}
	if (!t || (pb->len > 0 && !q)) {
		tc_error(sh, line, "out of memory");
		return TC_EXP_NOMEM;
	}
	out->text = t;
	out->quoted = q;
	out->len = pb->len;
	return TC_EXP_OK;
}

static int pb_append_literal_tilde(struct tc_shell *sh, unsigned long line,
				   const struct tc_wpart *wp,
				   bool at_word_start, struct pat_buf *pb)
{
	const char *s = wp->text;
	size_t len = wp->len;
	if (at_word_start && len > 0 && s[0] == '~') {
		size_t j = 1;
		while (j < len && s[j] != '/')
			j++;
		bool blocked = (j == len && wp->next != NULL &&
				wp->next->kind != TC_WPART_LITERAL);
		if (!blocked) {
			const char *dir = tilde_lookup(sh, s + 1, j - 1);
			if (dir) {
				/* Tilde expansion result is quoted (literal) in patterns. */
				int r = pb_append(sh, line, pb, dir, strlen(dir), 1);
				if (r != TC_EXP_OK)
					return r;
				return pb_append(sh, line, pb, s + j, len - j, 0);
			}
		}
	}
	return pb_append(sh, line, pb, s, len, 0);
}

static int expand_parts_pat(struct tc_shell *sh, const struct tc_wpart *parts,
			    unsigned long line, bool allow_tilde, int depth,
			    struct tc_pat *out)
{
	struct pat_buf pb;
	pb_init(&pb);
	int r = TC_EXP_OK;
	bool at_start = allow_tilde;

	for (const struct tc_wpart *wp = parts; wp; wp = wp->next) {
		if (wp->kind == TC_WPART_LITERAL) {
			if (allow_tilde)
				r = pb_append_literal_tilde(sh, line, wp, at_start, &pb);
			else
				r = pb_append(sh, line, &pb, wp->text, wp->len, 0);
		} else if (wp->kind == TC_WPART_QUOTED) {
			r = pb_append(sh, line, &pb, wp->text, wp->len, 1);
		} else if (wp->kind == TC_WPART_PARAM) {
			char *pv = NULL;
			r = eval_param_single(sh, wp, line, depth, &pv);
			if (r == TC_EXP_OK)
				r = pb_append(sh, line, &pb, pv, strlen(pv),
					      wp->quoted ? 1 : 0);
		} else if (wp->kind == TC_WPART_CMDSUB) {
			char *cv = NULL;
			r = eval_cmdsub_part(sh, wp, line, &cv);
			if (r == TC_EXP_OK)
				r = pb_append(sh, line, &pb, cv, strlen(cv),
					      wp->quoted ? 1 : 0);
		} else if (wp->kind == TC_WPART_ARITH) {
			char *av = NULL;
			r = eval_arith_part(sh, wp, line, depth, &av);
			if (r == TC_EXP_OK)
				r = pb_append(sh, line, &pb, av, strlen(av),
					      wp->quoted ? 1 : 0);
		}
		if (r != TC_EXP_OK) {
			pb_finish(&pb);
			return r;
		}
		at_start = false;
	}

	r = pb_to_arena(sh, line, &pb, out);
	pb_finish(&pb);
	return r;
}

int tc_expand_pat(struct tc_shell *sh, const struct tc_word *w,
		  struct tc_pat *out)
{
	return expand_parts_pat(sh, w->parts, w->line, true, 0, out);
}

int tc_expand_single(struct tc_shell *sh, const struct tc_word *w,
		     bool is_assign, char **out)
{
	return expand_parts_single(sh, w->parts, w->line, is_assign, true, 0, out);
}

int tc_expand_heredoc(struct tc_shell *sh, const struct tc_wpart *parts,
		      unsigned long line, char **out, size_t *out_len)
{
	char *s = NULL;
	int r = expand_parts_single(sh, parts, line, false, false, 0, &s);
	if (r != TC_EXP_OK)
		return r;
	*out = s;
	*out_len = strlen(s);
	return TC_EXP_OK;
}

struct arg_link {
	char *arg;
	struct arg_link *next;
};

static int push_arg(struct tc_shell *sh, unsigned long line,
		    struct arg_link ***tail, size_t *count,
		    const char *s, size_t len)
{
	struct arg_link *al = tc_arena_alloc(&sh->arena, sizeof(*al));
	char *copy = tc_arena_strndup(&sh->arena, s, len);
	if (!al || !copy) {
		tc_error(sh, line, "out of memory");
		return TC_EXP_NOMEM;
	}
	al->arg = copy;
	al->next = NULL;
	**tail = al;
	*tail = &al->next;
	(*count)++;
	return TC_EXP_OK;
}

struct field_link {
	struct tc_pat pat;
	struct field_link *next;
};

struct field_state {
	struct tc_shell *sh;
	unsigned long line;
	const char *ifs;
	struct pat_buf cur;
	bool has_quoted;
	bool seen_delim_ws;
	struct field_link *head;
	struct field_link **tail;
};

static int fs_flush(struct field_state *fs)
{
	struct field_link *fl = tc_arena_alloc(&fs->sh->arena, sizeof(*fl));
	if (!fl) {
		tc_error(fs->sh, fs->line, "out of memory");
		return TC_EXP_NOMEM;
	}
	int r = pb_to_arena(fs->sh, fs->line, &fs->cur, &fl->pat);
	if (r != TC_EXP_OK)
		return r;
	fl->next = NULL;
	*fs->tail = fl;
	fs->tail = &fl->next;
	fs->cur.len = 0;
	fs->has_quoted = false;
	return TC_EXP_OK;
}

static bool is_ifs_ws(const char *ifs, char c)
{
	return (c == ' ' || c == '\t' || c == '\n') && strchr(ifs, c) != NULL;
}

static bool is_ifs_nws(const char *ifs, char c)
{
	return c != '\0' && !is_ifs_ws(ifs, c) && strchr(ifs, c) != NULL;
}

/* Feed an unquoted expansion result through POSIX IFS field splitting
 * (XCU 2.6.5). */
static int fs_feed_splittable(struct field_state *fs, const char *s, size_t len)
{
	if (len == 0)
		return TC_EXP_OK;
	if (fs->ifs[0] == '\0') {
		fs->seen_delim_ws = false;
		return pb_append(fs->sh, fs->line, &fs->cur, s, len, 0);
	}

	for (size_t i = 0; i < len; i++) {
		char c = s[i];
		if (is_ifs_ws(fs->ifs, c)) {
			if (fs->cur.len > 0 || fs->has_quoted) {
				int r = fs_flush(fs);
				if (r != TC_EXP_OK)
					return r;
				fs->seen_delim_ws = true;
			}
		} else if (is_ifs_nws(fs->ifs, c)) {
			if (fs->cur.len > 0 || fs->has_quoted) {
				int r = fs_flush(fs);
				if (r != TC_EXP_OK)
					return r;
				fs->seen_delim_ws = false;
			} else if (fs->seen_delim_ws) {
				/* Absorb one non-ws IFS char immediately after ws delimiter. */
				fs->seen_delim_ws = false;
			} else {
				/* Consecutive or leading non-ws IFS delimiter -> empty field. */
				int r = fs_flush(fs);
				if (r != TC_EXP_OK)
					return r;
			}
		} else {
			fs->seen_delim_ws = false;
			int r = pb_append(fs->sh, fs->line, &fs->cur, &c, 1, 0);
			if (r != TC_EXP_OK)
				return r;
		}
	}
	return TC_EXP_OK;
}

/* Expand word parts directly into fs, preserving quoting and IFS splitting
 * through unquoted ${x:-word} and ${x:+word}. */
static int expand_parts_fields(struct field_state *fs,
			       const struct tc_wpart *parts,
			       bool allow_tilde, int depth)
{
	struct tc_shell *sh = fs->sh;
	unsigned long line = fs->line;
	if (depth > TC_PARAM_DEPTH_MAX) {
		tc_error(sh, line, "parameter expansion nested too deeply");
		return TC_EXP_ERR;
	}
	bool at_start = allow_tilde;
	int r = TC_EXP_OK;

	for (const struct tc_wpart *wp = parts; wp; wp = wp->next) {
		if (wp->kind == TC_WPART_LITERAL) {
			fs->seen_delim_ws = false;
			if (allow_tilde)
				r = pb_append_literal_tilde(sh, line, wp, at_start, &fs->cur);
			else
				r = pb_append(sh, line, &fs->cur, wp->text, wp->len, 0);
		} else if (wp->kind == TC_WPART_QUOTED) {
			fs->seen_delim_ws = false;
			fs->has_quoted = true;
			r = pb_append(sh, line, &fs->cur, wp->text, wp->len, 1);
		} else if (wp->kind == TC_WPART_PARAM) {
			bool is_at = (strcmp(wp->text, "@") == 0);
			bool is_star = (strcmp(wp->text, "*") == 0);
			bool split_params = (wp->op == TC_PARAM_PLAIN) &&
					    (is_at || (is_star && !wp->quoted));
			if (split_params) {
				if (sh->nparams == 0) {
					at_start = false;
					continue;
				}
				if (wp->quoted) {
					fs->seen_delim_ws = false;
					for (size_t i = 0; i < sh->nparams; i++) {
						if (i > 0) {
							r = fs_flush(fs);
							if (r != TC_EXP_OK)
								break;
						}
						fs->has_quoted = true;
						r = pb_append(sh, line, &fs->cur,
							      sh->params[i],
							      strlen(sh->params[i]), 1);
						if (r != TC_EXP_OK)
							break;
					}
				} else {
					for (size_t i = 0; i < sh->nparams; i++) {
						if (i > 0 && (fs->cur.len > 0 || fs->has_quoted)) {
							r = fs_flush(fs);
							if (r != TC_EXP_OK)
								break;
							fs->seen_delim_ws = false;
						}
						r = fs_feed_splittable(fs, sh->params[i],
								       strlen(sh->params[i]));
						if (r != TC_EXP_OK)
							break;
					}
				}
			} else if (!wp->quoted &&
				   (wp->op == TC_PARAM_USE_DEFAULT ||
				    wp->op == TC_PARAM_USE_ALT)) {
				const char *val = NULL;
				r = lookup_param(sh, wp->text, line, &val);
				if (r == TC_EXP_OK) {
					bool is_set = (val != NULL);
					bool is_nonempty = (val != NULL && val[0] != '\0');
					bool active = wp->colon ? is_nonempty : is_set;
					if (wp->op == TC_PARAM_USE_DEFAULT) {
						if (active)
							r = fs_feed_splittable(fs, val, strlen(val));
						else
							r = expand_parts_fields(fs, wp->arg, true, depth + 1);
					} else {
						if (active)
							r = expand_parts_fields(fs, wp->arg, true, depth + 1);
					}
				}
			} else {
				char *pv = NULL;
				r = eval_param_single(sh, wp, line, depth, &pv);
				if (r == TC_EXP_OK) {
					if (wp->quoted) {
						fs->seen_delim_ws = false;
						fs->has_quoted = true;
						r = pb_append(sh, line, &fs->cur,
							      pv, strlen(pv), 1);
					} else {
						r = fs_feed_splittable(fs, pv, strlen(pv));
					}
				}
			}
		} else if (wp->kind == TC_WPART_CMDSUB) {
			char *cv = NULL;
			r = eval_cmdsub_part(sh, wp, line, &cv);
			if (r == TC_EXP_OK) {
				if (wp->quoted) {
					fs->seen_delim_ws = false;
					fs->has_quoted = true;
					r = pb_append(sh, line, &fs->cur,
						      cv, strlen(cv), 1);
				} else {
					r = fs_feed_splittable(fs, cv, strlen(cv));
				}
			}
		} else if (wp->kind == TC_WPART_ARITH) {
			char *av = NULL;
			r = eval_arith_part(sh, wp, line, depth, &av);
			if (r == TC_EXP_OK) {
				if (wp->quoted) {
					fs->seen_delim_ws = false;
					fs->has_quoted = true;
					r = pb_append(sh, line, &fs->cur,
						      av, strlen(av), 1);
				} else {
					r = fs_feed_splittable(fs, av, strlen(av));
				}
			}
		}
		if (r != TC_EXP_OK)
			return r;
		at_start = false;
	}
	return TC_EXP_OK;
}

/* Expand one word into 0 or more arguments (tilde, param, cmdsub, arith,
 * IFS field splitting, pathname expansion, quote removal). */
static int expand_one_word(struct tc_shell *sh, const struct tc_word *w,
			   struct arg_link ***tail, size_t *count)
{
	const char *ifs = tc_var_get(&sh->vars, "IFS");
	if (!ifs)
		ifs = " \t\n";

	struct field_state fs;
	fs.sh = sh;
	fs.line = w->line;
	fs.ifs = ifs;
	pb_init(&fs.cur);
	fs.has_quoted = false;
	fs.seen_delim_ws = false;
	fs.head = NULL;
	fs.tail = &fs.head;

	int r = expand_parts_fields(&fs, w->parts, true, 0);
	if (r != TC_EXP_OK) {
		pb_finish(&fs.cur);
		return r;
	}

	if (fs.cur.len > 0 || fs.has_quoted)
		r = fs_flush(&fs);
	pb_finish(&fs.cur);
	if (r != TC_EXP_OK)
		return r;

	/* Pathname expansion (globbing) and quote removal on each field. */
	for (struct field_link *fl = fs.head; fl; fl = fl->next) {
		if (tc_pat_has_meta(&fl->pat)) {
			char **matches = NULL;
			size_t nmatches = 0;
			int gr = tc_glob_expand(sh, &fl->pat, w->line,
						&matches, &nmatches);
			if (gr != TC_OK)
				return TC_EXP_NOMEM;
			if (nmatches > 0) {
				for (size_t i = 0; i < nmatches; i++) {
					r = push_arg(sh, w->line, tail, count,
						     matches[i], strlen(matches[i]));
					if (r != TC_EXP_OK)
						return r;
				}
				continue;
			}
		}
		r = push_arg(sh, w->line, tail, count, fl->pat.text, fl->pat.len);
		if (r != TC_EXP_OK)
			return r;
	}
	return TC_EXP_OK;
}

int tc_expand_words(struct tc_shell *sh, const struct tc_word *words,
		    size_t nwords, char ***out_argv, size_t *out_argc)
{
	struct arg_link *head = NULL, **tail = &head;
	size_t argc = 0;

	for (size_t i = 0; i < nwords; i++) {
		int r = expand_one_word(sh, &words[i], &tail, &argc);
		if (r != TC_EXP_OK)
			return r;
	}

	char **argv = tc_arena_alloc(&sh->arena, (argc + 1) * sizeof(*argv));
	if (!argv) {
		tc_error(sh, nwords ? words[0].line : 0, "out of memory");
		return TC_EXP_NOMEM;
	}
	size_t idx = 0;
	for (struct arg_link *al = head; al; al = al->next)
		argv[idx++] = al->arg;
	argv[argc] = NULL;
	*out_argv = argv;
	*out_argc = argc;
	return TC_EXP_OK;
}
