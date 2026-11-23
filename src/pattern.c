/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Linear-time POSIX pattern matcher (XCU 2.13.1).
 * Compiles a tc_pat into atoms (LIT, ANY, STAR, 256-bit CLASS) and matches
 * using the greedy-star two-pointer algorithm, which never backtracks
 * exponentially. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "tam-clam/pattern.h"

enum atom_kind {
	ATOM_LIT,
	ATOM_ANY,
	ATOM_STAR,
	ATOM_CLASS,
};

struct pat_atom {
	enum atom_kind kind;
	unsigned char ch;
	uint64_t bits[4];
};

static inline bool pat_is_quoted(const struct tc_pat *pat, size_t i)
{
	return pat->quoted && pat->quoted[i];
}

static inline void bit_set(uint64_t bits[4], unsigned char c)
{
	bits[c >> 6] |= (uint64_t)1 << (c & 63);
}

static inline bool bit_test(const uint64_t bits[4], unsigned char c)
{
	return ((bits[c >> 6] >> (c & 63)) & 1) != 0;
}

static void add_posix_class(uint64_t bits[4], const char *name, size_t len)
{
	for (unsigned c = 0; c < 256; c++) {
		int match = 0;
		if (len == 5 && memcmp(name, "alnum", 5) == 0)
			match = isalnum((int)c);
		else if (len == 5 && memcmp(name, "alpha", 5) == 0)
			match = isalpha((int)c);
		else if (len == 5 && memcmp(name, "blank", 5) == 0)
			match = (c == ' ' || c == '\t');
		else if (len == 5 && memcmp(name, "cntrl", 5) == 0)
			match = iscntrl((int)c);
		else if (len == 5 && memcmp(name, "digit", 5) == 0)
			match = isdigit((int)c);
		else if (len == 5 && memcmp(name, "graph", 5) == 0)
			match = isgraph((int)c);
		else if (len == 5 && memcmp(name, "lower", 5) == 0)
			match = islower((int)c);
		else if (len == 5 && memcmp(name, "print", 5) == 0)
			match = isprint((int)c);
		else if (len == 5 && memcmp(name, "punct", 5) == 0)
			match = ispunct((int)c);
		else if (len == 5 && memcmp(name, "space", 5) == 0)
			match = isspace((int)c);
		else if (len == 5 && memcmp(name, "upper", 5) == 0)
			match = isupper((int)c);
		else if (len == 6 && memcmp(name, "xdigit", 6) == 0)
			match = isxdigit((int)c);
		if (match)
			bit_set(bits, (unsigned char)c);
	}
}

/* Try to parse a bracket expression starting at pat[start] == '[' (unquoted).
 * On success, fills *out and returns the index after the closing ']'.
 * If no closing ']' exists, returns 0 (caller treats '[' as literal). */
static size_t parse_bracket(const struct tc_pat *pat, size_t start,
			    struct pat_atom *out)
{
	size_t i = start + 1;
	size_t len = pat->len;
	if (i >= len)
		return 0;

	bool negate = false;
	if (!pat_is_quoted(pat, i) &&
	    (pat->text[i] == '!' || pat->text[i] == '^')) {
		negate = true;
		i++;
	}

	uint64_t bits[4] = {0, 0, 0, 0};
	bool first = true;
	bool closed = false;

	while (i < len) {
		if (!first && !pat_is_quoted(pat, i) && pat->text[i] == ']') {
			i++;
			closed = true;
			break;
		}
		first = false;

		/* POSIX character class [:name:] */
		if (!pat_is_quoted(pat, i) && pat->text[i] == '[' &&
		    i + 1 < len && !pat_is_quoted(pat, i + 1) &&
		    pat->text[i + 1] == ':') {
			size_t j = i + 2;
			while (j + 1 < len) {
				if (!pat_is_quoted(pat, j) && pat->text[j] == ':' &&
				    !pat_is_quoted(pat, j + 1) && pat->text[j + 1] == ']') {
					add_posix_class(bits, pat->text + i + 2,
							j - (i + 2));
					i = j + 2;
					goto next_elem;
				}
				j++;
			}
		}

		unsigned char lo;
		if (!pat_is_quoted(pat, i) && pat->text[i] == '\\' && i + 1 < len) {
			lo = (unsigned char)pat->text[i + 1];
			i += 2;
		} else {
			lo = (unsigned char)pat->text[i];
			i++;
		}

		/* Range lo-hi, unless '-' is followed immediately by unquoted ']'. */
		if (i + 1 < len && !pat_is_quoted(pat, i) && pat->text[i] == '-' &&
		    !(!pat_is_quoted(pat, i + 1) && pat->text[i + 1] == ']')) {
			i++; /* skip '-' */
			unsigned char hi;
			if (!pat_is_quoted(pat, i) && pat->text[i] == '\\' && i + 1 < len) {
				hi = (unsigned char)pat->text[i + 1];
				i += 2;
			} else {
				hi = (unsigned char)pat->text[i];
				i++;
			}
			for (unsigned c = lo; c <= hi; c++)
				bit_set(bits, (unsigned char)c);
		} else {
			bit_set(bits, lo);
		}
next_elem:
		;
	}

	if (!closed)
		return 0;

	if (negate) {
		for (int k = 0; k < 4; k++)
			bits[k] = ~bits[k];
	}
	if (out) {
		out->kind = ATOM_CLASS;
		out->ch = 0;
		memcpy(out->bits, bits, sizeof(bits));
	}
	return i;
}

static size_t compile_pat(const struct tc_pat *pat, struct pat_atom *atoms)
{
	size_t n = 0;
	size_t i = 0;
	while (i < pat->len) {
		unsigned char c = (unsigned char)pat->text[i];
		if (pat_is_quoted(pat, i)) {
			atoms[n].kind = ATOM_LIT;
			atoms[n].ch = c;
			n++;
			i++;
			continue;
		}
		if (c == '*') {
			if (n == 0 || atoms[n - 1].kind != ATOM_STAR) {
				atoms[n].kind = ATOM_STAR;
				atoms[n].ch = 0;
				n++;
			}
			i++;
		} else if (c == '?') {
			atoms[n].kind = ATOM_ANY;
			atoms[n].ch = 0;
			n++;
			i++;
		} else if (c == '[') {
			size_t next = parse_bracket(pat, i, &atoms[n]);
			if (next > 0) {
				n++;
				i = next;
			} else {
				atoms[n].kind = ATOM_LIT;
				atoms[n].ch = '[';
				n++;
				i++;
			}
		} else if (c == '\\' && i + 1 < pat->len) {
			atoms[n].kind = ATOM_LIT;
			atoms[n].ch = (unsigned char)pat->text[i + 1];
			n++;
			i += 2;
		} else {
			atoms[n].kind = ATOM_LIT;
			atoms[n].ch = c;
			n++;
			i++;
		}
	}
	return n;
}

static bool match_atoms(const struct pat_atom *atoms, size_t natoms,
			const char *str, size_t slen)
{
	size_t pi = 0, si = 0;
	size_t star_pi = SIZE_MAX, star_si = 0;

	while (si < slen) {
		if (pi < natoms && atoms[pi].kind == ATOM_STAR) {
			star_pi = pi;
			star_si = si;
			pi++;
		} else if (pi < natoms &&
			   (atoms[pi].kind == ATOM_ANY ||
			    (atoms[pi].kind == ATOM_LIT &&
			     atoms[pi].ch == (unsigned char)str[si]) ||
			    (atoms[pi].kind == ATOM_CLASS &&
			     bit_test(atoms[pi].bits, (unsigned char)str[si])))) {
			pi++;
			si++;
		} else if (star_pi != SIZE_MAX) {
			pi = star_pi + 1;
			si = ++star_si;
		} else {
			return false;
		}
	}

	while (pi < natoms && atoms[pi].kind == ATOM_STAR)
		pi++;
	return pi == natoms;
}

bool tc_pat_match(const struct tc_pat *pat, const char *str, size_t slen)
{
	struct pat_atom stack_buf[64];
	struct pat_atom *atoms = stack_buf;
	if (pat->len > 64) {
		atoms = malloc(pat->len * sizeof(*atoms));
		if (!atoms)
			return false;
	}
	size_t natoms = compile_pat(pat, atoms);
	bool res = match_atoms(atoms, natoms, str, slen);
	if (atoms != stack_buf)
		free(atoms);
	return res;
}

bool tc_pat_has_meta(const struct tc_pat *pat)
{
	for (size_t i = 0; i < pat->len; i++) {
		if (pat_is_quoted(pat, i))
			continue;
		char c = pat->text[i];
		if (c == '\\' && i + 1 < pat->len) {
			i++;
			continue;
		}
		if (c == '*' || c == '?')
			return true;
		if (c == '[' && parse_bracket(pat, i, NULL) > 0)
			return true;
	}
	return false;
}

size_t tc_pat_trim_prefix(const struct tc_pat *pat, const char *str,
			  size_t slen, bool longest)
{
	struct pat_atom stack_buf[64];
	struct pat_atom *atoms = stack_buf;
	if (pat->len > 64) {
		atoms = malloc(pat->len * sizeof(*atoms));
		if (!atoms)
			return 0;
	}
	size_t natoms = compile_pat(pat, atoms);
	size_t res = 0;
	if (longest) {
		for (size_t k = slen + 1; k > 0; k--) {
			if (match_atoms(atoms, natoms, str, k - 1)) {
				res = k - 1;
				break;
			}
		}
	} else {
		for (size_t k = 0; k <= slen; k++) {
			if (match_atoms(atoms, natoms, str, k)) {
				res = k;
				break;
			}
		}
	}
	if (atoms != stack_buf)
		free(atoms);
	return res;
}

size_t tc_pat_trim_suffix(const struct tc_pat *pat, const char *str,
			  size_t slen, bool longest)
{
	struct pat_atom stack_buf[64];
	struct pat_atom *atoms = stack_buf;
	if (pat->len > 64) {
		atoms = malloc(pat->len * sizeof(*atoms));
		if (!atoms)
			return 0;
	}
	size_t natoms = compile_pat(pat, atoms);
	size_t res = 0;
	if (longest) {
		for (size_t k = 0; k <= slen; k++) {
			if (match_atoms(atoms, natoms, str + k, slen - k)) {
				res = slen - k;
				break;
			}
		}
	} else {
		for (size_t k = slen + 1; k > 0; k--) {
			size_t pos = k - 1;
			if (match_atoms(atoms, natoms, str + pos, slen - pos)) {
				res = slen - pos;
				break;
			}
		}
	}
	if (atoms != stack_buf)
		free(atoms);
	return res;
}
