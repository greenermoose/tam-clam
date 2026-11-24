/* SPDX-License-Identifier: GPL-3.0-or-later */
/* POSIX arithmetic expansion evaluator ($((...))), 64-bit signed with
 * checked overflow and short-circuiting. */
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tam-clam/arith.h"

enum atok_kind {
	AT_EOF = 0,
	AT_NUM,
	AT_IDENT,
	AT_LPAREN, AT_RPAREN,
	AT_LBRACKET, AT_RBRACKET,
	AT_COMMA, AT_QUEST, AT_COLON,
	AT_OROR, AT_ANDAND,
	AT_PIPE, AT_CARET, AT_AMP,
	AT_EQEQ, AT_NEQ,
	AT_LT, AT_LE, AT_GT, AT_GE,
	AT_LSHIFT, AT_RSHIFT,
	AT_PLUS, AT_MINUS,
	AT_STAR, AT_SLASH, AT_PERCENT,
	AT_TILDE, AT_BANG,
	AT_PLUSPLUS, AT_MINUSMINUS,
	AT_EQ, AT_PLUSEQ, AT_MINUSEQ, AT_STAREQ, AT_SLASHEQ, AT_PERCENTEQ,
	AT_LSHIFTEQ, AT_RSHIFTEQ, AT_AMPEQ, AT_CARETEQ, AT_PIPEEQ,
};

struct atok {
	enum atok_kind kind;
	int64_t num;
	const char *start;
	size_t len;
};

struct arith_ctx {
	struct tc_shell *sh;
	unsigned long line;
	const char *p;
	struct atok tok;
	int err;
	unsigned depth;
};

static int is_ident_start(int c)
{
	return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_ident_char(int c)
{
	return is_ident_start(c) || (c >= '0' && c <= '9');
}

static void skip_ws(struct arith_ctx *c)
{
	while (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' ||
	       *c->p == '\r' || *c->p == '\v' || *c->p == '\f')
		c->p++;
}

static int parse_int_const(struct arith_ctx *c, const char *s, size_t *consumed,
			   int64_t *out)
{
	int base = 10;
	const char *p = s;
	if (p[0] == '0') {
		if (p[1] == 'x' || p[1] == 'X') {
			base = 16;
			p += 2;
			if (!isxdigit((unsigned char)*p)) {
				tc_error(c->sh, c->line,
					 "arithmetic syntax error: invalid hex constant");
				c->err = TC_ARITH_ERR;
				return TC_ARITH_ERR;
			}
		} else {
			base = 8;
		}
	}

	uint64_t val = 0;
	while (*p) {
		unsigned d;
		if (*p >= '0' && *p <= '9')
			d = (unsigned)(*p - '0');
		else if (*p >= 'a' && *p <= 'f')
			d = (unsigned)(*p - 'a' + 10);
		else if (*p >= 'A' && *p <= 'F')
			d = (unsigned)(*p - 'A' + 10);
		else
			break;
		if ((int)d >= base) {
			tc_error(c->sh, c->line,
				 "arithmetic syntax error: invalid digit for base %d", base);
			c->err = TC_ARITH_ERR;
			return TC_ARITH_ERR;
		}
		if (val > ((uint64_t)INT64_MAX - d) / (uint64_t)base) {
			tc_error(c->sh, c->line, "arithmetic overflow");
			c->err = TC_ARITH_ERR;
			return TC_ARITH_ERR;
		}
		val = val * (uint64_t)base + d;
		p++;
	}
	if (is_ident_char((unsigned char)*p)) {
		tc_error(c->sh, c->line, "arithmetic syntax error: invalid number");
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	*consumed = (size_t)(p - s);
	*out = (int64_t)val;
	return TC_ARITH_OK;
}

static int next_tok(struct arith_ctx *c)
{
	skip_ws(c);
	c->tok.start = c->p;
	c->tok.len = 0;
	c->tok.num = 0;

	if (*c->p == '\0') {
		c->tok.kind = AT_EOF;
		return TC_ARITH_OK;
	}

	if (*c->p >= '0' && *c->p <= '9') {
		size_t n = 0;
		int64_t v = 0;
		int r = parse_int_const(c, c->p, &n, &v);
		if (r != TC_ARITH_OK)
			return r;
		c->tok.kind = AT_NUM;
		c->tok.num = v;
		c->tok.len = n;
		c->p += n;
		return TC_ARITH_OK;
	}

	if (is_ident_start((unsigned char)*c->p)) {
		const char *s = c->p;
		while (is_ident_char((unsigned char)*c->p))
			c->p++;
		c->tok.kind = AT_IDENT;
		c->tok.start = s;
		c->tok.len = (size_t)(c->p - s);
		return TC_ARITH_OK;
	}

	char a = c->p[0];
	char b = c->p[1];
	char d = (b != '\0') ? c->p[2] : '\0';

	if (a == '<' && b == '<' && d == '=') { c->tok.kind = AT_LSHIFTEQ; c->p += 3; return TC_ARITH_OK; }
	if (a == '>' && b == '>' && d == '=') { c->tok.kind = AT_RSHIFTEQ; c->p += 3; return TC_ARITH_OK; }

	if (a == '|' && b == '|') { c->tok.kind = AT_OROR; c->p += 2; return TC_ARITH_OK; }
	if (a == '&' && b == '&') { c->tok.kind = AT_ANDAND; c->p += 2; return TC_ARITH_OK; }
	if (a == '=' && b == '=') { c->tok.kind = AT_EQEQ; c->p += 2; return TC_ARITH_OK; }
	if (a == '!' && b == '=') { c->tok.kind = AT_NEQ; c->p += 2; return TC_ARITH_OK; }
	if (a == '<' && b == '=') { c->tok.kind = AT_LE; c->p += 2; return TC_ARITH_OK; }
	if (a == '>' && b == '=') { c->tok.kind = AT_GE; c->p += 2; return TC_ARITH_OK; }
	if (a == '<' && b == '<') { c->tok.kind = AT_LSHIFT; c->p += 2; return TC_ARITH_OK; }
	if (a == '>' && b == '>') { c->tok.kind = AT_RSHIFT; c->p += 2; return TC_ARITH_OK; }
	if (a == '+' && b == '+') { c->tok.kind = AT_PLUSPLUS; c->p += 2; return TC_ARITH_OK; }
	if (a == '-' && b == '-') { c->tok.kind = AT_MINUSMINUS; c->p += 2; return TC_ARITH_OK; }
	if (a == '+' && b == '=') { c->tok.kind = AT_PLUSEQ; c->p += 2; return TC_ARITH_OK; }
	if (a == '-' && b == '=') { c->tok.kind = AT_MINUSEQ; c->p += 2; return TC_ARITH_OK; }
	if (a == '*' && b == '=') { c->tok.kind = AT_STAREQ; c->p += 2; return TC_ARITH_OK; }
	if (a == '/' && b == '=') { c->tok.kind = AT_SLASHEQ; c->p += 2; return TC_ARITH_OK; }
	if (a == '%' && b == '=') { c->tok.kind = AT_PERCENTEQ; c->p += 2; return TC_ARITH_OK; }
	if (a == '&' && b == '=') { c->tok.kind = AT_AMPEQ; c->p += 2; return TC_ARITH_OK; }
	if (a == '^' && b == '=') { c->tok.kind = AT_CARETEQ; c->p += 2; return TC_ARITH_OK; }
	if (a == '|' && b == '=') { c->tok.kind = AT_PIPEEQ; c->p += 2; return TC_ARITH_OK; }

	c->p++;
	switch (a) {
	case '(': c->tok.kind = AT_LPAREN; return TC_ARITH_OK;
	case ')': c->tok.kind = AT_RPAREN; return TC_ARITH_OK;
	case '[': c->tok.kind = AT_LBRACKET; return TC_ARITH_OK;
	case ']': c->tok.kind = AT_RBRACKET; return TC_ARITH_OK;
	case ',': c->tok.kind = AT_COMMA; return TC_ARITH_OK;
	case '?': c->tok.kind = AT_QUEST; return TC_ARITH_OK;
	case ':': c->tok.kind = AT_COLON; return TC_ARITH_OK;
	case '|': c->tok.kind = AT_PIPE; return TC_ARITH_OK;
	case '^': c->tok.kind = AT_CARET; return TC_ARITH_OK;
	case '&': c->tok.kind = AT_AMP; return TC_ARITH_OK;
	case '<': c->tok.kind = AT_LT; return TC_ARITH_OK;
	case '>': c->tok.kind = AT_GT; return TC_ARITH_OK;
	case '+': c->tok.kind = AT_PLUS; return TC_ARITH_OK;
	case '-': c->tok.kind = AT_MINUS; return TC_ARITH_OK;
	case '*': c->tok.kind = AT_STAR; return TC_ARITH_OK;
	case '/': c->tok.kind = AT_SLASH; return TC_ARITH_OK;
	case '%': c->tok.kind = AT_PERCENT; return TC_ARITH_OK;
	case '~': c->tok.kind = AT_TILDE; return TC_ARITH_OK;
	case '!': c->tok.kind = AT_BANG; return TC_ARITH_OK;
	case '=': c->tok.kind = AT_EQ; return TC_ARITH_OK;
	default:
		tc_error(c->sh, c->line, "arithmetic syntax error near `%c'", a);
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
}

static int checked_add(struct arith_ctx *c, int64_t a, int64_t b, int64_t *out)
{
	if (__builtin_add_overflow(a, b, out)) {
		tc_error(c->sh, c->line, "arithmetic overflow");
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	return TC_ARITH_OK;
}

static int checked_sub(struct arith_ctx *c, int64_t a, int64_t b, int64_t *out)
{
	if (__builtin_sub_overflow(a, b, out)) {
		tc_error(c->sh, c->line, "arithmetic overflow");
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	return TC_ARITH_OK;
}

static int checked_mul(struct arith_ctx *c, int64_t a, int64_t b, int64_t *out)
{
	if (__builtin_mul_overflow(a, b, out)) {
		tc_error(c->sh, c->line, "arithmetic overflow");
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	return TC_ARITH_OK;
}

static int checked_div(struct arith_ctx *c, int64_t a, int64_t b, int64_t *out)
{
	if (b == 0) {
		tc_error(c->sh, c->line, "division by zero");
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	if (a == INT64_MIN && b == -1) {
		tc_error(c->sh, c->line, "arithmetic overflow");
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	*out = a / b;
	return TC_ARITH_OK;
}

static int checked_mod(struct arith_ctx *c, int64_t a, int64_t b, int64_t *out)
{
	if (b == 0) {
		tc_error(c->sh, c->line, "division by zero");
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	if (a == INT64_MIN && b == -1) {
		tc_error(c->sh, c->line, "arithmetic overflow");
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	*out = a % b;
	return TC_ARITH_OK;
}

static int checked_lshift(struct arith_ctx *c, int64_t a, int64_t b, int64_t *out)
{
	if (b < 0 || b >= 64) {
		tc_error(c->sh, c->line, "arithmetic shift count out of range");
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	*out = (int64_t)((uint64_t)a << (unsigned)b);
	return TC_ARITH_OK;
}

static int checked_rshift(struct arith_ctx *c, int64_t a, int64_t b, int64_t *out)
{
	if (b < 0 || b >= 64) {
		tc_error(c->sh, c->line, "arithmetic shift count out of range");
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	if (a >= 0) {
		*out = (int64_t)((uint64_t)a >> (unsigned)b);
	} else {
		/* Arithmetic right shift without signed-shift UB. */
		uint64_t u = (uint64_t)a;
		uint64_t fill = (b == 0) ? 0 : (~(uint64_t)0 << (64 - (unsigned)b));
		*out = (int64_t)((u >> (unsigned)b) | fill);
	}
	return TC_ARITH_OK;
}

struct arith_lval {
	const char *name;
	size_t nlen;
	bool has_sub;
	bool is_assoc;
	int64_t idx;
	const char *key;
};

static int parse_comma(struct arith_ctx *c, bool noeval, int64_t *out);
static int parse_assign(struct arith_ctx *c, bool noeval, int64_t *out);

static int parse_int_val(struct arith_ctx *c, const char *n, const char *val,
			 int64_t *out)
{
	if (!val) {
		if (c->sh->opt_nounset) {
			tc_error(c->sh, c->line, "%s: unbound variable", n);
			c->err = TC_ARITH_ERR;
			return TC_ARITH_ERR;
		}
		*out = 0;
		return TC_ARITH_OK;
	}
	while (*val == ' ' || *val == '\t' || *val == '\n' ||
	       *val == '\r' || *val == '\v' || *val == '\f')
		val++;
	if (*val == '\0') {
		*out = 0;
		return TC_ARITH_OK;
	}
	bool neg = false;
	if (*val == '+' || *val == '-') {
		neg = (*val == '-');
		val++;
	}
	size_t used = 0;
	int64_t v = 0;
	if (*val < '0' || *val > '9') {
		tc_error(c->sh, c->line, "%s: invalid integer value", n);
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	if (neg && strcmp(val, "9223372036854775808") == 0) {
		*out = INT64_MIN;
		return TC_ARITH_OK;
	}
	int r = parse_int_const(c, val, &used, &v);
	if (r != TC_ARITH_OK)
		return r;
	val += used;
	while (*val == ' ' || *val == '\t' || *val == '\n' ||
	       *val == '\r' || *val == '\v' || *val == '\f')
		val++;
	if (*val != '\0') {
		tc_error(c->sh, c->line, "%s: invalid integer value", n);
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	*out = neg ? -v : v;
	return TC_ARITH_OK;
}

/* Parse optional [subscript] right after an AT_IDENT token has been consumed. */
static int parse_lval_sub(struct arith_ctx *c, const char *name, size_t nlen,
			  bool noeval, struct arith_lval *lv)
{
	memset(lv, 0, sizeof(*lv));
	lv->name = name;
	lv->nlen = nlen;
	if (c->tok.kind != AT_LBRACKET)
		return TC_ARITH_OK;

	if (c->depth >= TC_ARITH_DEPTH_MAX) {
		tc_error(c->sh, c->line, "arithmetic nesting exceeds limit (%d)",
			 TC_ARITH_DEPTH_MAX);
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	lv->has_sub = true;

	char *n = tc_arena_strndup(&c->sh->arena, name, nlen);
	if (!n) {
		tc_error(c->sh, c->line, "out of memory");
		c->err = TC_ARITH_NOMEM;
		return TC_ARITH_NOMEM;
	}
	const struct tc_var_entry *e = tc_var_find(&c->sh->vars, n);
	if (e && (e->flags & TC_VAR_ASSOC)) {
		lv->is_assoc = true;
		/* Capture raw key text up to matching ']'. */
		const char *kstart = c->p;
		int bdepth = 1;
		while (*c->p && bdepth > 0) {
			if (*c->p == '[')
				bdepth++;
			else if (*c->p == ']')
				bdepth--;
			if (bdepth > 0)
				c->p++;
		}
		if (bdepth != 0) {
			tc_error(c->sh, c->line, "arithmetic syntax error: expected `]'");
			c->err = TC_ARITH_ERR;
			return TC_ARITH_ERR;
		}
		size_t klen = (size_t)(c->p - kstart);
		c->p++; /* consume ']' */
		lv->key = tc_arena_strndup(&c->sh->arena, kstart, klen);
		if (!lv->key) {
			tc_error(c->sh, c->line, "out of memory");
			c->err = TC_ARITH_NOMEM;
			return TC_ARITH_NOMEM;
		}
		return next_tok(c);
	}

	c->depth++;
	int r = next_tok(c);
	if (r == TC_ARITH_OK)
		r = parse_comma(c, noeval, &lv->idx);
	c->depth--;
	if (r != TC_ARITH_OK)
		return r;
	if (c->tok.kind != AT_RBRACKET) {
		tc_error(c->sh, c->line, "arithmetic syntax error: expected `]'");
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	return next_tok(c);
}

static int read_lval(struct arith_ctx *c, const struct arith_lval *lv, int64_t *out)
{
	char *n = tc_arena_strndup(&c->sh->arena, lv->name, lv->nlen);
	if (!n) {
		tc_error(c->sh, c->line, "out of memory");
		c->err = TC_ARITH_NOMEM;
		return TC_ARITH_NOMEM;
	}
	const char *val = NULL;
	if (!lv->has_sub) {
		val = tc_var_get(&c->sh->vars, n);
	} else {
		const struct tc_var_entry *e = tc_var_find(&c->sh->vars, n);
		if (e) {
			if (lv->is_assoc) {
				val = e->assoc ? tc_assoc_get(e->assoc, lv->key) : NULL;
			} else if (e->flags & TC_VAR_ARRAY) {
				if (tc_array_get(e->arr, lv->idx, &val) == TC_ARR_BADSUB) {
					tc_error(c->sh, c->line, "%s: bad array subscript", n);
					c->err = TC_ARITH_ERR;
					return TC_ARITH_ERR;
				}
			} else if (lv->idx == 0 || lv->idx == -1) {
				val = e->value;
			} else if (lv->idx < -1) {
				tc_error(c->sh, c->line, "%s: bad array subscript", n);
				c->err = TC_ARITH_ERR;
				return TC_ARITH_ERR;
			}
		}
	}
	return parse_int_val(c, n, val, out);
}

static int write_lval(struct arith_ctx *c, const struct arith_lval *lv, int64_t val)
{
	char *n = tc_arena_strndup(&c->sh->arena, lv->name, lv->nlen);
	if (!n) {
		tc_error(c->sh, c->line, "out of memory");
		c->err = TC_ARITH_NOMEM;
		return TC_ARITH_NOMEM;
	}
	char buf[32];
	snprintf(buf, sizeof(buf), "%" PRId64, val);
	int sr;
	if (!lv->has_sub) {
		sr = tc_var_set(&c->sh->vars, n, buf, 0);
	} else if (lv->is_assoc) {
		sr = tc_var_set_elem_key(&c->sh->vars, n, lv->key, buf, false, 0);
	} else {
		sr = tc_var_set_elem_idx(&c->sh->vars, n, lv->idx, buf, false, 0);
	}
	if (sr == TC_VAR_ERR_READONLY) {
		tc_error(c->sh, c->line, "%s: readonly variable", n);
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	if (sr == TC_VAR_ERR_BADSUB) {
		tc_error(c->sh, c->line, "%s: bad array subscript", n);
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	if (sr != TC_VAR_OK) {
		tc_error(c->sh, c->line, "out of memory");
		c->err = TC_ARITH_NOMEM;
		return TC_ARITH_NOMEM;
	}
	return TC_ARITH_OK;
}

static int parse_primary(struct arith_ctx *c, bool noeval, int64_t *out)
{
	if (c->tok.kind == AT_NUM) {
		*out = c->tok.num;
		return next_tok(c);
	}
	if (c->tok.kind == AT_IDENT) {
		const char *name = c->tok.start;
		size_t nlen = c->tok.len;
		int r = next_tok(c);
		if (r != TC_ARITH_OK)
			return r;
		struct arith_lval lv;
		if ((r = parse_lval_sub(c, name, nlen, noeval, &lv)) != TC_ARITH_OK)
			return r;
		/* Postfix ++ / -- */
		if (c->tok.kind == AT_PLUSPLUS || c->tok.kind == AT_MINUSMINUS) {
			enum atok_kind op = c->tok.kind;
			r = next_tok(c);
			if (r != TC_ARITH_OK)
				return r;
			if (noeval) {
				*out = 0;
				return TC_ARITH_OK;
			}
			int64_t cur = 0, nxt = 0;
			if ((r = read_lval(c, &lv, &cur)) != TC_ARITH_OK)
				return r;
			r = (op == AT_PLUSPLUS) ? checked_add(c, cur, 1, &nxt)
						: checked_sub(c, cur, 1, &nxt);
			if (r != TC_ARITH_OK)
				return r;
			if ((r = write_lval(c, &lv, nxt)) != TC_ARITH_OK)
				return r;
			*out = cur;
			return TC_ARITH_OK;
		}
		if (noeval) {
			*out = 0;
			return TC_ARITH_OK;
		}
		return read_lval(c, &lv, out);
	}
	if (c->tok.kind == AT_LPAREN) {
		if (c->depth >= TC_ARITH_DEPTH_MAX) {
			tc_error(c->sh, c->line, "arithmetic nesting exceeds limit (%d)",
				 TC_ARITH_DEPTH_MAX);
			c->err = TC_ARITH_ERR;
			return TC_ARITH_ERR;
		}
		c->depth++;
		int r = next_tok(c);
		if (r == TC_ARITH_OK)
			r = parse_comma(c, noeval, out);
		c->depth--;
		if (r != TC_ARITH_OK)
			return r;
		if (c->tok.kind != AT_RPAREN) {
			tc_error(c->sh, c->line, "arithmetic syntax error: expected `)'");
			c->err = TC_ARITH_ERR;
			return TC_ARITH_ERR;
		}
		return next_tok(c);
	}
	tc_error(c->sh, c->line, "arithmetic syntax error: expected operand");
	c->err = TC_ARITH_ERR;
	return TC_ARITH_ERR;
}

static int parse_unary(struct arith_ctx *c, bool noeval, int64_t *out)
{
	if (c->tok.kind == AT_PLUS || c->tok.kind == AT_MINUS ||
	    c->tok.kind == AT_TILDE || c->tok.kind == AT_BANG) {
		if (c->depth >= TC_ARITH_DEPTH_MAX) {
			tc_error(c->sh, c->line, "arithmetic nesting exceeds limit (%d)",
				 TC_ARITH_DEPTH_MAX);
			c->err = TC_ARITH_ERR;
			return TC_ARITH_ERR;
		}
		enum atok_kind op = c->tok.kind;
		c->depth++;
		int r = next_tok(c);
		int64_t v = 0;
		if (r == TC_ARITH_OK)
			r = parse_unary(c, noeval, &v);
		c->depth--;
		if (r != TC_ARITH_OK)
			return r;
		if (noeval) {
			*out = 0;
			return TC_ARITH_OK;
		}
		if (op == AT_PLUS) {
			*out = v;
		} else if (op == AT_MINUS) {
			if (v == INT64_MIN) {
				tc_error(c->sh, c->line, "arithmetic overflow");
				c->err = TC_ARITH_ERR;
				return TC_ARITH_ERR;
			}
			*out = -v;
		} else if (op == AT_TILDE) {
			*out = (int64_t)(~(uint64_t)v);
		} else {
			*out = (v == 0) ? 1 : 0;
		}
		return TC_ARITH_OK;
	}
	if (c->tok.kind == AT_PLUSPLUS || c->tok.kind == AT_MINUSMINUS) {
		enum atok_kind op = c->tok.kind;
		int r = next_tok(c);
		if (r != TC_ARITH_OK)
			return r;
		if (c->tok.kind != AT_IDENT) {
			tc_error(c->sh, c->line,
				 "arithmetic syntax error: expected variable name");
			c->err = TC_ARITH_ERR;
			return TC_ARITH_ERR;
		}
		const char *name = c->tok.start;
		size_t nlen = c->tok.len;
		r = next_tok(c);
		if (r != TC_ARITH_OK)
			return r;
		struct arith_lval lv;
		if ((r = parse_lval_sub(c, name, nlen, noeval, &lv)) != TC_ARITH_OK)
			return r;
		if (noeval) {
			*out = 0;
			return TC_ARITH_OK;
		}
		int64_t cur = 0, nxt = 0;
		if ((r = read_lval(c, &lv, &cur)) != TC_ARITH_OK)
			return r;
		r = (op == AT_PLUSPLUS) ? checked_add(c, cur, 1, &nxt)
					: checked_sub(c, cur, 1, &nxt);
		if (r != TC_ARITH_OK)
			return r;
		if ((r = write_lval(c, &lv, nxt)) != TC_ARITH_OK)
			return r;
		*out = nxt;
		return TC_ARITH_OK;
	}
	return parse_primary(c, noeval, out);
}

static int parse_mul(struct arith_ctx *c, bool noeval, int64_t *out)
{
	int r = parse_unary(c, noeval, out);
	while (r == TC_ARITH_OK &&
	       (c->tok.kind == AT_STAR || c->tok.kind == AT_SLASH ||
		c->tok.kind == AT_PERCENT)) {
		enum atok_kind op = c->tok.kind;
		int64_t rhs = 0;
		if ((r = next_tok(c)) != TC_ARITH_OK)
			return r;
		if ((r = parse_unary(c, noeval, &rhs)) != TC_ARITH_OK)
			return r;
		if (!noeval) {
			if (op == AT_STAR)
				r = checked_mul(c, *out, rhs, out);
			else if (op == AT_SLASH)
				r = checked_div(c, *out, rhs, out);
			else
				r = checked_mod(c, *out, rhs, out);
		}
	}
	return r;
}

static int parse_add(struct arith_ctx *c, bool noeval, int64_t *out)
{
	int r = parse_mul(c, noeval, out);
	while (r == TC_ARITH_OK &&
	       (c->tok.kind == AT_PLUS || c->tok.kind == AT_MINUS)) {
		enum atok_kind op = c->tok.kind;
		int64_t rhs = 0;
		if ((r = next_tok(c)) != TC_ARITH_OK)
			return r;
		if ((r = parse_mul(c, noeval, &rhs)) != TC_ARITH_OK)
			return r;
		if (!noeval) {
			if (op == AT_PLUS)
				r = checked_add(c, *out, rhs, out);
			else
				r = checked_sub(c, *out, rhs, out);
		}
	}
	return r;
}

static int parse_shift(struct arith_ctx *c, bool noeval, int64_t *out)
{
	int r = parse_add(c, noeval, out);
	while (r == TC_ARITH_OK &&
	       (c->tok.kind == AT_LSHIFT || c->tok.kind == AT_RSHIFT)) {
		enum atok_kind op = c->tok.kind;
		int64_t rhs = 0;
		if ((r = next_tok(c)) != TC_ARITH_OK)
			return r;
		if ((r = parse_add(c, noeval, &rhs)) != TC_ARITH_OK)
			return r;
		if (!noeval) {
			if (op == AT_LSHIFT)
				r = checked_lshift(c, *out, rhs, out);
			else
				r = checked_rshift(c, *out, rhs, out);
		}
	}
	return r;
}

static int parse_rel(struct arith_ctx *c, bool noeval, int64_t *out)
{
	int r = parse_shift(c, noeval, out);
	while (r == TC_ARITH_OK &&
	       (c->tok.kind == AT_LT || c->tok.kind == AT_LE ||
		c->tok.kind == AT_GT || c->tok.kind == AT_GE)) {
		enum atok_kind op = c->tok.kind;
		int64_t rhs = 0;
		if ((r = next_tok(c)) != TC_ARITH_OK)
			return r;
		if ((r = parse_shift(c, noeval, &rhs)) != TC_ARITH_OK)
			return r;
		if (!noeval) {
			if (op == AT_LT) *out = (*out < rhs);
			else if (op == AT_LE) *out = (*out <= rhs);
			else if (op == AT_GT) *out = (*out > rhs);
			else *out = (*out >= rhs);
		}
	}
	return r;
}

static int parse_eq(struct arith_ctx *c, bool noeval, int64_t *out)
{
	int r = parse_rel(c, noeval, out);
	while (r == TC_ARITH_OK &&
	       (c->tok.kind == AT_EQEQ || c->tok.kind == AT_NEQ)) {
		enum atok_kind op = c->tok.kind;
		int64_t rhs = 0;
		if ((r = next_tok(c)) != TC_ARITH_OK)
			return r;
		if ((r = parse_rel(c, noeval, &rhs)) != TC_ARITH_OK)
			return r;
		if (!noeval)
			*out = (op == AT_EQEQ) ? (*out == rhs) : (*out != rhs);
	}
	return r;
}

static int parse_bitand(struct arith_ctx *c, bool noeval, int64_t *out)
{
	int r = parse_eq(c, noeval, out);
	while (r == TC_ARITH_OK && c->tok.kind == AT_AMP) {
		int64_t rhs = 0;
		if ((r = next_tok(c)) != TC_ARITH_OK)
			return r;
		if ((r = parse_eq(c, noeval, &rhs)) != TC_ARITH_OK)
			return r;
		if (!noeval)
			*out = (int64_t)((uint64_t)*out & (uint64_t)rhs);
	}
	return r;
}

static int parse_bitxor(struct arith_ctx *c, bool noeval, int64_t *out)
{
	int r = parse_bitand(c, noeval, out);
	while (r == TC_ARITH_OK && c->tok.kind == AT_CARET) {
		int64_t rhs = 0;
		if ((r = next_tok(c)) != TC_ARITH_OK)
			return r;
		if ((r = parse_bitand(c, noeval, &rhs)) != TC_ARITH_OK)
			return r;
		if (!noeval)
			*out = (int64_t)((uint64_t)*out ^ (uint64_t)rhs);
	}
	return r;
}

static int parse_bitor(struct arith_ctx *c, bool noeval, int64_t *out)
{
	int r = parse_bitxor(c, noeval, out);
	while (r == TC_ARITH_OK && c->tok.kind == AT_PIPE) {
		int64_t rhs = 0;
		if ((r = next_tok(c)) != TC_ARITH_OK)
			return r;
		if ((r = parse_bitxor(c, noeval, &rhs)) != TC_ARITH_OK)
			return r;
		if (!noeval)
			*out = (int64_t)((uint64_t)*out | (uint64_t)rhs);
	}
	return r;
}

static int parse_logand(struct arith_ctx *c, bool noeval, int64_t *out)
{
	int r = parse_bitor(c, noeval, out);
	while (r == TC_ARITH_OK && c->tok.kind == AT_ANDAND) {
		bool skip = noeval || (*out == 0);
		int64_t rhs = 0;
		if ((r = next_tok(c)) != TC_ARITH_OK)
			return r;
		if ((r = parse_bitor(c, skip, &rhs)) != TC_ARITH_OK)
			return r;
		if (!noeval)
			*out = (!skip && rhs != 0) ? 1 : 0;
	}
	return r;
}

static int parse_logor(struct arith_ctx *c, bool noeval, int64_t *out)
{
	int r = parse_logand(c, noeval, out);
	while (r == TC_ARITH_OK && c->tok.kind == AT_OROR) {
		bool skip = noeval || (*out != 0);
		int64_t rhs = 0;
		if ((r = next_tok(c)) != TC_ARITH_OK)
			return r;
		if ((r = parse_logand(c, skip, &rhs)) != TC_ARITH_OK)
			return r;
		if (!noeval)
			*out = (skip || rhs != 0) ? 1 : 0;
	}
	return r;
}

static int parse_ternary(struct arith_ctx *c, bool noeval, int64_t *out)
{
	int r = parse_logor(c, noeval, out);
	if (r != TC_ARITH_OK || c->tok.kind != AT_QUEST)
		return r;
	if (c->depth >= TC_ARITH_DEPTH_MAX) {
		tc_error(c->sh, c->line, "arithmetic nesting exceeds limit (%d)",
			 TC_ARITH_DEPTH_MAX);
		c->err = TC_ARITH_ERR;
		return TC_ARITH_ERR;
	}
	bool cond = (*out != 0);
	int64_t tv = 0, fv = 0;
	c->depth++;
	r = next_tok(c);
	if (r == TC_ARITH_OK)
		r = parse_comma(c, noeval || !cond, &tv);
	if (r == TC_ARITH_OK && c->tok.kind != AT_COLON) {
		tc_error(c->sh, c->line, "arithmetic syntax error: expected `:'");
		c->err = TC_ARITH_ERR;
		r = TC_ARITH_ERR;
	}
	if (r == TC_ARITH_OK)
		r = next_tok(c);
	if (r == TC_ARITH_OK)
		r = parse_ternary(c, noeval || cond, &fv);
	c->depth--;
	if (r != TC_ARITH_OK)
		return r;
	if (!noeval)
		*out = cond ? tv : fv;
	return TC_ARITH_OK;
}

static bool is_assign_op(enum atok_kind k)
{
	return k == AT_EQ || k == AT_PLUSEQ || k == AT_MINUSEQ ||
	       k == AT_STAREQ || k == AT_SLASHEQ || k == AT_PERCENTEQ ||
	       k == AT_LSHIFTEQ || k == AT_RSHIFTEQ || k == AT_AMPEQ ||
	       k == AT_CARETEQ || k == AT_PIPEEQ;
}

static int parse_assign(struct arith_ctx *c, bool noeval, int64_t *out)
{
	/* Look ahead if current token is IDENT (with optional [sub]) followed by
	 * an assignment op. */
	if (c->tok.kind == AT_IDENT) {
		const char *save_p = c->p;
		struct atok save_tok = c->tok;
		int r = next_tok(c);
		if (r != TC_ARITH_OK)
			return r;
		struct arith_lval lv;
		if (c->tok.kind == AT_LBRACKET) {
			/* Probe past [...] without evaluating side effects first. */
			const char *p2 = c->p;
			int bdepth = 1;
			while (*p2 && bdepth > 0) {
				if (*p2 == '[') bdepth++;
				else if (*p2 == ']') bdepth--;
				p2++;
			}
			while (*p2 == ' ' || *p2 == '\t' || *p2 == '\n' ||
			       *p2 == '\r' || *p2 == '\v' || *p2 == '\f')
				p2++;
			bool looks_assign = false;
			if (bdepth == 0) {
				if (p2[0] == '=' && p2[1] != '=')
					looks_assign = true;
				else if ((p2[0] == '+' || p2[0] == '-' || p2[0] == '*' ||
					  p2[0] == '/' || p2[0] == '%' || p2[0] == '&' ||
					  p2[0] == '^' || p2[0] == '|') && p2[1] == '=')
					looks_assign = true;
				else if (((p2[0] == '<' && p2[1] == '<') ||
					  (p2[0] == '>' && p2[1] == '>')) && p2[2] == '=')
					looks_assign = true;
			}
			if (!looks_assign) {
				c->p = save_p;
				c->tok = save_tok;
				return parse_ternary(c, noeval, out);
			}
			if ((r = parse_lval_sub(c, save_tok.start, save_tok.len, noeval, &lv)) != TC_ARITH_OK)
				return r;
		} else {
			memset(&lv, 0, sizeof(lv));
			lv.name = save_tok.start;
			lv.nlen = save_tok.len;
		}
		if (is_assign_op(c->tok.kind)) {
			if (c->depth >= TC_ARITH_DEPTH_MAX) {
				tc_error(c->sh, c->line,
					 "arithmetic nesting exceeds limit (%d)",
					 TC_ARITH_DEPTH_MAX);
				c->err = TC_ARITH_ERR;
				return TC_ARITH_ERR;
			}
			enum atok_kind op = c->tok.kind;
			c->depth++;
			r = next_tok(c);
			int64_t rhs = 0;
			if (r == TC_ARITH_OK)
				r = parse_assign(c, noeval, &rhs);
			c->depth--;
			if (r != TC_ARITH_OK)
				return r;
			if (noeval) {
				*out = 0;
				return TC_ARITH_OK;
			}
			int64_t val = rhs;
			if (op != AT_EQ) {
				int64_t cur = 0;
				if ((r = read_lval(c, &lv, &cur)) != TC_ARITH_OK)
					return r;
				switch (op) {
				case AT_PLUSEQ:    r = checked_add(c, cur, rhs, &val); break;
				case AT_MINUSEQ:   r = checked_sub(c, cur, rhs, &val); break;
				case AT_STAREQ:    r = checked_mul(c, cur, rhs, &val); break;
				case AT_SLASHEQ:   r = checked_div(c, cur, rhs, &val); break;
				case AT_PERCENTEQ: r = checked_mod(c, cur, rhs, &val); break;
				case AT_LSHIFTEQ:  r = checked_lshift(c, cur, rhs, &val); break;
				case AT_RSHIFTEQ:  r = checked_rshift(c, cur, rhs, &val); break;
				case AT_AMPEQ:     val = (int64_t)((uint64_t)cur & (uint64_t)rhs); break;
				case AT_CARETEQ:   val = (int64_t)((uint64_t)cur ^ (uint64_t)rhs); break;
				case AT_PIPEEQ:    val = (int64_t)((uint64_t)cur | (uint64_t)rhs); break;
				default: break;
				}
				if (r != TC_ARITH_OK)
					return r;
			}
			if ((r = write_lval(c, &lv, val)) != TC_ARITH_OK)
				return r;
			*out = val;
			return TC_ARITH_OK;
		}
		/* Not an assignment: restore IDENT token and parse normally. */
		c->p = save_p;
		c->tok = save_tok;
	}
	return parse_ternary(c, noeval, out);
}

static int parse_comma(struct arith_ctx *c, bool noeval, int64_t *out)
{
	int r = parse_assign(c, noeval, out);
	while (r == TC_ARITH_OK && c->tok.kind == AT_COMMA) {
		if ((r = next_tok(c)) != TC_ARITH_OK)
			return r;
		r = parse_assign(c, noeval, out);
	}
	return r;
}

int tc_arith_eval(struct tc_shell *sh, const char *expr, unsigned long line,
		  int64_t *out)
{
	struct arith_ctx ctx = {
		.sh = sh,
		.line = line,
		.p = expr ? expr : "",
		.err = TC_ARITH_OK,
		.depth = 0,
	};
	int r = next_tok(&ctx);
	if (r != TC_ARITH_OK)
		return r;
	if (ctx.tok.kind == AT_EOF) {
		*out = 0;
		return TC_ARITH_OK;
	}
	int64_t val = 0;
	r = parse_comma(&ctx, false, &val);
	if (r != TC_ARITH_OK)
		return r;
	if (ctx.tok.kind != AT_EOF) {
		tc_error(sh, line, "arithmetic syntax error: unexpected token");
		return TC_ARITH_ERR;
	}
	*out = val;
	return TC_ARITH_OK;
}
