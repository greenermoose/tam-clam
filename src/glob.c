/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pathname expansion (globbing), XCU 2.6.6 and 2.13.3. */
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <tam.h>

#include "tam-clam/glob.h"
#include "tam-clam/parse.h"

struct glob_seg {
	struct tc_pat pat;
	size_t slashes_after; /* number of '/' following this segment */
	bool has_meta;
	bool is_globstar;     /* exact unquoted "**" when sh->opt_globstar */
};

struct glob_res {
	struct tc_shell *sh;
	unsigned long line;
	char **items;
	size_t count;
	size_t cap;
	int err;
};

static int glob_push(struct glob_res *res, const char *path, size_t len)
{
	if (res->count >= TC_WORDS_MAX) {
		tc_error(res->sh, res->line, "pathname expansion exceeds word limit");
		res->err = TC_ERR_NOMEM;
		return -1;
	}
	if (res->count == res->cap) {
		size_t ncap = res->cap ? res->cap * 2 : 16;
		char **ni = realloc(res->items, ncap * sizeof(*ni));
		if (!ni) {
			tc_error(res->sh, res->line, "out of memory");
			res->err = TC_ERR_NOMEM;
			return -1;
		}
		res->items = ni;
		res->cap = ncap;
	}
	char *copy = tc_arena_strndup(&res->sh->arena, path, len);
	if (!copy) {
		tc_error(res->sh, res->line, "out of memory");
		res->err = TC_ERR_NOMEM;
		return -1;
	}
	res->items[res->count++] = copy;
	return 0;
}

static bool is_dir_path(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_real_dir_path(const char *path)
{
	struct stat st;
	return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_globstar_pat(const struct tc_pat *p)
{
	if (p->len != 2 || p->text[0] != '*' || p->text[1] != '*')
		return false;
	if (p->quoted && (p->quoted[0] || p->quoted[1]))
		return false;
	return true;
}

static bool path_exists(const char *path)
{
	struct stat st;
	return lstat(path, &st) == 0;
}

/* Remove unquoted backslashes from a literal pattern segment when building a
 * filesystem path. */
static int append_unescaped(tam_str_t *dst, const struct tc_pat *seg)
{
	for (size_t i = 0; i < seg->len; i++) {
		bool q = seg->quoted && seg->quoted[i];
		char c = seg->text[i];
		if (!q && c == '\\' && i + 1 < seg->len) {
			i++;
			c = seg->text[i];
		}
		tam_str_t next = tam_str_cat_len(*dst, &c, 1);
		if (!next)
			return -1;
		*dst = next;
	}
	return 0;
}

/* POSIX XCU 2.13.3: a leading '.' in a filename must be matched explicitly
 * by a literal '.' (quoted or unquoted), not by '*', '?', or '[.]'. */
static bool pat_starts_with_dot(const struct tc_pat *seg)
{
	if (seg->len == 0)
		return false;
	if (seg->text[0] == '.')
		return true;
	if (!(seg->quoted && seg->quoted[0]) && seg->text[0] == '\\' &&
	    seg->len >= 2 && seg->text[1] == '.')
		return true;
	return false;
}

/* Recursively emit every non-hidden file and directory under prefix (for trailing **). */
static void walk_globstar_all(struct glob_res *res, const char *prefix,
			      size_t plen, unsigned depth)
{
	if (res->err != TC_OK || depth > TC_GLOB_DEPTH_MAX)
		return;

	const char *dir_path = (plen == 0) ? "." : prefix;
	DIR *d = opendir(dir_path);
	if (!d)
		return;

	struct dirent *de;
	while (res->err == TC_OK && (de = readdir(d)) != NULL) {
		const char *name = de->d_name;
		if (name[0] == '.')
			continue;
		size_t nlen = strlen(name);
		tam_str_t next = tam_str_new_len(prefix, plen);
		if (!next) {
			tc_error(res->sh, res->line, "out of memory");
			res->err = TC_ERR_NOMEM;
			break;
		}
		tam_str_t t = tam_str_cat_len(next, name, nlen);
		if (!t) {
			tam_str_free(next);
			tc_error(res->sh, res->line, "out of memory");
			res->err = TC_ERR_NOMEM;
			break;
		}
		next = t;

		if (glob_push(res, next, tam_str_len(next)) < 0) {
			tam_str_free(next);
			break;
		}
		if (is_real_dir_path(next)) {
			t = tam_str_cat_len(next, "/", 1);
			if (!t) {
				tam_str_free(next);
				tc_error(res->sh, res->line, "out of memory");
				res->err = TC_ERR_NOMEM;
				break;
			}
			next = t;
			walk_globstar_all(res, next, tam_str_len(next), depth + 1);
		}
		tam_str_free(next);
	}
	closedir(d);
}

static void walk_glob(struct glob_res *res, const struct glob_seg *segs,
		      size_t nsegs, size_t idx, const char *prefix,
		      size_t plen, unsigned depth)
{
	if (res->err != TC_OK || depth > TC_GLOB_DEPTH_MAX)
		return;

	if (idx == nsegs) {
		glob_push(res, prefix, plen);
		return;
	}

	const struct glob_seg *seg = &segs[idx];
	bool need_dir = (seg->slashes_after > 0);

	if (seg->is_globstar) {
		if (!need_dir) {
			/* Trailing "**": include non-empty prefix (matching bash) plus all
			 * non-hidden files and directories recursively. */
			if (plen > 0 && glob_push(res, prefix, plen) < 0)
				return;
			walk_globstar_all(res, prefix, plen, depth + 1);
			return;
		}
		/* Globstar with trailing slash: match zero directories first. */
		if (idx + 1 < nsegs) {
			walk_glob(res, segs, nsegs, idx + 1, prefix, plen, depth + 1);
		} else if (plen > 0) {
			glob_push(res, prefix, plen);
		}
		if (res->err != TC_OK)
			return;

		/* Then descend into each non-hidden real subdirectory (never following symlinks). */
		const char *dir_path = (plen == 0) ? "." : prefix;
		DIR *d = opendir(dir_path);
		if (!d)
			return;
		struct dirent *de;
		while (res->err == TC_OK && (de = readdir(d)) != NULL) {
			const char *name = de->d_name;
			if (name[0] == '.')
				continue;
			size_t nlen = strlen(name);
			tam_str_t next = tam_str_new_len(prefix, plen);
			if (!next) {
				tc_error(res->sh, res->line, "out of memory");
				res->err = TC_ERR_NOMEM;
				break;
			}
			tam_str_t t = tam_str_cat_len(next, name, nlen);
			if (!t) {
				tam_str_free(next);
				tc_error(res->sh, res->line, "out of memory");
				res->err = TC_ERR_NOMEM;
				break;
			}
			next = t;
			if (is_real_dir_path(next)) {
				for (size_t s = 0; s < seg->slashes_after; s++) {
					t = tam_str_cat_len(next, "/", 1);
					if (!t) {
						tam_str_free(next);
						tc_error(res->sh, res->line, "out of memory");
						res->err = TC_ERR_NOMEM;
						goto done_gs;
					}
					next = t;
				}
				walk_glob(res, segs, nsegs, idx, next, tam_str_len(next),
					  depth + 1);
			}
			tam_str_free(next);
		}
done_gs:
		closedir(d);
		return;
	}

	if (!seg->has_meta) {
		tam_str_t next = tam_str_new_len(prefix, plen);
		if (!next) {
			tc_error(res->sh, res->line, "out of memory");
			res->err = TC_ERR_NOMEM;
			return;
		}
		if (append_unescaped(&next, &seg->pat) < 0) {
			tam_str_free(next);
			tc_error(res->sh, res->line, "out of memory");
			res->err = TC_ERR_NOMEM;
			return;
		}
		bool ok = need_dir ? is_dir_path(next) : path_exists(next);
		if (ok) {
			for (size_t s = 0; s < seg->slashes_after; s++) {
				tam_str_t t = tam_str_cat_len(next, "/", 1);
				if (!t) {
					tam_str_free(next);
					tc_error(res->sh, res->line, "out of memory");
					res->err = TC_ERR_NOMEM;
					return;
				}
				next = t;
			}
			walk_glob(res, segs, nsegs, idx + 1, next, tam_str_len(next),
				  depth + 1);
		}
		tam_str_free(next);
		return;
	}

	const char *dir_path = (plen == 0) ? "." : prefix;
	DIR *d = opendir(dir_path);
	if (!d)
		return;

	bool allow_dot = pat_starts_with_dot(&seg->pat);
	struct dirent *de;
	while (res->err == TC_OK && (de = readdir(d)) != NULL) {
		const char *name = de->d_name;
		if (name[0] == '.' && !allow_dot)
			continue;
		size_t nlen = strlen(name);
		if (!tc_pat_match(&seg->pat, name, nlen))
			continue;

		tam_str_t next = tam_str_new_len(prefix, plen);
		if (!next) {
			tc_error(res->sh, res->line, "out of memory");
			res->err = TC_ERR_NOMEM;
			break;
		}
		tam_str_t t = tam_str_cat_len(next, name, nlen);
		if (!t) {
			tam_str_free(next);
			tc_error(res->sh, res->line, "out of memory");
			res->err = TC_ERR_NOMEM;
			break;
		}
		next = t;

		if (need_dir) {
			if (!is_dir_path(next)) {
				tam_str_free(next);
				continue;
			}
			for (size_t s = 0; s < seg->slashes_after; s++) {
				t = tam_str_cat_len(next, "/", 1);
				if (!t) {
					tam_str_free(next);
					tc_error(res->sh, res->line, "out of memory");
					res->err = TC_ERR_NOMEM;
					goto done_dir;
				}
				next = t;
			}
		}

		walk_glob(res, segs, nsegs, idx + 1, next, tam_str_len(next),
			  depth + 1);
		tam_str_free(next);
	}
done_dir:
	closedir(d);
}

static int cmp_str_ptr(const void *a, const void *b)
{
	const char *sa = *(const char *const *)a;
	const char *sb = *(const char *const *)b;
	return strcmp(sa, sb);
}

int tc_glob_expand(struct tc_shell *sh, const struct tc_pat *pat,
		   unsigned long line, char ***out, size_t *out_n)
{
	*out = NULL;
	*out_n = 0;
	if (!pat || pat->len == 0 || !tc_pat_has_meta(pat))
		return TC_OK;

	/* Count leading slashes. */
	size_t i = 0;
	while (i < pat->len && pat->text[i] == '/')
		i++;
	size_t lead_slashes = i;

	/* Split remaining bytes into '/' delimited segments. */
	size_t seg_cap = 16, nsegs = 0;
	struct glob_seg *segs = malloc(seg_cap * sizeof(*segs));
	if (!segs) {
		tc_error(sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}

	while (i < pat->len) {
		size_t start = i;
		while (i < pat->len && pat->text[i] != '/')
			i++;
		size_t slen = i - start;
		size_t slashes = 0;
		while (i < pat->len && pat->text[i] == '/') {
			slashes++;
			i++;
		}
		if (nsegs >= TC_GLOB_DEPTH_MAX) {
			free(segs);
			return TC_OK;
		}
		if (nsegs == seg_cap) {
			seg_cap *= 2;
			struct glob_seg *ns = realloc(segs, seg_cap * sizeof(*ns));
			if (!ns) {
				free(segs);
				tc_error(sh, line, "out of memory");
				return TC_ERR_NOMEM;
			}
			segs = ns;
		}
		segs[nsegs].pat.text = pat->text + start;
		segs[nsegs].pat.quoted = pat->quoted ? pat->quoted + start : NULL;
		segs[nsegs].pat.len = slen;
		segs[nsegs].slashes_after = slashes;
		segs[nsegs].has_meta = tc_pat_has_meta(&segs[nsegs].pat);
		segs[nsegs].is_globstar = sh->opt_globstar &&
					  is_globstar_pat(&segs[nsegs].pat);
		nsegs++;
	}

	struct glob_res res = {
		.sh = sh,
		.line = line,
		.items = NULL,
		.count = 0,
		.cap = 0,
		.err = TC_OK,
	};

	tam_str_t init = tam_str_new("");
	if (!init) {
		free(segs);
		tc_error(sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}
	for (size_t s = 0; s < lead_slashes; s++) {
		tam_str_t t = tam_str_cat_len(init, "/", 1);
		if (!t) {
			tam_str_free(init);
			free(segs);
			tc_error(sh, line, "out of memory");
			return TC_ERR_NOMEM;
		}
		init = t;
	}

	walk_glob(&res, segs, nsegs, 0, init, tam_str_len(init), 0);
	tam_str_free(init);
	free(segs);

	if (res.err != TC_OK) {
		free(res.items);
		return res.err;
	}
	if (res.count == 0) {
		free(res.items);
		return TC_OK;
	}

	qsort(res.items, res.count, sizeof(*res.items), cmp_str_ptr);
	/* Deduplicate sorted paths (can occur with overlapping ** patterns). */
	size_t uniq = 0;
	for (size_t k = 0; k < res.count; k++) {
		if (uniq == 0 || strcmp(res.items[k], res.items[uniq - 1]) != 0)
			res.items[uniq++] = res.items[k];
	}
	res.count = uniq;
	char **arr = tc_arena_alloc(&sh->arena, res.count * sizeof(*arr));
	if (!arr) {
		free(res.items);
		tc_error(sh, line, "out of memory");
		return TC_ERR_NOMEM;
	}
	memcpy(arr, res.items, res.count * sizeof(*arr));
	free(res.items);
	*out = arr;
	*out_n = res.count;
	return TC_OK;
}
