/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fish-style interactive abbreviations: hash table, line-editor expansion,
 * and `abbr` builtin (0.3.4). */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tam-clam/abbr.h"
#include "tam-clam/builtins.h"
#include "tam-clam/edit.h"
#include "tam-clam/shell.h"

#define TC_ABBR_INIT_CAP 32

static uint64_t hash_str(const char *s)
{
	uint64_t h = 14695981039346656037ULL;
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ULL;
	}
	return h;
}

static char *xstrdup(const char *s)
{
	size_t n = strlen(s);
	char *p = malloc(n + 1);
	if (p)
		memcpy(p, s, n + 1);
	return p;
}

void tc_abbr_init(struct tc_abbrs *ab)
{
	ab->slots = NULL;
	ab->cap = 0;
	ab->count = 0;
}

void tc_abbr_finish(struct tc_abbrs *ab)
{
	if (ab->slots) {
		for (size_t i = 0; i < ab->cap; i++) {
			free(ab->slots[i].name);
			free(ab->slots[i].expansion);
		}
		free(ab->slots);
		ab->slots = NULL;
	}
	ab->cap = 0;
	ab->count = 0;
}

static struct tc_abbr_entry *find_slot(struct tc_abbr_entry *slots, size_t cap,
				       const char *name)
{
	size_t mask = cap - 1;
	size_t idx = (size_t)hash_str(name) & mask;
	for (;;) {
		struct tc_abbr_entry *e = &slots[idx];
		if (!e->name || strcmp(e->name, name) == 0)
			return e;
		idx = (idx + 1) & mask;
	}
}

static int grow_table(struct tc_abbrs *ab, size_t ncap)
{
	struct tc_abbr_entry *ns = calloc(ncap, sizeof(*ns));
	if (!ns)
		return -1;
	for (size_t i = 0; i < ab->cap; i++) {
		if (!ab->slots[i].name)
			continue;
		struct tc_abbr_entry *dst = find_slot(ns, ncap, ab->slots[i].name);
		*dst = ab->slots[i];
	}
	free(ab->slots);
	ab->slots = ns;
	ab->cap = ncap;
	return 0;
}

const struct tc_abbr_entry *tc_abbr_get(const struct tc_abbrs *ab, const char *name)
{
	if (!ab || ab->cap == 0 || !name || !*name)
		return NULL;
	struct tc_abbr_entry *e = find_slot(ab->slots, ab->cap, name);
	return e->name ? e : NULL;
}

int tc_abbr_set(struct tc_abbrs *ab, const char *name, const char *expansion,
		enum tc_abbr_pos pos)
{
	if (!name || !*name || !expansion)
		return -1;
	if (ab->cap == 0) {
		if (grow_table(ab, TC_ABBR_INIT_CAP) < 0)
			return -1;
	} else if ((ab->count + 1) * 10 >= ab->cap * 7) {
		if (grow_table(ab, ab->cap * 2) < 0)
			return -1;
	}

	struct tc_abbr_entry *e = find_slot(ab->slots, ab->cap, name);
	char *ecopy = xstrdup(expansion);
	if (!ecopy)
		return -1;
	if (e->name) {
		free(e->expansion);
		e->expansion = ecopy;
		e->pos = pos;
		return 0;
	}
	char *ncopy = xstrdup(name);
	if (!ncopy) {
		free(ecopy);
		return -1;
	}
	e->name = ncopy;
	e->expansion = ecopy;
	e->pos = pos;
	ab->count++;
	return 0;
}

bool tc_abbr_erase(struct tc_abbrs *ab, const char *name)
{
	if (!ab || ab->cap == 0 || !name || !*name)
		return false;
	size_t mask = ab->cap - 1;
	size_t idx = (size_t)hash_str(name) & mask;
	for (;;) {
		struct tc_abbr_entry *e = &ab->slots[idx];
		if (!e->name)
			return false;
		if (strcmp(e->name, name) == 0) {
			free(e->name);
			free(e->expansion);
			memset(e, 0, sizeof(*e));
			ab->count--;
			size_t next = (idx + 1) & mask;
			while (ab->slots[next].name) {
				struct tc_abbr_entry tmp = ab->slots[next];
				memset(&ab->slots[next], 0, sizeof(ab->slots[next]));
				struct tc_abbr_entry *dst =
					find_slot(ab->slots, ab->cap, tmp.name);
				*dst = tmp;
				next = (next + 1) & mask;
			}
			return true;
		}
		idx = (idx + 1) & mask;
	}
}

/* ---- Interactive expansion -------------------------------------------- */

static bool is_sep_char(char c)
{
	return c == ' ' || c == '\t' || c == '\n' ||
	       c == ';' || c == '|' || c == '&' || c == '(' || c == ')';
}

static bool is_cmd_intro_kw(const char *s, size_t len)
{
	static const char *const kw[] = {
		"if", "then", "elif", "else", "do", "while", "until", "!", "{"
	};
	for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
		if (strlen(kw[i]) == len && memcmp(s, kw[i], len) == 0)
			return true;
	}
	return false;
}

bool tc_abbr_try_expand(const struct tc_abbrs *ab, struct tc_edit_buf *buf)
{
	if (!ab || ab->count == 0 || !buf || buf->cursor == 0 || !buf->data)
		return false;

	const char *s = buf->data;
	size_t cur = buf->cursor;
	if (is_sep_char(s[cur - 1]))
		return false;

	/* Scan from 0..cur tracking quoting and whether the active token is
	 * in command position. */
	bool in_sq = false, in_dq = false, escaped = false;
	bool expect_cmd = true;
	bool tok_quoted = false;
	size_t tok_start = cur;
	bool in_tok = false;

	for (size_t i = 0; i < cur; i++) {
		char c = s[i];
		if (escaped) {
			escaped = false;
			if (!in_tok) {
				in_tok = true;
				tok_start = i - 1;
			}
			tok_quoted = true;
			continue;
		}
		if (in_sq) {
			if (c == '\'')
				in_sq = false;
			tok_quoted = true;
			continue;
		}
		if (in_dq) {
			if (c == '\\')
				escaped = true;
			else if (c == '"')
				in_dq = false;
			tok_quoted = true;
			continue;
		}
		if (c == '\\') {
			escaped = true;
			if (!in_tok) {
				in_tok = true;
				tok_start = i;
			}
			tok_quoted = true;
			continue;
		}
		if (c == '\'') {
			in_sq = true;
			if (!in_tok) {
				in_tok = true;
				tok_start = i;
			}
			tok_quoted = true;
			continue;
		}
		if (c == '"') {
			in_dq = true;
			if (!in_tok) {
				in_tok = true;
				tok_start = i;
			}
			tok_quoted = true;
			continue;
		}
		if (is_sep_char(c)) {
			if (in_tok) {
				size_t wlen = i - tok_start;
				if (expect_cmd)
					expect_cmd = !tok_quoted &&
						     is_cmd_intro_kw(s + tok_start, wlen);
				in_tok = false;
				tok_quoted = false;
			}
			if (c == '\n' || c == ';' || c == '|' || c == '&' || c == '(')
				expect_cmd = true;
			continue;
		}
		if (!in_tok) {
			in_tok = true;
			tok_start = i;
			tok_quoted = false;
		}
	}

	if (!in_tok || in_sq || in_dq || escaped || tok_quoted || tok_start >= cur)
		return false;

	size_t tlen = cur - tok_start;
	if (tlen >= 128)
		return false;
	char tok[128];
	memcpy(tok, s + tok_start, tlen);
	tok[tlen] = '\0';

	const struct tc_abbr_entry *e = tc_abbr_get(ab, tok);
	if (!e)
		return false;
	if (e->pos == TC_ABBR_COMMAND && !expect_cmd)
		return false;

	/* Replace [tok_start, cur) with e->expansion as a single undoable edit. */
	if (tc_ebuf_delete_range(buf, tok_start, cur, true) < 0)
		return false;
	(void)tc_ebuf_insert(buf, e->expansion, strlen(e->expansion), false);
	return true;
}

/* ---- `abbr` builtin --------------------------------------------------- */

static int cmp_abbr_ptr(const void *a, const void *b)
{
	const struct tc_abbr_entry *ea = *(const struct tc_abbr_entry *const *)a;
	const struct tc_abbr_entry *eb = *(const struct tc_abbr_entry *const *)b;
	return strcmp(ea->name, eb->name);
}

static int print_abbrs(struct tc_shell *sh, bool names_only, unsigned long line)
{
	if (sh->abbrs.count == 0)
		return 0;
	const struct tc_abbr_entry **list = malloc(sh->abbrs.count * sizeof(*list));
	if (!list) {
		tc_error(sh, line, "abbr: out of memory");
		return 1;
	}
	size_t n = 0;
	for (size_t i = 0; i < sh->abbrs.cap; i++) {
		if (sh->abbrs.slots[i].name)
			list[n++] = &sh->abbrs.slots[i];
	}
	qsort(list, n, sizeof(*list), cmp_abbr_ptr);

	int status = 0;
	for (size_t i = 0; i < n; i++) {
		const struct tc_abbr_entry *e = list[i];
		if (names_only) {
			if (tc_write_all(STDOUT_FILENO, e->name, strlen(e->name)) < 0 ||
			    tc_write_all(STDOUT_FILENO, "\n", 1) < 0) {
				status = 1;
				break;
			}
		} else {
			const char *hdr = (e->pos == TC_ABBR_ANYWHERE)
				? "abbr -a -p anywhere "
				: "abbr -a ";
			if (tc_write_all(STDOUT_FILENO, hdr, strlen(hdr)) < 0 ||
			    tc_write_all(STDOUT_FILENO, e->name, strlen(e->name)) < 0 ||
			    tc_write_all(STDOUT_FILENO, " ", 1) < 0 ||
			    tc_write_shell_quoted(STDOUT_FILENO, e->expansion) < 0 ||
			    tc_write_all(STDOUT_FILENO, "\n", 1) < 0) {
				status = 1;
				break;
			}
		}
	}
	if (status != 0)
		tc_error(sh, line, "abbr: write error: %s", strerror(errno));
	free(list);
	return status;
}

int tc_bi_abbr(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	enum { MODE_DEFAULT, MODE_ADD, MODE_ERASE, MODE_QUERY, MODE_LIST, MODE_SHOW } mode = MODE_DEFAULT;
	enum tc_abbr_pos pos = TC_ABBR_COMMAND;
	int i = 1;

	for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		const char *a = argv[i];
		if (strcmp(a, "--") == 0) {
			i++;
			break;
		}
		if (strcmp(a, "-a") == 0 || strcmp(a, "--add") == 0) {
			mode = MODE_ADD;
		} else if (strcmp(a, "-e") == 0 || strcmp(a, "--erase") == 0) {
			mode = MODE_ERASE;
		} else if (strcmp(a, "-q") == 0 || strcmp(a, "--query") == 0) {
			mode = MODE_QUERY;
		} else if (strcmp(a, "-l") == 0 || strcmp(a, "--list") == 0) {
			mode = MODE_LIST;
		} else if (strcmp(a, "-s") == 0 || strcmp(a, "--show") == 0) {
			mode = MODE_SHOW;
		} else if (strcmp(a, "-p") == 0 || strcmp(a, "--position") == 0) {
			if (i + 1 >= argc) {
				tc_error(sh, line, "abbr: %s requires an argument", a);
				return TC_STATUS_USAGE;
			}
			const char *pv = argv[++i];
			if (strcmp(pv, "command") == 0)
				pos = TC_ABBR_COMMAND;
			else if (strcmp(pv, "anywhere") == 0)
				pos = TC_ABBR_ANYWHERE;
			else {
				tc_error(sh, line, "abbr: invalid position `%s' (use command or anywhere)", pv);
				return TC_STATUS_USAGE;
			}
		} else if (strncmp(a, "--position=", 11) == 0) {
			const char *pv = a + 11;
			if (strcmp(pv, "command") == 0)
				pos = TC_ABBR_COMMAND;
			else if (strcmp(pv, "anywhere") == 0)
				pos = TC_ABBR_ANYWHERE;
			else {
				tc_error(sh, line, "abbr: invalid position `%s' (use command or anywhere)", pv);
				return TC_STATUS_USAGE;
			}
		} else {
			tc_error(sh, line, "abbr: %s: invalid option", a);
			return TC_STATUS_USAGE;
		}
	}

	if (mode == MODE_LIST)
		return print_abbrs(sh, true, line);
	if (mode == MODE_SHOW || (mode == MODE_DEFAULT && i >= argc))
		return print_abbrs(sh, false, line);

	if (mode == MODE_QUERY) {
		if (i >= argc)
			return 1;
		for (; i < argc; i++) {
			if (!tc_abbr_get(&sh->abbrs, argv[i]))
				return 1;
		}
		return 0;
	}

	if (mode == MODE_ERASE) {
		if (i >= argc) {
			tc_error(sh, line, "abbr: -e requires an abbreviation name");
			return TC_STATUS_USAGE;
		}
		int status = 0;
		for (; i < argc; i++) {
			if (!tc_abbr_erase(&sh->abbrs, argv[i])) {
				tc_error(sh, line, "abbr: %s: not found", argv[i]);
				status = 1;
			}
		}
		return status;
	}

	/* MODE_ADD or MODE_DEFAULT with arguments. */
	if (i >= argc) {
		tc_error(sh, line, "abbr: -a requires NAME and EXPANSION");
		return TC_STATUS_USAGE;
	}

	const char *name = argv[i];
	const char *eq = strchr(name, '=');
	if (eq && i + 1 == argc) {
		size_t nlen = (size_t)(eq - name);
		if (nlen == 0) {
			tc_error(sh, line, "abbr: empty abbreviation name");
			return TC_STATUS_USAGE;
		}
		char *ncopy = malloc(nlen + 1);
		if (!ncopy) {
			tc_error(sh, line, "abbr: out of memory");
			return 1;
		}
		memcpy(ncopy, name, nlen);
		ncopy[nlen] = '\0';
		int r = tc_abbr_set(&sh->abbrs, ncopy, eq + 1, pos);
		free(ncopy);
		if (r < 0) {
			tc_error(sh, line, "abbr: out of memory");
			return 1;
		}
		return 0;
	}

	if (i + 1 >= argc) {
		tc_error(sh, line, "abbr: missing expansion for `%s'", name);
		return TC_STATUS_USAGE;
	}

	/* Join remaining arguments with single spaces. */
	size_t total = 0;
	for (int k = i + 1; k < argc; k++)
		total += strlen(argv[k]) + (k > i + 1 ? 1 : 0);
	char *exp = malloc(total + 1);
	if (!exp) {
		tc_error(sh, line, "abbr: out of memory");
		return 1;
	}
	char *p = exp;
	for (int k = i + 1; k < argc; k++) {
		if (k > i + 1)
			*p++ = ' ';
		size_t kl = strlen(argv[k]);
		memcpy(p, argv[k], kl);
		p += kl;
	}
	*p = '\0';

	int r = tc_abbr_set(&sh->abbrs, name, exp, pos);
	free(exp);
	if (r < 0) {
		tc_error(sh, line, "abbr: out of memory");
		return 1;
	}
	return 0;
}
