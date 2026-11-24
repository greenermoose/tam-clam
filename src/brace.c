/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Brace expansion: {a,b,c}, nested braces, Cartesian products, and
 * {start..end[..incr]} integer/character/zero-padded sequences.
 * Operates on unquoted TC_WPART_LITERAL segments before all other expansions. */
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tam-clam/brace.h"

/* Flattened item: either one unquoted literal byte or an opaque tc_wpart. */
struct brace_item {
	bool is_char;
	char ch;
	const struct tc_wpart *part; /* when !is_char */
};

struct item_vec {
	struct brace_item *items;
	size_t len;
};

struct vec_list {
	struct item_vec *vecs;
	size_t len;
	size_t cap;
};

static bool has_unquoted_lbrace(const struct tc_wpart *parts)
{
	for (const struct tc_wpart *wp = parts; wp; wp = wp->next) {
		if (wp->kind == TC_WPART_LITERAL &&
		    memchr(wp->text, '{', wp->len) != NULL)
			return true;
	}
	return false;
}

static int flatten_parts(const struct tc_wpart *parts, struct item_vec *out)
{
	size_t total = 0;
	for (const struct tc_wpart *wp = parts; wp; wp = wp->next) {
		if (wp->kind == TC_WPART_LITERAL)
			total += wp->len;
		else
			total += 1;
	}
	struct brace_item *items = total ? malloc(total * sizeof(*items)) : NULL;
	if (total && !items)
		return -1;
	size_t pos = 0;
	for (const struct tc_wpart *wp = parts; wp; wp = wp->next) {
		if (wp->kind == TC_WPART_LITERAL) {
			for (size_t i = 0; i < wp->len; i++) {
				items[pos].is_char = true;
				items[pos].ch = wp->text[i];
				items[pos].part = NULL;
				pos++;
			}
		} else {
			items[pos].is_char = false;
			items[pos].ch = 0;
			items[pos].part = wp;
			pos++;
		}
	}
	out->items = items;
	out->len = total;
	return 0;
}

static void vec_list_free(struct vec_list *vl)
{
	for (size_t i = 0; i < vl->len; i++)
		free(vl->vecs[i].items);
	free(vl->vecs);
	vl->vecs = NULL;
	vl->len = 0;
	vl->cap = 0;
}

static int vec_list_push(struct vec_list *vl, struct brace_item *items, size_t len)
{
	if (vl->len >= TC_BRACE_WORDS_MAX) {
		free(items);
		return -2;
	}
	if (vl->len == vl->cap) {
		size_t ncap = vl->cap ? vl->cap * 2 : 8;
		if (ncap > TC_BRACE_WORDS_MAX)
			ncap = TC_BRACE_WORDS_MAX;
		struct item_vec *nv = realloc(vl->vecs, ncap * sizeof(*nv));
		if (!nv) {
			free(items);
			return -1;
		}
		vl->vecs = nv;
		vl->cap = ncap;
	}
	vl->vecs[vl->len].items = items;
	vl->vecs[vl->len].len = len;
	vl->len++;
	return 0;
}

static struct brace_item *items_dup(const struct brace_item *src, size_t len)
{
	if (len == 0)
		return NULL;
	struct brace_item *dst = malloc(len * sizeof(*dst));
	if (dst)
		memcpy(dst, src, len * sizeof(*dst));
	return dst;
}

/* Parse a signed 64-bit integer strictly (no whitespace) and detect zero-padding. */
static bool parse_seq_int(const char *s, size_t len, int64_t *out_val,
			  size_t *out_width, bool *out_zpad)
{
	if (len == 0 || len > 20)
		return false;
	size_t i = 0;
	bool neg = false;
	if (s[0] == '-' || s[0] == '+') {
		neg = (s[0] == '-');
		i++;
		if (i >= len)
			return false;
	}
	size_t digits_start = i;
	for (; i < len; i++) {
		if (!isdigit((unsigned char)s[i]))
			return false;
	}
	char buf[24];
	memcpy(buf, s, len);
	buf[len] = '\0';
	errno = 0;
	char *end = NULL;
	long long v = strtoll(buf, &end, 10);
	if (errno == ERANGE || !end || *end != '\0')
		return false;

	*out_val = (int64_t)v;
	bool zpad = (len - digits_start > 1 && s[digits_start] == '0');
	*out_zpad = zpad;
	*out_width = zpad ? len : 0;
	(void)neg;
	return true;
}

/* Try to expand a pure-literal sequence inside {body} (start..end or start..end..incr).
 * Returns: 1 on valid sequence (appended to out), 0 if not a valid sequence,
 * -1 on OOM, -2 on TC_BRACE_WORDS_MAX exceeded. */
static int try_expand_sequence(const struct brace_item *body, size_t blen,
			       struct vec_list *out)
{
	if (blen < 4)
		return 0;
	/* Must be purely unquoted literal characters. */
	for (size_t i = 0; i < blen; i++) {
		if (!body[i].is_char)
			return 0;
	}

	char stack_buf[128];
	char *s = blen < sizeof(stack_buf) ? stack_buf : malloc(blen + 1);
	if (!s)
		return -1;
	for (size_t i = 0; i < blen; i++)
		s[i] = body[i].ch;
	s[blen] = '\0';

	/* Split by ".." into 2 or 3 parts. */
	const char *d1 = strstr(s, "..");
	if (!d1 || d1 == s) {
		if (s != stack_buf)
			free(s);
		return 0;
	}
	const char *p1 = s;
	size_t l1 = (size_t)(d1 - s);
	const char *rest = d1 + 2;
	const char *d2 = strstr(rest, "..");
	const char *p2 = rest;
	size_t l2 = d2 ? (size_t)(d2 - rest) : strlen(rest);
	const char *p3 = d2 ? d2 + 2 : NULL;
	size_t l3 = p3 ? strlen(p3) : 0;

	if (l2 == 0 || (d2 && (l3 == 0 || strstr(p3, "..") != NULL))) {
		if (s != stack_buf)
			free(s);
		return 0;
	}

	int64_t step = 1;
	if (p3) {
		size_t dummy_w = 0;
		bool dummy_z = false;
		if (!parse_seq_int(p3, l3, &step, &dummy_w, &dummy_z)) {
			if (s != stack_buf)
				free(s);
			return 0;
		}
		if (step == INT64_MIN) {
			if (s != stack_buf)
				free(s);
			return 0;
		}
		if (step == 0)
			step = 1;
		else if (step < 0)
			step = -step;
	}

	int ret = 0;
	int64_t a = 0, b = 0;
	size_t wa = 0, wb = 0;
	bool za = false, zb = false;

	if (parse_seq_int(p1, l1, &a, &wa, &za) &&
	    parse_seq_int(p2, l2, &b, &wb, &zb)) {
		bool zpad = za || zb;
		size_t width = wa > wb ? wa : wb;
		if (zpad) {
			if (l1 > width)
				width = l1;
			if (l2 > width)
				width = l2;
		}
		int64_t dir = (a <= b) ? step : -step;
		int64_t cur = a;
		for (;;) {
			char num[64];
			int n;
			if (zpad)
				n = snprintf(num, sizeof(num), "%0*" PRId64, (int)width, cur);
			else
				n = snprintf(num, sizeof(num), "%" PRId64, cur);
			if (n < 0 || (size_t)n >= sizeof(num)) {
				ret = -1;
				break;
			}
			struct brace_item *it = malloc((size_t)n * sizeof(*it));
			if (!it) {
				ret = -1;
				break;
			}
			for (int k = 0; k < n; k++) {
				it[k].is_char = true;
				it[k].ch = num[k];
				it[k].part = NULL;
			}
			int pr = vec_list_push(out, it, (size_t)n);
			if (pr != 0) {
				ret = pr;
				break;
			}
			if (dir > 0) {
				if (b - cur < dir)
					break;
			} else {
				if (cur - b < -dir)
					break;
			}
			cur += dir;
		}
		if (ret == 0)
			ret = 1;
	} else if (l1 == 1 && l2 == 1 &&
		   isalpha((unsigned char)p1[0]) &&
		   isalpha((unsigned char)p2[0])) {
		int ca = (unsigned char)p1[0];
		int cb = (unsigned char)p2[0];
		int64_t dir = (ca <= cb) ? step : -step;
		int64_t cur = ca;
		for (;;) {
			struct brace_item *it = malloc(sizeof(*it));
			if (!it) {
				ret = -1;
				break;
			}
			it[0].is_char = true;
			it[0].ch = (char)cur;
			it[0].part = NULL;
			int pr = vec_list_push(out, it, 1);
			if (pr != 0) {
				ret = pr;
				break;
			}
			if (dir > 0) {
				if (cb - cur < dir)
					break;
			} else {
				if (cur - cb < -dir)
					break;
			}
			cur += dir;
		}
		if (ret == 0)
			ret = 1;
	}

	if (s != stack_buf)
		free(s);
	return ret;
}

/* Find the first valid unquoted '{...}' in items[0..len).
 * Returns true if found, setting *out_open and *out_close. */
static bool find_valid_brace(const struct brace_item *items, size_t len,
			     size_t *out_open, size_t *out_close,
			     bool *out_has_comma)
{
	for (size_t i = 0; i < len; i++) {
		if (!items[i].is_char || items[i].ch != '{')
			continue;
		int depth = 1;
		bool has_comma = false;
		size_t close = SIZE_MAX;
		for (size_t j = i + 1; j < len; j++) {
			if (!items[j].is_char)
				continue;
			if (items[j].ch == '{') {
				depth++;
			} else if (items[j].ch == '}') {
				depth--;
				if (depth == 0) {
					close = j;
					break;
				}
			} else if (items[j].ch == ',' && depth == 1) {
				has_comma = true;
			}
		}
		if (close == SIZE_MAX)
			continue;
		if (has_comma) {
			*out_open = i;
			*out_close = close;
			*out_has_comma = true;
			return true;
		}
		/* Check if it is a valid sequence {a..b[..c]}. */
		struct vec_list probe = {0};
		int sr = try_expand_sequence(items + i + 1, close - (i + 1), &probe);
		vec_list_free(&probe);
		if (sr == 1 || sr == -2) {
			*out_open = i;
			*out_close = close;
			*out_has_comma = false;
			return true;
		}
	}
	return false;
}

static int expand_vec(const struct brace_item *items, size_t len,
		      unsigned depth, struct vec_list *out)
{
	if (depth > TC_BRACE_DEPTH_MAX)
		return -2;

	size_t open = 0, close = 0;
	bool has_comma = false;
	if (!find_valid_brace(items, len, &open, &close, &has_comma)) {
		struct brace_item *copy = items_dup(items, len);
		if (len > 0 && !copy)
			return -1;
		return vec_list_push(out, copy, len);
	}

	/* Collect expanded alternatives for items[open+1 .. close). */
	struct vec_list alts = {0};
	if (has_comma) {
		size_t seg_start = open + 1;
		int bdepth = 0;
		for (size_t j = open + 1; j <= close; j++) {
			bool is_sep = false;
			if (j == close) {
				is_sep = true;
			} else if (items[j].is_char) {
				if (items[j].ch == '{')
					bdepth++;
				else if (items[j].ch == '}')
					bdepth--;
				else if (items[j].ch == ',' && bdepth == 0)
					is_sep = true;
			}
			if (is_sep) {
				int er = expand_vec(items + seg_start, j - seg_start,
						    depth + 1, &alts);
				if (er != 0) {
					vec_list_free(&alts);
					return er;
				}
				seg_start = j + 1;
			}
		}
	} else {
		int sr = try_expand_sequence(items + open + 1, close - (open + 1), &alts);
		if (sr < 0) {
			vec_list_free(&alts);
			return sr;
		}
		if (sr == 0) {
			vec_list_free(&alts);
			struct brace_item *copy = items_dup(items, len);
			if (len > 0 && !copy)
				return -1;
			return vec_list_push(out, copy, len);
		}
	}

	/* Expand suffix items[close+1 .. len). */
	struct vec_list suffixes = {0};
	int sr = expand_vec(items + close + 1, len - (close + 1), depth + 1, &suffixes);
	if (sr != 0) {
		vec_list_free(&alts);
		vec_list_free(&suffixes);
		return sr;
	}

	/* Cartesian product: prefix + alt + suffix. */
	int ret = 0;
	for (size_t a = 0; a < alts.len && ret == 0; a++) {
		for (size_t s = 0; s < suffixes.len && ret == 0; s++) {
			size_t tlen = open + alts.vecs[a].len + suffixes.vecs[s].len;
			struct brace_item *comb = tlen ? malloc(tlen * sizeof(*comb)) : NULL;
			if (tlen && !comb) {
				ret = -1;
				break;
			}
			size_t p = 0;
			if (open > 0) {
				memcpy(comb + p, items, open * sizeof(*comb));
				p += open;
			}
			if (alts.vecs[a].len > 0) {
				memcpy(comb + p, alts.vecs[a].items,
				       alts.vecs[a].len * sizeof(*comb));
				p += alts.vecs[a].len;
			}
			if (suffixes.vecs[s].len > 0) {
				memcpy(comb + p, suffixes.vecs[s].items,
				       suffixes.vecs[s].len * sizeof(*comb));
			}
			ret = vec_list_push(out, comb, tlen);
		}
	}

	vec_list_free(&alts);
	vec_list_free(&suffixes);
	return ret;
}

/* Reconstruct a tc_wpart list in sh->arena from an item_vec, coalescing
 * adjacent literal characters into single TC_WPART_LITERAL nodes. */
static int rebuild_wparts(struct tc_arena *a, const struct item_vec *v,
			  struct tc_wpart **out)
{
	struct tc_wpart *head = NULL, **tail = &head;
	size_t i = 0;
	while (i < v->len) {
		if (v->items[i].is_char) {
			size_t start = i;
			while (i < v->len && v->items[i].is_char)
				i++;
			size_t n = i - start;
			char *txt = tc_arena_alloc(a, n + 1);
			struct tc_wpart *wp = tc_arena_alloc(a, sizeof(*wp));
			if (!txt || !wp)
				return -1;
			for (size_t k = 0; k < n; k++)
				txt[k] = v->items[start + k].ch;
			txt[n] = '\0';
			memset(wp, 0, sizeof(*wp));
			wp->kind = TC_WPART_LITERAL;
			wp->text = txt;
			wp->len = n;
			*tail = wp;
			tail = &wp->next;
		} else {
			struct tc_wpart *wp = tc_arena_alloc(a, sizeof(*wp));
			if (!wp)
				return -1;
			*wp = *v->items[i].part;
			wp->next = NULL;
			*tail = wp;
			tail = &wp->next;
			i++;
		}
	}
	*out = head;
	return 0;
}

int tc_brace_expand(struct tc_shell *sh, const struct tc_wpart *parts,
		    unsigned long line,
		    struct tc_wpart ***out_words, size_t *out_nwords)
{
	if (!parts || !has_unquoted_lbrace(parts)) {
		struct tc_wpart **arr = tc_arena_alloc(&sh->arena, sizeof(*arr));
		if (!arr) {
			tc_error(sh, line, "out of memory");
			return TC_ERR_NOMEM;
		}
		arr[0] = (struct tc_wpart *)parts;
		*out_words = arr;
		*out_nwords = 1;
		return TC_OK;
	}

	struct item_vec flat = {0};
	if (flatten_parts(parts, &flat) < 0) {
		tc_error(sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}

	struct vec_list res = {0};
	int er = expand_vec(flat.items, flat.len, 0, &res);
	free(flat.items);
	if (er == -2) {
		vec_list_free(&res);
		tc_error(sh, line, "brace expansion exceeds limit (%d words / depth %d)",
			 TC_BRACE_WORDS_MAX, TC_BRACE_DEPTH_MAX);
		return TC_ERR_NOMEM;
	}
	if (er != 0) {
		vec_list_free(&res);
		tc_error(sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}

	struct tc_wpart **arr = tc_arena_alloc(&sh->arena, res.len * sizeof(*arr));
	if (!arr) {
		vec_list_free(&res);
		tc_error(sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}
	for (size_t i = 0; i < res.len; i++) {
		if (rebuild_wparts(&sh->arena, &res.vecs[i], &arr[i]) < 0) {
			vec_list_free(&res);
			tc_error(sh, line, "out of memory");
			return TC_ERR_NOMEM;
		}
	}
	*out_words = arr;
	*out_nwords = res.len;
	vec_list_free(&res);
	return TC_OK;
}
