/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Context-aware completion engine: commands, files, variables, user names,
 * and lazy declarative *.comp files for command options & subcommands. */
#define _XOPEN_SOURCE 700
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tam-clam/alias.h"
#include "tam-clam/builtins.h"
#include "tam-clam/complete.h"
#include "tam-clam/func.h"
#include "tam-clam/shell.h"
#include "tam-clam/var.h"
#include "tam.h"

void tc_comp_list_init(struct tc_comp_list *cl)
{
	memset(cl, 0, sizeof(*cl));
}

void tc_comp_list_finish(struct tc_comp_list *cl)
{
	if (!cl)
		return;
	for (size_t i = 0; i < cl->count; i++) {
		free(cl->items[i].text);
		free(cl->items[i].desc);
	}
	free(cl->items);
	memset(cl, 0, sizeof(*cl));
}

static int comp_add(struct tc_comp_list *cl, const char *text,
		    const char *desc, bool append_sp)
{
	if (cl->count >= TC_COMP_MAX)
		return 0;
	if (cl->count == cl->cap) {
		size_t ncap = cl->cap ? cl->cap * 2 : 32;
		if (ncap > TC_COMP_MAX)
			ncap = TC_COMP_MAX;
		struct tc_comp_item *ni = realloc(cl->items, ncap * sizeof(*ni));
		if (!ni)
			return -1;
		cl->items = ni;
		cl->cap = ncap;
	}
	size_t tlen = strlen(text);
	char *tc = malloc(tlen + 1);
	if (!tc)
		return -1;
	memcpy(tc, text, tlen + 1);

	char *dc = NULL;
	if (desc && *desc) {
		size_t dlen = strlen(desc);
		dc = malloc(dlen + 1);
		if (!dc) {
			free(tc);
			return -1;
		}
		memcpy(dc, desc, dlen + 1);
	}
	cl->items[cl->count].text = tc;
	cl->items[cl->count].desc = dc;
	cl->items[cl->count].append_sp = append_sp;
	cl->count++;
	return 0;
}

static int cmp_item(const void *a, const void *b)
{
	const struct tc_comp_item *ia = a;
	const struct tc_comp_item *ib = b;
	return strcmp(ia->text, ib->text);
}

static void comp_sort_dedup(struct tc_comp_list *cl)
{
	if (cl->count <= 1)
		return;
	qsort(cl->items, cl->count, sizeof(cl->items[0]), cmp_item);
	size_t w = 1;
	for (size_t i = 1; i < cl->count; i++) {
		if (strcmp(cl->items[i].text, cl->items[w - 1].text) == 0) {
			if (!cl->items[w - 1].desc && cl->items[i].desc) {
				cl->items[w - 1].desc = cl->items[i].desc;
				cl->items[i].desc = NULL;
			}
			free(cl->items[i].text);
			free(cl->items[i].desc);
		} else {
			if (w != i)
				cl->items[w] = cl->items[i];
			w++;
		}
	}
	cl->count = w;
}

size_t tc_comp_common_prefix(const struct tc_comp_list *cl)
{
	if (!cl || cl->count == 0)
		return 0;
	const char *first = cl->items[0].text;
	size_t pfx = strlen(first);
	for (size_t i = 1; i < cl->count && pfx > 0; i++) {
		const char *s = cl->items[i].text;
		size_t j = 0;
		while (j < pfx && first[j] == s[j])
			j++;
		pfx = j;
	}
	return pfx;
}

static bool is_sep(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == ';' ||
	       c == '|' || c == '&' || c == '(' || c == ')' ||
	       c == '<' || c == '>';
}

/* Complete $VAR or ${VAR. Returns true if token was a variable reference. */
static bool try_complete_var(struct tc_shell *sh, const char *tok, size_t tlen,
			     size_t tok_start, struct tc_comp_list *cl)
{
	/* Find the last unescaped '$' inside tok. */
	ssize_t dollar = -1;
	for (size_t i = 0; i < tlen; i++) {
		if (tok[i] == '\\' && i + 1 < tlen) {
			i++;
			continue;
		}
		if (tok[i] == '$')
			dollar = (ssize_t)i;
	}
	if (dollar < 0)
		return false;

	size_t p = (size_t)dollar + 1;
	bool braced = false;
	if (p < tlen && tok[p] == '{') {
		braced = true;
		p++;
	}
	/* Every char after $ / ${ must be valid variable name char. */
	for (size_t i = p; i < tlen; i++) {
		unsigned char c = (unsigned char)tok[i];
		if (!(isalnum(c) || c == '_'))
			return false;
	}

	const char *pfx = tok + p;
	size_t plen = tlen - p;
	const struct tc_var_entry **list = NULL;
	size_t n = 0;
	if (tc_var_list(&sh->vars, 0, &list, &n) != 0)
		return true;

	cl->tok_start = tok_start + p;
	for (size_t i = 0; i < n; i++) {
		if (plen == 0 || strncmp(list[i]->name, pfx, plen) == 0) {
			if (braced) {
				size_t nl = strlen(list[i]->name);
				char *tmp = malloc(nl + 2);
				if (tmp) {
					memcpy(tmp, list[i]->name, nl);
					tmp[nl] = '}';
					tmp[nl + 1] = '\0';
					(void)comp_add(cl, tmp, NULL, false);
					free(tmp);
				}
			} else {
				(void)comp_add(cl, list[i]->name, NULL, true);
			}
		}
	}
	free(list);
	return true;
}

/* Complete ~user when token begins with '~' and has no '/'. */
static bool try_complete_user(const char *tok, size_t tlen,
			      struct tc_comp_list *cl)
{
	if (tlen == 0 || tok[0] != '~' || memchr(tok, '/', tlen) != NULL)
		return false;
	const char *pfx = tok + 1;
	size_t plen = tlen - 1;
	setpwent();
	for (;;) {
		struct passwd *pw = getpwent();
		if (!pw)
			break;
		if (plen == 0 || strncmp(pw->pw_name, pfx, plen) == 0) {
			size_t nl = strlen(pw->pw_name);
			char *tmp = malloc(nl + 3);
			if (tmp) {
				tmp[0] = '~';
				memcpy(tmp + 1, pw->pw_name, nl);
				tmp[nl + 1] = '/';
				tmp[nl + 2] = '\0';
				(void)comp_add(cl, tmp, NULL, false);
				free(tmp);
			}
			if (cl->count >= TC_COMP_MAX)
				break;
		}
	}
	endpwent();
	return true;
}

static char *escape_spaces(const char *s)
{
	size_t len = strlen(s);
	size_t extra = 0;
	for (size_t i = 0; i < len; i++) {
		if (s[i] == ' ' || s[i] == '\t')
			extra++;
	}
	char *out = malloc(len + extra + 1);
	if (!out)
		return NULL;
	size_t pos = 0;
	for (size_t i = 0; i < len; i++) {
		if (s[i] == ' ' || s[i] == '\t')
			out[pos++] = '\\';
		out[pos++] = s[i];
	}
	out[pos] = '\0';
	return out;
}

static void complete_files(struct tc_shell *sh, const char *tok, size_t tlen,
			   bool dirs_only, bool exec_only, struct tc_comp_list *cl)
{
	/* Split into dir prefix (for display) and basename prefix. */
	char *raw = malloc(tlen + 1);
	if (!raw)
		return;
	/* Unescape '\ ' in token for filesystem lookup. */
	size_t rlen = 0;
	for (size_t i = 0; i < tlen; i++) {
		if (tok[i] == '\\' && i + 1 < tlen && tok[i + 1] == ' ')
			i++;
		raw[rlen++] = tok[i];
	}
	raw[rlen] = '\0';

	const char *slash = strrchr(raw, '/');
	size_t dlen = slash ? (size_t)(slash - raw) + 1 : 0;
	const char *base = slash ? slash + 1 : raw;
	size_t blen = strlen(base);

	/* Expand leading ~/ for opendir(), while keeping ~/ in display prefix. */
	char *open_dir = NULL;
	if (dlen == 0) {
		open_dir = strdup(".");
	} else if (dlen >= 2 && raw[0] == '~' && raw[1] == '/') {
		const char *home = tc_var_get(&sh->vars, "HOME");
		if (home && *home) {
			size_t hl = strlen(home);
			open_dir = malloc(hl + dlen);
			if (open_dir) {
				memcpy(open_dir, home, hl);
				memcpy(open_dir + hl, raw + 1, dlen - 1);
				open_dir[hl + dlen - 1] = '\0';
			}
		}
	} else {
		open_dir = malloc(dlen + 1);
		if (open_dir) {
			memcpy(open_dir, raw, dlen);
			open_dir[dlen] = '\0';
		}
	}
	if (!open_dir) {
		free(raw);
		return;
	}

	DIR *dp = opendir(open_dir);
	if (!dp) {
		free(open_dir);
		free(raw);
		return;
	}

	struct dirent *de;
	while ((de = readdir(dp)) != NULL && cl->count < TC_COMP_MAX) {
		const char *nm = de->d_name;
		if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
			continue;
		if (nm[0] == '.' && (blen == 0 || base[0] != '.'))
			continue;
		if (blen > 0 && strncmp(nm, base, blen) != 0)
			continue;

		size_t od_len = strlen(open_dir);
		size_t nm_len = strlen(nm);
		char *full = malloc(od_len + nm_len + 2);
		if (!full)
			break;
		memcpy(full, open_dir, od_len);
		size_t fp = od_len;
		if (fp > 0 && full[fp - 1] != '/')
			full[fp++] = '/';
		memcpy(full + fp, nm, nm_len + 1);

		struct stat st;
		bool is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
		bool ok = true;
		if (dirs_only && !is_dir)
			ok = false;
		else if (exec_only && !is_dir && access(full, X_OK) != 0)
			ok = false;
		free(full);
		if (!ok)
			continue;

		char *cand = malloc(dlen + nm_len + 2);
		if (!cand)
			break;
		if (dlen > 0)
			memcpy(cand, raw, dlen);
		memcpy(cand + dlen, nm, nm_len);
		size_t cp = dlen + nm_len;
		if (is_dir)
			cand[cp++] = '/';
		cand[cp] = '\0';

		char *esc = escape_spaces(cand);
		free(cand);
		if (!esc)
			break;
		(void)comp_add(cl, esc, NULL, !is_dir);
		free(esc);
	}
	closedir(dp);
	free(open_dir);
	free(raw);
}

static void complete_commands(struct tc_shell *sh, const char *tok, size_t tlen,
			      struct tc_comp_list *cl)
{
	if (memchr(tok, '/', tlen) != NULL) {
		complete_files(sh, tok, tlen, false, true, cl);
		return;
	}

	/* 1. Builtins. */
	for (size_t i = 0; ; i++) {
		const struct tc_builtin *bi = tc_builtin_at(i);
		if (!bi)
			break;
		if (tlen == 0 || strncmp(bi->name, tok, tlen) == 0)
			(void)comp_add(cl, bi->name, "builtin", true);
	}

	/* 2. Aliases. */
	const struct tc_alias_entry **alist = NULL;
	size_t an = 0;
	if (tc_alias_list(&sh->aliases, &alist, &an) == 0) {
		for (size_t i = 0; i < an; i++) {
			if (tlen == 0 || strncmp(alist[i]->name, tok, tlen) == 0)
				(void)comp_add(cl, alist[i]->name, "alias", true);
		}
		free(alist);
	}

	/* 3. Functions. */
	for (size_t i = 0; i < sh->funcs.cap; i++) {
		if (!sh->funcs.slots[i])
			continue;
		const char *fn = sh->funcs.slots[i]->name;
		if (tlen == 0 || strncmp(fn, tok, tlen) == 0)
			(void)comp_add(cl, fn, "function", true);
	}

	/* 4. Executables in $PATH. */
	const char *path = tc_var_get(&sh->vars, "PATH");
	if (!path || !*path)
		return;
	for (const char *p = path; *p && cl->count < TC_COMP_MAX; ) {
		const char *colon = strchr(p, ':');
		size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
		char *dir = NULL;
		if (dlen == 0) {
			dir = strdup(".");
		} else {
			dir = malloc(dlen + 1);
			if (dir) {
				memcpy(dir, p, dlen);
				dir[dlen] = '\0';
			}
		}
		if (dir) {
			DIR *dp = opendir(dir);
			if (dp) {
				struct dirent *de;
				while ((de = readdir(dp)) != NULL && cl->count < TC_COMP_MAX) {
					const char *nm = de->d_name;
					if (nm[0] == '.')
						continue;
					if (tlen > 0 && strncmp(nm, tok, tlen) != 0)
						continue;
					char *full = tam_path_join(dir, nm);
					if (!full)
						break;
					struct stat st;
					if (stat(full, &st) == 0 && !S_ISDIR(st.st_mode) &&
					    access(full, X_OK) == 0) {
						(void)comp_add(cl, nm, NULL, true);
					}
					free(full);
				}
				closedir(dp);
			}
			free(dir);
		}
		if (!colon)
			break;
		p = colon + 1;
	}
}

/* Lazy declarative completion file loader: looks for <cmd>.comp. */
static FILE *open_comp_file(struct tc_shell *sh, const char *cmd)
{
	if (!cmd || !*cmd || strchr(cmd, '/') != NULL || strcmp(cmd, ".") == 0 ||
	    strcmp(cmd, "..") == 0)
		return NULL;

	char leaf[160];
	int ln = snprintf(leaf, sizeof(leaf), "%s.comp", cmd);
	if (ln <= 0 || (size_t)ln >= sizeof(leaf))
		return NULL;

	/* 1. $TAM_CLAM_COMPLETIONS_DIR */
	const char *env_dir = tc_var_get(&sh->vars, "TAM_CLAM_COMPLETIONS_DIR");
	if (env_dir && *env_dir) {
		char *p = tam_path_join(env_dir, leaf);
		if (p) {
			FILE *f = fopen(p, "r");
			free(p);
			if (f)
				return f;
		}
	}

	/* 2. XDG config dir: ~/.config/tamlinux/tam-clam/completions/<cmd>.comp */
	char *xdg_dir = tam_xdg_path(TAM_XDG_CONFIG, "tam-clam", "completions", false);
	if (xdg_dir) {
		char *p = tam_path_join(xdg_dir, leaf);
		free(xdg_dir);
		if (p) {
			FILE *f = fopen(p, "r");
			free(p);
			if (f)
				return f;
		}
	}

	/* 3 & 4. Relative to executable path (installed or in-tree build/). */
	if (sh->self_path[0]) {
		char base[TC_SELF_PATH_MAX];
		snprintf(base, sizeof(base), "%s", sh->self_path);
		char *sl = strrchr(base, '/');
		if (sl) {
			*sl = '\0';
			static const char *const rels[] = {
				"../share/tam-clam/completions",
				"../share/completions",
				"../../share/completions",
			};
			for (size_t i = 0; i < sizeof(rels) / sizeof(rels[0]); i++) {
				char full[TC_SELF_PATH_MAX];
				int fn = snprintf(full, sizeof(full), "%s/%s/%s",
						  base, rels[i], leaf);
				if (fn > 0 && (size_t)fn < sizeof(full)) {
					FILE *f = fopen(full, "r");
					if (f)
						return f;
				}
			}
		}
	}
	return NULL;
}

static bool complete_from_spec(struct tc_shell *sh, const char *cmd,
			       const char *tok, size_t tlen, int arg_pos,
			       struct tc_comp_list *cl)
{
	FILE *f = open_comp_file(sh, cmd);
	if (!f)
		return false;

	bool want_opt = (tlen > 0 && tok[0] == '-');
	bool want_sub = (!want_opt && arg_pos == 1);
	bool matched_any = false;

	char line[512];
	while (fgets(line, sizeof(line), f) != NULL && cl->count < TC_COMP_MAX) {
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == '\0')
			continue;
		size_t ll = strlen(p);
		while (ll > 0 && (p[ll - 1] == '\n' || p[ll - 1] == '\r'))
			p[--ll] = '\0';

		bool is_opt = (strncmp(p, "opt ", 4) == 0 || strncmp(p, "opt\t", 4) == 0);
		bool is_sub = (strncmp(p, "sub ", 4) == 0 || strncmp(p, "sub\t", 4) == 0);
		if (!is_opt && !is_sub)
			continue;
		if (is_opt && !want_opt)
			continue;
		if (is_sub && !want_sub)
			continue;

		p += 4;
		while (*p == ' ' || *p == '\t')
			p++;
		char *w = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		size_t wlen = (size_t)(p - w);
		if (wlen == 0)
			continue;
		char *desc = NULL;
		if (*p) {
			*p++ = '\0';
			while (*p == ' ' || *p == '\t')
				p++;
			if (*p)
				desc = p;
		}
		if (tlen == 0 || strncmp(w, tok, tlen) == 0) {
			(void)comp_add(cl, w, desc, true);
			matched_any = true;
		}
	}
	fclose(f);
	return matched_any;
}

int tc_complete(struct tc_shell *sh, const char *buf, size_t len,
		size_t cursor, struct tc_comp_list *cl)
{
	tc_comp_list_init(cl);
	if (!sh || !buf)
		return 0;
	if (cursor > len)
		cursor = len;

	/* Find start of current simple command (after last ; | & ( { \n) and
	 * start of current token (respecting '\ '). */
	size_t cmd_start = 0;
	bool in_sq = false, in_dq = false;
	for (size_t i = 0; i < cursor; i++) {
		char c = buf[i];
		if (!in_sq && !in_dq && c == '\\' && i + 1 < cursor) {
			i++;
			continue;
		}
		if (!in_dq && c == '\'') {
			in_sq = !in_sq;
			continue;
		}
		if (!in_sq && c == '"') {
			in_dq = !in_dq;
			continue;
		}
		if (!in_sq && !in_dq &&
		    (c == ';' || c == '|' || c == '&' || c == '(' ||
		     (c == '{' && (i == 0 || buf[i - 1] != '$')) ||
		     c == '\n')) {
			cmd_start = i + 1;
		}
	}

	/* Scan tokens from cmd_start to cursor to determine arg_pos and cmd word. */
	size_t tok_start = cursor;
	size_t first_start = 0, first_end = 0;
	int arg_pos = 0;
	size_t pos = cmd_start;
	while (pos < cursor) {
		while (pos < cursor && (buf[pos] == ' ' || buf[pos] == '\t'))
			pos++;
		if (pos >= cursor)
			break;
		size_t ws = pos;
		bool sq = false, dq = false;
		while (pos < cursor) {
			char c = buf[pos];
			if (!sq && !dq && c == '\\' && pos + 1 < cursor) {
				pos += 2;
				continue;
			}
			if (!dq && c == '\'') {
				sq = !sq;
				pos++;
				continue;
			}
			if (!sq && c == '"') {
				dq = !dq;
				pos++;
				continue;
			}
			if (!sq && !dq && is_sep(c))
				break;
			pos++;
		}
		if (arg_pos == 0) {
			first_start = ws;
			first_end = pos;
		}
		if (pos == cursor) {
			tok_start = ws;
			break;
		}
		arg_pos++;
	}

	cl->tok_start = tok_start;
	cl->tok_end = cursor;
	const char *tok = buf + tok_start;
	size_t tlen = cursor - tok_start;

	/* 1. Variable completion ($VAR / ${VAR). */
	if (try_complete_var(sh, tok, tlen, tok_start, cl)) {
		comp_sort_dedup(cl);
		return 0;
	}

	/* 2. User-name completion (~user). */
	if (try_complete_user(tok, tlen, cl)) {
		comp_sort_dedup(cl);
		return 0;
	}

	/* 3. Command position (arg_pos == 0). */
	if (arg_pos == 0) {
		complete_commands(sh, tok, tlen, cl);
		comp_sort_dedup(cl);
		return 0;
	}

	/* 4. Argument position: extract command name. */
	char cmd[128] = {0};
	size_t clen = first_end > first_start ? first_end - first_start : 0;
	if (clen > 0 && clen < sizeof(cmd)) {
		memcpy(cmd, buf + first_start, clen);
		cmd[clen] = '\0';
		/* Use basename if command had a path (e.g. ./build/tam-clam). */
		const char *sl = strrchr(cmd, '/');
		if (sl && sl[1])
			memmove(cmd, sl + 1, strlen(sl + 1) + 1);
	}

	bool from_spec = false;
	if (cmd[0])
		from_spec = complete_from_spec(sh, cmd, tok, tlen, arg_pos, cl);

	/* 5. File / directory completion when no option/subcommand matched (or
	 *    non-option argument). */
	if (!from_spec || (tlen > 0 && tok[0] != '-')) {
		bool dirs_only = (strcmp(cmd, "cd") == 0);
		complete_files(sh, tok, tlen, dirs_only, false, cl);
	}

	comp_sort_dedup(cl);
	return 0;
}
