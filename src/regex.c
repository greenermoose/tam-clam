/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Linear-time POSIX ERE compiler and Thompson NFA / PikeVM executor.
 * Guarantees O(M * N) worst-case matching time (immune to ReDoS) with
 * leftmost-greedy submatch capture tracking for BASH_REMATCH. */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tam-clam/regex.h"

#define TC_RE_SLOTS_MAX (TC_RE_GROUPS_MAX * 2)

enum re_op {
	RE_CHAR,
	RE_ANY,
	RE_CLASS,
	RE_BOL,
	RE_EOL,
	RE_SPLIT,
	RE_JMP,
	RE_SAVE,
	RE_MATCH,
};

struct re_insn {
	enum re_op op;
	unsigned char ch;
	unsigned short slot;
	size_t x;
	size_t y;
	uint64_t bits[4];
};

struct re_prog {
	struct re_insn *insns;
	size_t len;
	size_t cap;
	size_t ngroups; /* including group 0 */
};

struct re_compiler {
	const char *pat;
	size_t len;
	size_t pos;
	unsigned depth;
	struct re_prog prog;
	int err;
};

static inline void bit_set(uint64_t bits[4], unsigned char c)
{
	bits[c >> 6] |= (uint64_t)1 << (c & 63);
}

static inline bool bit_test(const uint64_t bits[4], unsigned char c)
{
	return ((bits[c >> 6] >> (c & 63)) & 1) != 0;
}

static int prog_emit(struct re_compiler *c, const struct re_insn *in, size_t *out_idx)
{
	if (c->prog.len >= TC_RE_INSN_MAX) {
		c->err = TC_RE_ERR_LIMIT;
		return c->err;
	}
	if (c->prog.len == c->prog.cap) {
		size_t ncap = c->prog.cap ? c->prog.cap * 2 : 64;
		if (ncap > TC_RE_INSN_MAX)
			ncap = TC_RE_INSN_MAX;
		struct re_insn *ni = realloc(c->prog.insns, ncap * sizeof(*ni));
		if (!ni) {
			c->err = TC_RE_ERR_NOMEM;
			return c->err;
		}
		c->prog.insns = ni;
		c->prog.cap = ncap;
	}
	size_t idx = c->prog.len++;
	c->prog.insns[idx] = *in;
	if (out_idx)
		*out_idx = idx;
	return 0;
}

/* Clone instructions [start, end) to the end of prog, adjusting internal jumps. */
static int prog_clone_slice(struct re_compiler *c, size_t start, size_t end)
{
	size_t count = end - start;
	if (count == 0)
		return 0;
	if (c->prog.len + count > TC_RE_INSN_MAX) {
		c->err = TC_RE_ERR_LIMIT;
		return c->err;
	}
	size_t dst_start = c->prog.len;
	size_t delta = dst_start - start;
	for (size_t i = 0; i < count; i++) {
		struct re_insn copy = c->prog.insns[start + i];
		if (copy.op == RE_JMP) {
			copy.x += delta;
		} else if (copy.op == RE_SPLIT) {
			copy.x += delta;
			copy.y += delta;
		}
		if (prog_emit(c, &copy, NULL) != 0)
			return c->err;
	}
	return 0;
}

/* Insert an instruction at index at, shifting [at, len) right by 1 and
 * adjusting all jump targets >= at. */
static int prog_insert(struct re_compiler *c, size_t at, const struct re_insn *in)
{
	struct re_insn zero;
	memset(&zero, 0, sizeof(zero));
	if (prog_emit(c, &zero, NULL) != 0)
		return c->err;
	size_t old_len = c->prog.len - 1;
	for (size_t i = old_len; i > at; i--) {
		struct re_insn cur = c->prog.insns[i - 1];
		if (cur.op == RE_JMP && cur.x >= at)
			cur.x++;
		else if (cur.op == RE_SPLIT) {
			if (cur.x >= at)
				cur.x++;
			if (cur.y >= at)
				cur.y++;
		}
		c->prog.insns[i] = cur;
	}
	/* Also update jumps before `at` that target >= `at`. */
	for (size_t i = 0; i < at; i++) {
		struct re_insn *cur = &c->prog.insns[i];
		if (cur->op == RE_JMP && cur->x >= at)
			cur->x++;
		else if (cur->op == RE_SPLIT) {
			if (cur->x >= at)
				cur->x++;
			if (cur->y >= at)
				cur->y++;
		}
	}
	c->prog.insns[at] = *in;
	return 0;
}

static void add_posix_class(uint64_t bits[4], const char *name, size_t len)
{
	for (unsigned ch = 0; ch < 256; ch++) {
		int match = 0;
		if (len == 5 && memcmp(name, "alnum", 5) == 0)
			match = isalnum((int)ch);
		else if (len == 5 && memcmp(name, "alpha", 5) == 0)
			match = isalpha((int)ch);
		else if (len == 5 && memcmp(name, "blank", 5) == 0)
			match = (ch == ' ' || ch == '\t');
		else if (len == 5 && memcmp(name, "cntrl", 5) == 0)
			match = iscntrl((int)ch);
		else if (len == 5 && memcmp(name, "digit", 5) == 0)
			match = isdigit((int)ch);
		else if (len == 5 && memcmp(name, "graph", 5) == 0)
			match = isgraph((int)ch);
		else if (len == 5 && memcmp(name, "lower", 5) == 0)
			match = islower((int)ch);
		else if (len == 5 && memcmp(name, "print", 5) == 0)
			match = isprint((int)ch);
		else if (len == 5 && memcmp(name, "punct", 5) == 0)
			match = ispunct((int)ch);
		else if (len == 5 && memcmp(name, "space", 5) == 0)
			match = isspace((int)ch);
		else if (len == 5 && memcmp(name, "upper", 5) == 0)
			match = isupper((int)ch);
		else if (len == 6 && memcmp(name, "xdigit", 6) == 0)
			match = isxdigit((int)ch);
		if (match)
			bit_set(bits, (unsigned char)ch);
	}
}

static bool valid_posix_class_name(const char *name, size_t len)
{
	static const char *const names[] = {
		"alnum", "alpha", "blank", "cntrl", "digit", "graph",
		"lower", "print", "punct", "space", "upper", "xdigit",
	};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (strlen(names[i]) == len && memcmp(name, names[i], len) == 0)
			return true;
	}
	return false;
}

static int parse_bracket(struct re_compiler *c)
{
	/* c->pat[c->pos] == '[' */
	c->pos++;
	if (c->pos >= c->len) {
		c->err = TC_RE_ERR_SYNTAX;
		return c->err;
	}

	bool negate = false;
	if (c->pat[c->pos] == '^') {
		negate = true;
		c->pos++;
	}

	uint64_t bits[4] = {0, 0, 0, 0};
	bool first = true;
	bool closed = false;

	while (c->pos < c->len) {
		if (!first && c->pat[c->pos] == ']') {
			c->pos++;
			closed = true;
			break;
		}
		first = false;

		/* POSIX character class [:name:] */
		if (c->pat[c->pos] == '[' && c->pos + 1 < c->len &&
		    c->pat[c->pos + 1] == ':') {
			size_t j = c->pos + 2;
			while (j + 1 < c->len &&
			       !(c->pat[j] == ':' && c->pat[j + 1] == ']'))
				j++;
			if (j + 1 >= c->len) {
				c->err = TC_RE_ERR_SYNTAX;
				return c->err;
			}
			const char *cname = c->pat + c->pos + 2;
			size_t clen = j - (c->pos + 2);
			if (!valid_posix_class_name(cname, clen)) {
				c->err = TC_RE_ERR_SYNTAX;
				return c->err;
			}
			add_posix_class(bits, cname, clen);
			c->pos = j + 2;
			continue;
		}

		unsigned char lo = (unsigned char)c->pat[c->pos++];
		if (c->pos + 1 < c->len && c->pat[c->pos] == '-' &&
		    c->pat[c->pos + 1] != ']') {
			c->pos++; /* skip '-' */
			unsigned char hi = (unsigned char)c->pat[c->pos++];
			if (lo > hi) {
				c->err = TC_RE_ERR_SYNTAX;
				return c->err;
			}
			for (unsigned ch = lo; ch <= hi; ch++)
				bit_set(bits, (unsigned char)ch);
		} else {
			bit_set(bits, lo);
		}
	}

	if (!closed) {
		c->err = TC_RE_ERR_SYNTAX;
		return c->err;
	}

	if (negate) {
		for (int k = 0; k < 4; k++)
			bits[k] = ~bits[k];
	}

	struct re_insn in;
	memset(&in, 0, sizeof(in));
	in.op = RE_CLASS;
	memcpy(in.bits, bits, sizeof(bits));
	return prog_emit(c, &in, NULL);
}

static int parse_alt(struct re_compiler *c);

static int parse_atom(struct re_compiler *c)
{
	if (c->pos >= c->len)
		return 0;

	char ch = c->pat[c->pos];
	if (ch == '(') {
		if (c->depth >= TC_RE_DEPTH_MAX) {
			c->err = TC_RE_ERR_LIMIT;
			return c->err;
		}
		c->pos++;
		c->depth++;
		size_t group = c->prog.ngroups;
		bool track = (group < TC_RE_GROUPS_MAX);
		if (track) {
			c->prog.ngroups++;
			struct re_insn sv;
			memset(&sv, 0, sizeof(sv));
			sv.op = RE_SAVE;
			sv.slot = (unsigned short)(group * 2);
			if (prog_emit(c, &sv, NULL) != 0)
				return c->err;
		}
		if (parse_alt(c) != 0)
			return c->err;
		c->depth--;
		if (c->pos >= c->len || c->pat[c->pos] != ')') {
			c->err = TC_RE_ERR_SYNTAX;
			return c->err;
		}
		c->pos++;
		if (track) {
			struct re_insn sv;
			memset(&sv, 0, sizeof(sv));
			sv.op = RE_SAVE;
			sv.slot = (unsigned short)(group * 2 + 1);
			if (prog_emit(c, &sv, NULL) != 0)
				return c->err;
		}
		return 0;
	}

	if (ch == '[')
		return parse_bracket(c);

	if (ch == '.') {
		c->pos++;
		struct re_insn in;
		memset(&in, 0, sizeof(in));
		in.op = RE_ANY;
		return prog_emit(c, &in, NULL);
	}

	if (ch == '^') {
		c->pos++;
		struct re_insn in;
		memset(&in, 0, sizeof(in));
		in.op = RE_BOL;
		return prog_emit(c, &in, NULL);
	}

	if (ch == '$') {
		c->pos++;
		struct re_insn in;
		memset(&in, 0, sizeof(in));
		in.op = RE_EOL;
		return prog_emit(c, &in, NULL);
	}

	if (ch == '\\') {
		c->pos++;
		if (c->pos >= c->len) {
			c->err = TC_RE_ERR_SYNTAX;
			return c->err;
		}
		struct re_insn in;
		memset(&in, 0, sizeof(in));
		in.op = RE_CHAR;
		in.ch = (unsigned char)c->pat[c->pos++];
		return prog_emit(c, &in, NULL);
	}

	if (ch == '*' || ch == '+' || ch == '?') {
		c->err = TC_RE_ERR_SYNTAX;
		return c->err;
	}

	c->pos++;
	struct re_insn in;
	memset(&in, 0, sizeof(in));
	in.op = RE_CHAR;
	in.ch = (unsigned char)ch;
	return prog_emit(c, &in, NULL);
}

/* Parse a bounded repetition {m}, {m,}, or {m,n} at c->pat[c->pos] == '{'.
 * Returns 1 if parsed, 0 if '{' should be treated as literal (not a valid
 * bound prefix), or negative error code on invalid bound syntax. */
static int parse_bound(struct re_compiler *c, unsigned *out_min,
		       unsigned *out_max, bool *out_unbounded)
{
	size_t p = c->pos + 1;
	if (p >= c->len || !isdigit((unsigned char)c->pat[p]))
		return 0; /* POSIX ERE: unescaped '{' not followed by digit is literal */

	unsigned m = 0;
	while (p < c->len && isdigit((unsigned char)c->pat[p])) {
		unsigned d = (unsigned)(c->pat[p] - '0');
		if (m > TC_RE_REPEAT_MAX) {
			c->err = TC_RE_ERR_LIMIT;
			return c->err;
		}
		m = m * 10 + d;
		p++;
	}
	if (m > TC_RE_REPEAT_MAX) {
		c->err = TC_RE_ERR_LIMIT;
		return c->err;
	}

	unsigned n = m;
	bool unbounded = false;
	if (p < c->len && c->pat[p] == ',') {
		p++;
		if (p < c->len && c->pat[p] == '}') {
			unbounded = true;
		} else if (p < c->len && isdigit((unsigned char)c->pat[p])) {
			n = 0;
			while (p < c->len && isdigit((unsigned char)c->pat[p])) {
				unsigned d = (unsigned)(c->pat[p] - '0');
				if (n > TC_RE_REPEAT_MAX) {
					c->err = TC_RE_ERR_LIMIT;
					return c->err;
				}
				n = n * 10 + d;
				p++;
			}
			if (n > TC_RE_REPEAT_MAX) {
				c->err = TC_RE_ERR_LIMIT;
				return c->err;
			}
			if (n < m) {
				c->err = TC_RE_ERR_SYNTAX;
				return c->err;
			}
		} else {
			c->err = TC_RE_ERR_SYNTAX;
			return c->err;
		}
	}

	if (p >= c->len || c->pat[p] != '}') {
		c->err = TC_RE_ERR_SYNTAX;
		return c->err;
	}

	c->pos = p + 1;
	*out_min = m;
	*out_max = n;
	*out_unbounded = unbounded;
	return 1;
}

static int apply_star(struct re_compiler *c, size_t atom_start)
{
	/* Before: [atom_start .. end)
	 * After:  SPLIT(atom_start+1, end+2); [atom]; JMP(atom_start); */
	size_t atom_end = c->prog.len;
	struct re_insn sp;
	memset(&sp, 0, sizeof(sp));
	sp.op = RE_SPLIT;
	sp.x = atom_start + 1;
	sp.y = atom_end + 2;
	if (prog_insert(c, atom_start, &sp) != 0)
		return c->err;
	struct re_insn jmp;
	memset(&jmp, 0, sizeof(jmp));
	jmp.op = RE_JMP;
	jmp.x = atom_start;
	return prog_emit(c, &jmp, NULL);
}

static int apply_plus(struct re_compiler *c, size_t atom_start)
{
	/* [atom]; SPLIT(atom_start, end+1) */
	struct re_insn sp;
	memset(&sp, 0, sizeof(sp));
	sp.op = RE_SPLIT;
	sp.x = atom_start;
	sp.y = c->prog.len + 1;
	return prog_emit(c, &sp, NULL);
}

static int apply_quest(struct re_compiler *c, size_t atom_start)
{
	/* SPLIT(atom_start+1, end+1); [atom] */
	size_t atom_end = c->prog.len;
	struct re_insn sp;
	memset(&sp, 0, sizeof(sp));
	sp.op = RE_SPLIT;
	sp.x = atom_start + 1;
	sp.y = atom_end + 1;
	return prog_insert(c, atom_start, &sp);
}

static int apply_bound(struct re_compiler *c, size_t atom_start,
		       unsigned min_rep, unsigned max_rep, bool unbounded)
{
	size_t atom_end = c->prog.len;
	if (min_rep == 0 && !unbounded && max_rep == 0) {
		/* {0} matches empty string: discard atom instructions. */
		c->prog.len = atom_start;
		return 0;
	}

	/* Keep a temporary copy of the atom slice on heap so we can emit
	 * min_rep mandatory copies + optional/star tail cleanly. */
	size_t alen = atom_end - atom_start;
	if (alen == 0)
		return 0;

	/* First emit min_rep copies (1 is already at [atom_start, atom_end)). */
	if (min_rep == 0) {
		/* Turn the single existing copy into optional (or star if unbounded). */
		if (unbounded)
			return apply_star(c, atom_start);
		/* max_rep >= 1: wrap first copy in '?', then append (max_rep - 1)
		 * optional copies without shifting earlier jump targets. */
		if (apply_quest(c, atom_start) != 0)
			return c->err;
		size_t src_start = atom_start + 1;
		size_t src_end = atom_end + 1;
		for (unsigned i = 1; i < max_rep; i++) {
			struct re_insn sp;
			memset(&sp, 0, sizeof(sp));
			sp.op = RE_SPLIT;
			sp.x = c->prog.len + 1;
			sp.y = c->prog.len + 1 + alen;
			if (prog_emit(c, &sp, NULL) != 0)
				return c->err;
			if (prog_clone_slice(c, src_start, src_end) != 0)
				return c->err;
		}
		return 0;
	}

	/* min_rep >= 1: clone (min_rep - 1) mandatory copies. */
	for (unsigned i = 1; i < min_rep; i++) {
		if (prog_clone_slice(c, atom_start, atom_end) != 0)
			return c->err;
	}

	if (unbounded) {
		/* Add one more copy wrapped in '*'. */
		size_t star_start = c->prog.len;
		if (prog_clone_slice(c, atom_start, atom_end) != 0)
			return c->err;
		return apply_star(c, star_start);
	}

	/* Add (max_rep - min_rep) optional copies by emitting SPLIT before clone. */
	for (unsigned i = min_rep; i < max_rep; i++) {
		struct re_insn sp;
		memset(&sp, 0, sizeof(sp));
		sp.op = RE_SPLIT;
		sp.x = c->prog.len + 1;
		sp.y = c->prog.len + 1 + alen;
		if (prog_emit(c, &sp, NULL) != 0)
			return c->err;
		if (prog_clone_slice(c, atom_start, atom_end) != 0)
			return c->err;
	}
	return 0;
}

static int parse_piece(struct re_compiler *c)
{
	size_t atom_start = c->prog.len;
	if (parse_atom(c) != 0)
		return c->err;

	if (c->pos < c->len) {
		char q = c->pat[c->pos];
		if (q == '*') {
			c->pos++;
			return apply_star(c, atom_start);
		}
		if (q == '+') {
			c->pos++;
			return apply_plus(c, atom_start);
		}
		if (q == '?') {
			c->pos++;
			return apply_quest(c, atom_start);
		}
		if (q == '{') {
			unsigned m = 0, n = 0;
			bool unbounded = false;
			int br = parse_bound(c, &m, &n, &unbounded);
			if (br < 0)
				return br;
			if (br == 1)
				return apply_bound(c, atom_start, m, n, unbounded);
		}
	}
	return 0;
}

static int parse_concat(struct re_compiler *c)
{
	while (c->pos < c->len) {
		char ch = c->pat[c->pos];
		if (ch == '|' || ch == ')')
			break;
		if (parse_piece(c) != 0)
			return c->err;
	}
	return 0;
}

static int parse_alt(struct re_compiler *c)
{
	size_t lhs_start = c->prog.len;
	if (parse_concat(c) != 0)
		return c->err;

	while (c->pos < c->len && c->pat[c->pos] == '|') {
		c->pos++;
		/* Insert SPLIT(lhs_start + 1, rhs_start) at lhs_start, and JMP(end)
		 * between lhs and rhs. */
		struct re_insn sp;
		memset(&sp, 0, sizeof(sp));
		sp.op = RE_SPLIT;
		sp.x = lhs_start + 1;
		sp.y = 0; /* patched below */
		if (prog_insert(c, lhs_start, &sp) != 0)
			return c->err;

		struct re_insn jmp;
		memset(&jmp, 0, sizeof(jmp));
		jmp.op = RE_JMP;
		jmp.x = 0; /* patched below */
		size_t jmp_idx = 0;
		if (prog_emit(c, &jmp, &jmp_idx) != 0)
			return c->err;

		size_t rhs_start = c->prog.len;
		c->prog.insns[lhs_start].y = rhs_start;

		if (parse_concat(c) != 0)
			return c->err;

		c->prog.insns[jmp_idx].x = c->prog.len;
	}
	return 0;
}

static int compile_ere(const char *pat, size_t len, struct re_prog *out)
{
	struct re_compiler c;
	memset(&c, 0, sizeof(c));
	c.pat = pat;
	c.len = len;
	c.prog.ngroups = 1; /* group 0 = whole match */

	/* Unanchored prefix:
	 * 0: SPLIT(3, 1)   -- prefer matching at current pos over advancing
	 * 1: ANY
	 * 2: JMP(0)
	 * 3: SAVE(0)
	 * ... compiled pattern ...
	 * N: SAVE(1)
	 * N+1: MATCH */
	struct re_insn in;
	memset(&in, 0, sizeof(in));
	in.op = RE_SPLIT;
	in.x = 3;
	in.y = 1;
	if (prog_emit(&c, &in, NULL) != 0)
		goto fail;

	memset(&in, 0, sizeof(in));
	in.op = RE_ANY;
	if (prog_emit(&c, &in, NULL) != 0)
		goto fail;

	memset(&in, 0, sizeof(in));
	in.op = RE_JMP;
	in.x = 0;
	if (prog_emit(&c, &in, NULL) != 0)
		goto fail;

	memset(&in, 0, sizeof(in));
	in.op = RE_SAVE;
	in.slot = 0;
	if (prog_emit(&c, &in, NULL) != 0)
		goto fail;

	if (parse_alt(&c) != 0)
		goto fail;

	if (c.pos != c.len) {
		c.err = TC_RE_ERR_SYNTAX;
		goto fail;
	}

	memset(&in, 0, sizeof(in));
	in.op = RE_SAVE;
	in.slot = 1;
	if (prog_emit(&c, &in, NULL) != 0)
		goto fail;

	memset(&in, 0, sizeof(in));
	in.op = RE_MATCH;
	if (prog_emit(&c, &in, NULL) != 0)
		goto fail;

	*out = c.prog;
	return 0;

fail:
	free(c.prog.insns);
	return c.err ? c.err : TC_RE_ERR_SYNTAX;
}

/* ---- PikeVM Executor -------------------------------------------------- */

struct vm_thread {
	size_t pc;
	size_t saved[TC_RE_SLOTS_MAX];
};

struct vm_list {
	struct vm_thread *threads;
	size_t len;
};

struct eps_frame {
	bool is_undo;
	size_t pc;
	unsigned short slot;
	size_t old_val;
};

struct vm_state {
	const struct re_prog *prog;
	const char *str;
	size_t str_len;
	size_t nslots;
	unsigned *visited;
	unsigned gen;
	struct eps_frame *stack;
	size_t stack_cap;
};

static int add_thread(struct vm_state *vm, struct vm_list *list,
		      size_t start_pc, const size_t *init_saved, size_t sp)
{
	size_t cur_saved[TC_RE_SLOTS_MAX];
	memcpy(cur_saved, init_saved, vm->nslots * sizeof(size_t));

	size_t top = 0;
	vm->stack[top].is_undo = false;
	vm->stack[top].pc = start_pc;
	top++;

	while (top > 0) {
		struct eps_frame f = vm->stack[--top];
		if (f.is_undo) {
			cur_saved[f.slot] = f.old_val;
			continue;
		}
		size_t pc = f.pc;
		if (vm->visited[pc] == vm->gen)
			continue;
		vm->visited[pc] = vm->gen;

		const struct re_insn *in = &vm->prog->insns[pc];
		switch (in->op) {
		case RE_JMP:
			if (top >= vm->stack_cap)
				return TC_RE_ERR_LIMIT;
			vm->stack[top].is_undo = false;
			vm->stack[top].pc = in->x;
			top++;
			break;
		case RE_SPLIT:
			if (top + 2 > vm->stack_cap)
				return TC_RE_ERR_LIMIT;
			/* Push y first so x is popped and explored first (greedy). */
			vm->stack[top].is_undo = false;
			vm->stack[top].pc = in->y;
			top++;
			vm->stack[top].is_undo = false;
			vm->stack[top].pc = in->x;
			top++;
			break;
		case RE_SAVE:
			if (top + 2 > vm->stack_cap)
				return TC_RE_ERR_LIMIT;
			vm->stack[top].is_undo = true;
			vm->stack[top].slot = in->slot;
			vm->stack[top].old_val = cur_saved[in->slot];
			top++;
			cur_saved[in->slot] = sp;
			vm->stack[top].is_undo = false;
			vm->stack[top].pc = pc + 1;
			top++;
			break;
		case RE_BOL:
			if (sp == 0) {
				if (top >= vm->stack_cap)
					return TC_RE_ERR_LIMIT;
				vm->stack[top].is_undo = false;
				vm->stack[top].pc = pc + 1;
				top++;
			}
			break;
		case RE_EOL:
			if (sp == vm->str_len) {
				if (top >= vm->stack_cap)
					return TC_RE_ERR_LIMIT;
				vm->stack[top].is_undo = false;
				vm->stack[top].pc = pc + 1;
				top++;
			}
			break;
		case RE_CHAR:
		case RE_ANY:
		case RE_CLASS:
		case RE_MATCH: {
			struct vm_thread *t = &list->threads[list->len++];
			t->pc = pc;
			memcpy(t->saved, cur_saved, vm->nslots * sizeof(size_t));
			break;
		}
		}
	}
	return 0;
}

int tc_re_exec(const char *pattern, size_t pat_len,
	       const char *str, size_t str_len,
	       struct tc_re_match *groups, size_t ngroups,
	       size_t *out_ngroups)
{
	if (out_ngroups)
		*out_ngroups = 0;

	struct re_prog prog;
	int cr = compile_ere(pattern, pat_len, &prog);
	if (cr != 0)
		return cr;

	size_t ninsns = prog.len;
	size_t nslots = prog.ngroups * 2;
	size_t stack_cap = ninsns * 4 + 16;

	struct vm_thread *buf_a = malloc(ninsns * sizeof(*buf_a));
	struct vm_thread *buf_b = malloc(ninsns * sizeof(*buf_b));
	unsigned *visited = calloc(ninsns, sizeof(*visited));
	struct eps_frame *stack = malloc(stack_cap * sizeof(*stack));
	if (!buf_a || !buf_b || !visited || !stack) {
		free(buf_a);
		free(buf_b);
		free(visited);
		free(stack);
		free(prog.insns);
		return TC_RE_ERR_NOMEM;
	}

	struct vm_state vm = {
		.prog = &prog,
		.str = str,
		.str_len = str_len,
		.nslots = nslots,
		.visited = visited,
		.gen = 1,
		.stack = stack,
		.stack_cap = stack_cap,
	};

	struct vm_list cur = { .threads = buf_a, .len = 0 };
	struct vm_list nxt = { .threads = buf_b, .len = 0 };

	size_t init_saved[TC_RE_SLOTS_MAX];
	for (size_t i = 0; i < nslots; i++)
		init_saved[i] = SIZE_MAX;

	bool matched = false;
	size_t best_saved[TC_RE_SLOTS_MAX];
	int ret = TC_RE_NOMATCH;

	if (add_thread(&vm, &cur, 0, init_saved, 0) != 0) {
		ret = TC_RE_ERR_LIMIT;
		goto done;
	}

	for (size_t sp = 0; sp <= str_len; sp++) {
		nxt.len = 0;
		vm.gen++;
		if (vm.gen == 0) {
			memset(vm.visited, 0, ninsns * sizeof(*vm.visited));
			vm.gen = 1;
		}

		for (size_t i = 0; i < cur.len; i++) {
			struct vm_thread *t = &cur.threads[i];
			const struct re_insn *in = &prog.insns[t->pc];
			if (in->op == RE_MATCH) {
				matched = true;
				memcpy(best_saved, t->saved, nslots * sizeof(size_t));
				/* Higher-priority threads already ran; drop all lower-priority
				 * threads (including unanchored start-later threads). */
				break;
			}
			if (sp == str_len)
				continue;
			unsigned char ch = (unsigned char)str[sp];
			bool ok = false;
			if (in->op == RE_CHAR)
				ok = (ch == in->ch);
			else if (in->op == RE_ANY)
				ok = true;
			else if (in->op == RE_CLASS)
				ok = bit_test(in->bits, ch);
			if (ok) {
				if (add_thread(&vm, &nxt, t->pc + 1, t->saved, sp + 1) != 0) {
					ret = TC_RE_ERR_LIMIT;
					goto done;
				}
			}
		}

		struct vm_list tmp = cur;
		cur = nxt;
		nxt = tmp;
		if (cur.len == 0)
			break;
	}

	if (matched) {
		ret = TC_RE_MATCH;
		if (out_ngroups)
			*out_ngroups = prog.ngroups;
		if (groups) {
			size_t copy_n = prog.ngroups < ngroups ? prog.ngroups : ngroups;
			for (size_t g = 0; g < copy_n; g++) {
				groups[g].start = best_saved[g * 2];
				groups[g].end = best_saved[g * 2 + 1];
			}
		}
	}

done:
	free(buf_a);
	free(buf_b);
	free(visited);
	free(stack);
	free(prog.insns);
	return ret;
}
