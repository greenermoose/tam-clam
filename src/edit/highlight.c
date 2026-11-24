/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Live syntax highlighting (from real parser + lexical scanner + per-prompt
 * command-existence cache) and history ghost-text suggestions. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tam-clam/alias.h"
#include "tam-clam/builtins.h"
#include "tam-clam/exec.h"
#include "tam-clam/func.h"
#include "tam-clam/highlight.h"
#include "tam-clam/history.h"
#include "tam-clam/input.h"
#include "tam-clam/parse.h"
#include "tam-clam/shell.h"

void tc_hl_cache_clear(struct tc_hl_cache *c)
{
	if (c)
		c->count = 0;
}

static bool cmd_exists(struct tc_shell *sh, struct tc_hl_cache *cache,
		       const char *word, size_t wlen)
{
	if (wlen == 0 || wlen >= 64)
		return true;
	char name[64];
	memcpy(name, word, wlen);
	name[wlen] = '\0';

	if (cache) {
		for (size_t i = 0; i < cache->count; i++) {
			if (strcmp(cache->entries[i].name, name) == 0)
				return cache->entries[i].exists;
		}
	}

	bool ok = false;
	if (tc_builtin_find(name) != NULL ||
	    tc_alias_get(&sh->aliases, name) != NULL ||
	    tc_func_find(&sh->funcs, name) != NULL) {
		ok = true;
	} else if (strchr(name, '/') != NULL) {
		struct stat st;
		ok = (stat(name, &st) == 0 && !S_ISDIR(st.st_mode) &&
		      access(name, X_OK) == 0);
	} else {
		tc_hash_sync(&sh->hash, sh->vars.path_gen);
		if (tc_hash_get(&sh->hash, name, false) != NULL) {
			ok = true;
		} else {
			char *found = tc_exec_find_in_path(sh, name, false);
			if (found) {
				ok = true;
				free(found);
			}
		}
	}

	if (cache && cache->count < TC_HL_CACHE_MAX) {
		memcpy(cache->entries[cache->count].name, name, wlen + 1);
		cache->entries[cache->count].exists = ok;
		cache->count++;
	}
	return ok;
}

static void paint(unsigned char *styles, size_t start, size_t end,
		  enum tc_hl_kind k)
{
	for (size_t i = start; i < end; i++)
		styles[i] = (unsigned char)k;
}

static bool is_assign_word(const char *s, size_t len)
{
	const char *eq = memchr(s, '=', len);
	if (!eq || eq == s)
		return false;
	size_t nlen = (size_t)(eq - s);
	return tc_var_valid_name(s, nlen);
}

static bool is_op_char(char c)
{
	return c == '|' || c == '&' || c == ';' || c == '(' || c == ')' ||
	       c == '<' || c == '>';
}

static bool word_introduces_cmd(const char *w, size_t len)
{
	static const char *const kw[] = {
		"if", "then", "elif", "else", "do", "while", "until", "!", "{", "[["
	};
	for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
		if (strlen(kw[i]) == len && memcmp(w, kw[i], len) == 0)
			return true;
	}
	return false;
}

/* Convert 1-based (line, col) to byte offset in buf[0..len). */
static size_t line_col_to_off(const char *buf, size_t len,
			      unsigned long line, unsigned long col)
{
	unsigned long cur_line = 1;
	size_t i = 0;
	while (i < len && cur_line < line) {
		if (buf[i] == '\n')
			cur_line++;
		i++;
	}
	size_t c = col > 0 ? (size_t)(col - 1) : 0;
	while (i < len && c > 0 && buf[i] != '\n') {
		i++;
		c--;
	}
	return i;
}

void tc_highlight_line(struct tc_shell *sh, struct tc_hl_cache *cache,
		       const char *buf, size_t len, unsigned char *styles)
{
	if (!styles || len == 0)
		return;
	memset(styles, TC_HL_NONE, len);
	if (!buf)
		return;

	bool expect_cmd = true;
	bool any_unclosed = false;
	size_t i = 0;
	while (i < len) {
		char c = buf[i];
		if (c == ' ' || c == '\t') {
			i++;
			continue;
		}
		if (c == '\n') {
			expect_cmd = true;
			i++;
			continue;
		}
		if (c == '#') {
			size_t start = i;
			while (i < len && buf[i] != '\n')
				i++;
			paint(styles, start, i, TC_HL_COMMENT);
			continue;
		}
		if (is_op_char(c)) {
			size_t start = i;
			if ((c == '|' || c == '&' || c == '<' || c == '>' || c == ';') &&
			    i + 1 < len && buf[i + 1] == c) {
				i += 2;
			} else {
				i++;
			}
			paint(styles, start, i, TC_HL_OP);
			if (c == '|' || c == '&' || c == ';' || c == '(')
				expect_cmd = true;
			continue;
		}

		/* Scan one shell word, tracking quotes and $var spans inside it. */
		size_t wstart = i;
		bool has_quote = false;
		bool has_var = false;
		bool unclosed_quote = false;
		while (i < len) {
			char ch = buf[i];
			if (ch == ' ' || ch == '\t' || ch == '\n' || is_op_char(ch))
				break;
			if (ch == '\\' && i + 1 < len) {
				i += 2;
				continue;
			}
			if (ch == '\'') {
				has_quote = true;
				size_t qs = i++;
				while (i < len && buf[i] != '\'')
					i++;
				if (i < len) {
					i++;
					paint(styles, qs, i, TC_HL_STRING);
				} else {
					unclosed_quote = true;
					paint(styles, qs, i, TC_HL_ERROR);
				}
				continue;
			}
			if (ch == '"') {
				has_quote = true;
				size_t qs = i++;
				while (i < len && buf[i] != '"') {
					if (buf[i] == '\\' && i + 1 < len)
						i += 2;
					else
						i++;
				}
				if (i < len) {
					i++;
					paint(styles, qs, i, TC_HL_STRING);
				} else {
					unclosed_quote = true;
					paint(styles, qs, i, TC_HL_ERROR);
				}
				continue;
			}
			if (ch == '$') {
				has_var = true;
				size_t vs = i++;
				if (i < len && buf[i] == '\'') {
					/* $'...' ANSI-C string */
					i++;
					while (i < len && buf[i] != '\'') {
						if (buf[i] == '\\' && i + 1 < len)
							i += 2;
						else
							i++;
					}
					if (i < len) {
						i++;
						paint(styles, vs, i, TC_HL_STRING);
					} else {
						unclosed_quote = true;
						paint(styles, vs, i, TC_HL_ERROR);
					}
					continue;
				}
				if (i < len && buf[i] == '{') {
					i++;
					while (i < len && buf[i] != '}')
						i++;
					if (i < len)
						i++;
				} else if (i < len && (isalpha((unsigned char)buf[i]) ||
						       buf[i] == '_')) {
					while (i < len && (isalnum((unsigned char)buf[i]) ||
							   buf[i] == '_'))
						i++;
				} else if (i < len) {
					i++;
				}
				paint(styles, vs, i, TC_HL_VAR);
				continue;
			}
			i++;
		}

		size_t wlen = i - wstart;
		if (unclosed_quote) {
			any_unclosed = true;
			expect_cmd = false;
			continue;
		}
		if (expect_cmd) {
			char tmp[32];
			if (!has_quote && !has_var && wlen < sizeof(tmp)) {
				memcpy(tmp, buf + wstart, wlen);
				tmp[wlen] = '\0';
				if (tc_is_reserved_word(tmp)) {
					paint(styles, wstart, i, TC_HL_KEYWORD);
					expect_cmd = word_introduces_cmd(tmp, wlen);
					continue;
				}
			}
			if (!has_quote && is_assign_word(buf + wstart, wlen)) {
				const char *eq = memchr(buf + wstart, '=', wlen);
				size_t eq_off = (size_t)(eq - buf);
				paint(styles, wstart, eq_off, TC_HL_VAR);
				/* Assignment prefix keeps expect_cmd = true. */
				continue;
			}
			if (!has_quote && !has_var && sh) {
				bool ok = cmd_exists(sh, cache, buf + wstart, wlen);
				paint(styles, wstart, i, ok ? TC_HL_CMD_OK : TC_HL_CMD_BAD);
			}
			expect_cmd = false;
		}
	}

	/* Run a silent probe with the real parser to catch unexpected-token
	 * syntax errors (ignoring EOF-only incomplete multi-line constructs
	 * and already-painted unclosed quotes). */
	if (sh && !any_unclosed) {
		struct tc_input in;
		tc_input_from_string(&in, buf, len);
		in.sh = sh;
		struct tc_parser p;
		bool prev_silent = sh->silent_errors;
		sh->silent_errors = true;
		if (tc_parser_init(&p, sh, &in) == TC_OK) {
			for (;;) {
				struct tc_node *node = NULL;
				int r = tc_parse_next(&p, &node);
				if (r == TC_OK)
					continue;
				if (r == TC_ERR_SYNTAX && p.tok.kind != TC_TOK_END) {
					size_t off = line_col_to_off(buf, len,
								     p.tok.line, p.tok.col);
					size_t end = off;
					while (end < len && buf[end] != ' ' &&
					       buf[end] != '\t' && buf[end] != '\n')
						end++;
					if (end == off && off < len)
						end = off + 1;
					paint(styles, off, end, TC_HL_ERROR);
				}
				break;
			}
			tc_parser_finish(&p);
		}
		tc_input_finish(&in);
		tc_arena_reset(&sh->arena);
		sh->silent_errors = prev_silent;
	}
}

const char *tc_suggest_history(struct tc_shell *sh, const char *buf, size_t len)
{
	if (!sh || !sh->hist || !buf || len == 0 || sh->hist->count == 0)
		return NULL;
	/* Only suggest for single-line buffers without trailing newline. */
	if (memchr(buf, '\n', len) != NULL)
		return NULL;
	for (ssize_t i = (ssize_t)sh->hist->count - 1; i >= 0; i--) {
		const struct tc_hist_entry *e = &sh->hist->entries[i];
		if (e->cmd_len > len && memcmp(e->cmd, buf, len) == 0)
			return e->cmd + len;
	}
	return NULL;
}
