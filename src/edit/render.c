/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ANSI/xterm + TERM=linux multi-line renderer on libtam (tam_tty, tam_uni). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tam-clam/edit.h"
#include "tam-clam/highlight.h"
#include "tam_uni.h"

struct out_buf {
	char *data;
	size_t len;
	size_t cap;
	bool oom;
};

static void ob_append(struct out_buf *ob, const char *s, size_t n)
{
	if (ob->oom || n == 0)
		return;
	if (ob->len + n > ob->cap) {
		size_t ncap = ob->cap ? ob->cap * 2 : 512;
		while (ncap < ob->len + n)
			ncap *= 2;
		char *nd = realloc(ob->data, ncap);
		if (!nd) {
			ob->oom = true;
			return;
		}
		ob->data = nd;
		ob->cap = ncap;
	}
	memcpy(ob->data + ob->len, s, n);
	ob->len += n;
}

static void ob_str(struct out_buf *ob, const char *s)
{
	ob_append(ob, s, strlen(s));
}

/* Visible display width of a prompt string, skipping CSI SGR escapes. */
static int prompt_width(const char *p)
{
	if (!p)
		return 0;
	int w = 0;
	size_t len = strlen(p);
	size_t i = 0;
	while (i < len) {
		if ((unsigned char)p[i] == 0x1B && i + 1 < len && p[i + 1] == '[') {
			i += 2;
			while (i < len && !((unsigned char)p[i] >= 0x40 &&
					    (unsigned char)p[i] <= 0x7E))
				i++;
			if (i < len)
				i++;
			continue;
		}
		uint32_t cp = 0;
		size_t n = tam_utf8_decode(p + i, len - i, &cp);
		if (n == 0)
			n = 1;
		int cw = tam_uni_width(cp);
		if (cw > 0)
			w += cw;
		i += n;
	}
	return w;
}

void tc_render_init(struct tc_render_state *rs)
{
	rs->prev_rows = 1;
	rs->prev_cursor_row = 0;
	rs->active = false;
}

static void advance_col(int cols, int cw, int *row, int *col)
{
	if (cw <= 0)
		return;
	if (*col + cw > cols) {
		(*row)++;
		*col = 0;
	}
	*col += cw;
	if (*col >= cols) {
		(*row)++;
		*col = 0;
	}
}

static const char *sgr_for_kind(unsigned char k)
{
	switch ((enum tc_hl_kind)k) {
	case TC_HL_CMD_OK:  return "\x1b[32m";    /* green */
	case TC_HL_CMD_BAD: return "\x1b[1;31m";  /* bold red */
	case TC_HL_KEYWORD: return "\x1b[35m";    /* magenta */
	case TC_HL_STRING:  return "\x1b[33m";    /* yellow */
	case TC_HL_VAR:     return "\x1b[36m";    /* cyan */
	case TC_HL_OP:      return "\x1b[36m";    /* cyan */
	case TC_HL_COMMENT: return "\x1b[2m";     /* dim */
	case TC_HL_ERROR:   return "\x1b[1;31m";  /* bold red */
	case TC_HL_NONE:
	default:            return "\x1b[0m";
	}
}

static int render_internal(struct tc_render_state *rs, tam_tty_t *tty,
			   const struct tc_edit_buf *buf,
			   const char *ps1, const char *ps2,
			   const unsigned char *styles,
			   const char *ghost,
			   const char *overlay, bool finishing)
{
	int cols = tty->cols > 0 ? tty->cols : 80;
	bool use_color = !finishing && styles != NULL && tty->colors > 0;
	struct out_buf ob = {0};

	/* Move back to row 0, column 0 of our previous frame. */
	ob_str(&ob, "\r");
	if (rs->active && rs->prev_cursor_row > 0) {
		char seq[32];
		int n = snprintf(seq, sizeof(seq), "\x1b[%dA", rs->prev_cursor_row);
		if (n > 0)
			ob_append(&ob, seq, (size_t)n);
	}
	/* Clear from cursor to end of screen. */
	ob_str(&ob, "\x1b[J");

	size_t sel0 = 0, sel1 = 0;
	bool has_sel = !finishing && tc_ebuf_sel_range(buf, &sel0, &sel1);
	bool in_rev = false;
	unsigned char cur_style = TC_HL_NONE;

	int row = 0, col = 0;
	int cur_row = 0, cur_col = 0;
	bool cur_set = false;

	const char *p1 = ps1 ? ps1 : "";
	const char *p2 = ps2 ? ps2 : "";
	int w1 = prompt_width(p1);
	int w2 = prompt_width(p2);

	ob_str(&ob, p1);
	row = w1 / cols;
	col = w1 % cols;

	size_t i = 0;
	while (i <= buf->len) {
		if (!cur_set && i == buf->cursor) {
			cur_row = row;
			cur_col = col;
			cur_set = true;
		}
		if (i == buf->len)
			break;

		bool want_rev = has_sel && (i >= sel0 && i < sel1);
		unsigned char want_style = (use_color && !want_rev) ? styles[i] : TC_HL_NONE;

		if (want_rev != in_rev || (!want_rev && want_style != cur_style)) {
			if (in_rev || cur_style != TC_HL_NONE)
				ob_str(&ob, "\x1b[0m");
			in_rev = want_rev;
			cur_style = want_rev ? TC_HL_NONE : want_style;
			if (in_rev)
				ob_str(&ob, "\x1b[7m");
			else if (cur_style != TC_HL_NONE)
				ob_str(&ob, sgr_for_kind(cur_style));
		}

		unsigned char ch = (unsigned char)buf->data[i];
		if (ch == '\n') {
			if (in_rev || cur_style != TC_HL_NONE) {
				ob_str(&ob, "\x1b[0m");
				in_rev = false;
				cur_style = TC_HL_NONE;
			}
			ob_str(&ob, "\r\n");
			ob_str(&ob, p2);
			row++;
			row += w2 / cols;
			col = w2 % cols;
			i++;
			continue;
		}

		if (ch == '\t') {
			/* Expand tab to 4 spaces visually for predictable cursor math. */
			for (int k = 0; k < 4; k++) {
				ob_append(&ob, " ", 1);
				advance_col(cols, 1, &row, &col);
			}
			i++;
			continue;
		}

		if (ch < 0x20 || ch == 0x7F) {
			char caret[2] = {'^', ch == 0x7F ? '?' : (char)('@' + ch)};
			ob_append(&ob, caret, 2);
			advance_col(cols, 2, &row, &col);
			i++;
			continue;
		}

		uint32_t cp = 0;
		size_t n = tam_utf8_decode(buf->data + i, buf->len - i, &cp);
		if (n == 0)
			n = 1;
		int cw = tam_uni_width(cp);
		if (cw < 0)
			cw = 1;
		if (cw == 2 && col + 2 > cols) {
			/* Wide char wraps to next row cleanly. */
			ob_str(&ob, "\r\n");
			row++;
			col = 0;
		}
		ob_append(&ob, buf->data + i, n);
		col += cw;
		if (col >= cols) {
			/* Force explicit wrap so terminal cursor matches row/col. */
			if (i + n == buf->len && cur_set && cur_row == row && cur_col == col) {
				ob_str(&ob, "\r\n");
			} else if (col == cols) {
				ob_str(&ob, "\r\n");
			}
			row++;
			col = 0;
		}
		i += n;
	}

	if (in_rev || cur_style != TC_HL_NONE)
		ob_str(&ob, "\x1b[0m");

	if (!finishing && ghost && *ghost) {
		ob_str(&ob, "\x1b[2m");
		const char *gp = ghost;
		while (*gp && *gp != '\n') {
			uint32_t cp = 0;
			size_t n = tam_utf8_decode(gp, strlen(gp), &cp);
			if (n == 0)
				n = 1;
			int cw = tam_uni_width(cp);
			if (cw < 0)
				cw = 1;
			if (cw == 2 && col + 2 > cols) {
				ob_str(&ob, "\r\n");
				row++;
				col = 0;
			}
			ob_append(&ob, gp, n);
			col += cw;
			if (col >= cols) {
				ob_str(&ob, "\r\n");
				row++;
				col = 0;
			}
			gp += n;
		}
		ob_str(&ob, "\x1b[0m");
	}

	if (finishing) {
		ob_str(&ob, "\r\n");
		int rc = ob.oom ? -1 : tam_tty_write(tty, ob.data, ob.len);
		free(ob.data);
		rs->active = false;
		rs->prev_rows = 1;
		rs->prev_cursor_row = 0;
		return rc;
	}

	int end_row = row;
	if (overlay && *overlay) {
		ob_str(&ob, "\r\n");
		end_row++;
		const char *op = overlay;
		int ocol = 0;
		while (*op) {
			if (*op == '\n') {
				ob_str(&ob, "\r\n");
				end_row++;
				ocol = 0;
				op++;
				continue;
			}
			uint32_t cp = 0;
			size_t n = tam_utf8_decode(op, strlen(op), &cp);
			if (n == 0)
				n = 1;
			int cw = tam_uni_width(cp);
			if (cw < 0)
				cw = 1;
			if (ocol + cw > cols) {
				ob_str(&ob, "\r\n");
				end_row++;
				ocol = 0;
			}
			ob_append(&ob, op, n);
			ocol += cw;
			op += n;
		}
	}

	/* Position cursor at (cur_row, cur_col). */
	int up = end_row - cur_row;
	if (up > 0) {
		char seq[32];
		int n = snprintf(seq, sizeof(seq), "\x1b[%dA", up);
		if (n > 0)
			ob_append(&ob, seq, (size_t)n);
	}
	ob_str(&ob, "\r");
	if (cur_col > 0) {
		char seq[32];
		int n = snprintf(seq, sizeof(seq), "\x1b[%dC", cur_col);
		if (n > 0)
			ob_append(&ob, seq, (size_t)n);
	}

	int rc = 0;
	if (ob.oom) {
		rc = -1;
	} else {
		(void)tam_tty_frame_begin(tty);
		rc = tam_tty_write(tty, ob.data, ob.len);
		(void)tam_tty_frame_end(tty);
	}
	free(ob.data);
	rs->active = true;
	rs->prev_rows = end_row + 1;
	rs->prev_cursor_row = cur_row;
	return rc;
}

int tc_render_draw(struct tc_render_state *rs, tam_tty_t *tty,
		   const struct tc_edit_buf *buf,
		   const char *ps1, const char *ps2,
		   const unsigned char *styles,
		   const char *ghost,
		   const char *overlay)
{
	return render_internal(rs, tty, buf, ps1, ps2, styles, ghost, overlay, false);
}

int tc_render_finish(struct tc_render_state *rs, tam_tty_t *tty,
		     const struct tc_edit_buf *buf,
		     const char *ps1, const char *ps2)
{
	return render_internal(rs, tty, buf, ps1, ps2, NULL, NULL, NULL, true);
}

/* Base64-encode and emit OSC 52 clipboard write (skipped on TERM=linux). */
int tc_render_osc52(tam_tty_t *tty, bool is_linux_vt,
		    const char *data, size_t len)
{
	static const char b64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	if (!tty || !tty->interactive || is_linux_vt || !data || len == 0)
		return 0;
	if (len > TC_EDIT_BUF_MAX)
		len = TC_EDIT_BUF_MAX;
	size_t enc_len = ((len + 2) / 3) * 4;
	char *out = malloc(enc_len + 16);
	if (!out)
		return -1;
	size_t pos = 0;
	memcpy(out, "\x1b]52;c;", 7);
	pos = 7;
	for (size_t i = 0; i < len; i += 3) {
		unsigned a = (unsigned char)data[i];
		unsigned b = (i + 1 < len) ? (unsigned char)data[i + 1] : 0;
		unsigned c = (i + 2 < len) ? (unsigned char)data[i + 2] : 0;
		unsigned triple = (a << 16) | (b << 8) | c;
		out[pos++] = b64[(triple >> 18) & 0x3F];
		out[pos++] = b64[(triple >> 12) & 0x3F];
		out[pos++] = (i + 1 < len) ? b64[(triple >> 6) & 0x3F] : '=';
		out[pos++] = (i + 2 < len) ? b64[triple & 0x3F] : '=';
	}
	out[pos++] = '\x07';
	int rc = tam_tty_write(tty, out, pos);
	free(out);
	return rc;
}
