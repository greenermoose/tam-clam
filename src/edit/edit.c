/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Interactive line editor event loop, bracketed paste, overlays, $EDITOR,
 * termios management, and the `bind` builtin. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tam-clam/builtins.h"
#include "tam-clam/edit.h"
#include "tam-clam/shell.h"
#include "tam-clam/signals.h"

int tc_editor_init(struct tc_editor *ed, int in_fd, int out_fd)
{
	memset(ed, 0, sizeof(*ed));
	ed->in_fd = in_fd;
	ed->out_fd = out_fd;
	const char *term = getenv("TERM");
	ed->is_linux_vt = (term != NULL && strcmp(term, "linux") == 0);
	tc_keymap_init_defaults(&ed->keymap);
	tc_clip_init(&ed->clip);
	if (isatty(in_fd) && tcgetattr(in_fd, &ed->orig_termios) == 0)
		ed->has_orig_termios = true;
	return 0;
}

void tc_editor_configure_child_termios(struct tc_editor *ed)
{
	if (!ed || !ed->has_orig_termios)
		return;
	struct termios t = ed->orig_termios;
	/* Per tamlinux/arch/keybindings.md & arch/keys.md:
	 *   VINTR = Ctrl+\ (0x1C), VQUIT disabled, VSUSP = Ctrl+P (0x10). */
	t.c_cc[VINTR] = 0x1C;
#ifdef _POSIX_VDISABLE
	t.c_cc[VQUIT] = _POSIX_VDISABLE;
#else
	t.c_cc[VQUIT] = 0;
#endif
	t.c_cc[VSUSP] = 0x10;
	(void)tcsetattr(ed->in_fd, TCSANOW, &t);
}

void tc_editor_restore_orig_termios(struct tc_editor *ed)
{
	if (!ed || !ed->has_orig_termios)
		return;
	(void)tcsetattr(ed->in_fd, TCSANOW, &ed->orig_termios);
}

void tc_editor_finish(struct tc_editor *ed)
{
	if (!ed)
		return;
	tc_editor_restore_orig_termios(ed);
	tc_clip_finish(&ed->clip);
	memset(ed, 0, sizeof(*ed));
}

/* Lightweight lexical check: true if the buffer clearly needs another line
 * (unclosed quote, trailing backslash, or trailing pipe/and-or operator). */
static bool buffer_incomplete(const char *s, size_t len)
{
	bool in_sq = false, in_dq = false, esc = false;
	for (size_t i = 0; i < len; i++) {
		char c = s[i];
		if (esc) {
			esc = false;
			continue;
		}
		if (in_sq) {
			if (c == '\'')
				in_sq = false;
			continue;
		}
		if (in_dq) {
			if (c == '\\')
				esc = true;
			else if (c == '"')
				in_dq = false;
			continue;
		}
		if (c == '\\') {
			esc = true;
		} else if (c == '\'') {
			in_sq = true;
		} else if (c == '"') {
			in_dq = true;
		} else if (c == '#') {
			if (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t' ||
			    s[i - 1] == '\n' || s[i - 1] == ';') {
				while (i < len && s[i] != '\n')
					i++;
			}
		}
	}
	if (in_sq || in_dq || esc)
		return true;
	/* Trailing |, &&, || */
	size_t end = len;
	while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t'))
		end--;
	if (end > 0 && s[end - 1] == '|')
		return true;
	if (end >= 2 && s[end - 1] == '&' && s[end - 2] == '&')
		return true;
	return false;
}

/* Guidance overlay text (Ctrl+G). */
static const char guidance_text[] =
	"--- tam-clam guidance (Escape to close) ---\n"
	"Ctrl+X Cut   Ctrl+C Copy    Ctrl+V Paste (Alt+V prev)    Ctrl+Z Undo    Ctrl+Y Redo\n"
	"Ctrl+A All    Ctrl+K Clear   Ctrl+L Redraw  Ctrl+E Run    Ctrl+Q Quit  Ctrl+\\ Cancel\n"
	"Ctrl+Left/Right (Alt+,/.) Word   Ctrl+Home/End (Alt+</>) Buffer   Alt+M Mark\n"
	"Alt+Enter Newline   Alt+E $EDITOR   Alt+Q Verbatim   Alt+# Comment   Alt+K/Shift+K Del";

/* Build filtered widget palette text for Ctrl+E. */
static enum tc_widget build_palette_overlay(const char *query, char *out, size_t cap)
{
	enum tc_widget first = TC_W_NONE;
	size_t pos = 0;
	pos += (size_t)snprintf(out + pos, cap - pos,
				"--- execute widget: %s_ (Enter run, Escape close) ---",
				query);
	size_t shown = 0;
	for (size_t i = 0; i < tc_widget_count() && shown < 8; i++) {
		const struct tc_widget_info *w = tc_widget_info_at(i);
		if (!w || w->id == TC_W_NOOP)
			continue;
		if (query[0] != '\0' && !strstr(w->name, query))
			continue;
		if (first == TC_W_NONE)
			first = w->id;
		pos += (size_t)snprintf(out + pos, cap - pos, "\n  %-22s %s",
					w->name, w->desc);
		shown++;
	}
	if (shown == 0)
		(void)snprintf(out + pos, cap - pos, "\n  (no matching widget)");
	return first;
}

/* Open current buffer in $VISUAL or $EDITOR via a temp file. */
static int open_in_external_editor(struct tc_editor *ed, struct tc_shell *sh,
				   tam_tty_t *tty, struct tc_edit_buf *buf)
{
	const char *editor = NULL;
	if (sh) {
		editor = tc_var_get(&sh->vars, "VISUAL");
		if (!editor || !*editor)
			editor = tc_var_get(&sh->vars, "EDITOR");
	}
	if (!editor || !*editor)
		editor = "vi";

	char path[] = "/tmp/tam-clam-edit-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0)
		return -1;
	if (buf->len > 0 && tc_write_all(fd, buf->data, buf->len) < 0) {
		close(fd);
		unlink(path);
		return -1;
	}
	close(fd);

	/* Restore normal terminal mode while the external editor runs. */
	tam_tty_paste(tty, false);
	tam_tty_raw(tty, false);
	tc_editor_restore_orig_termios(ed);

	pid_t pid = fork();
	if (pid == 0) {
		tc_signals_child();
		execlp(editor, editor, path, (char *)NULL);
		_exit(127);
	}
	int status = 0;
	if (pid > 0) {
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
			;
	}

	tc_editor_configure_child_termios(ed);
	(void)tam_tty_raw(tty, true);
	(void)tam_tty_paste(tty, true);

	if (pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		int rfd = open(path, O_RDONLY);
		if (rfd >= 0) {
			char tmp[TC_EDIT_BUF_MAX + 1];
			size_t total = 0;
			for (;;) {
				ssize_t n = read(rfd, tmp + total, TC_EDIT_BUF_MAX - total);
				if (n < 0) {
					if (errno == EINTR)
						continue;
					break;
				}
				if (n == 0)
					break;
				total += (size_t)n;
				if (total >= TC_EDIT_BUF_MAX)
					break;
			}
			close(rfd);
			while (total > 0 && (tmp[total - 1] == '\n' || tmp[total - 1] == '\r'))
				total--;
			(void)tc_ebuf_set(buf, tmp, total);
		}
	}
	unlink(path);
	return 0;
}

/* Bracketed paste stream splitter: intercepts \x1b[200~ and \x1b[201~ before
 * feeding regular bytes to tam_key_parser_t. */
struct paste_filter {
	bool in_paste;
	unsigned char match[6];
	size_t match_len;
};

static const unsigned char PASTE_START[6] = {0x1B, '[', '2', '0', '0', '~'};
static const unsigned char PASTE_END[6]   = {0x1B, '[', '2', '0', '1', '~'};

static void pf_flush_to_key(struct paste_filter *pf, tam_key_parser_t *kp)
{
	if (pf->match_len > 0) {
		(void)tam_key_feed(kp, pf->match, pf->match_len);
		pf->match_len = 0;
	}
}

static void pf_flush_to_buf(struct paste_filter *pf, struct tc_edit_buf *buf)
{
	for (size_t i = 0; i < pf->match_len; i++) {
		char c = (char)pf->match[i];
		if (c == '\r')
			c = '\n';
		(void)tc_ebuf_insert(buf, &c, 1, false);
	}
	pf->match_len = 0;
}

/* Feed one byte through the bracketed paste filter. Returns true if the buffer
 * was modified directly (inside paste mode). */
static bool pf_feed_byte(struct paste_filter *pf, unsigned char b,
			 tam_key_parser_t *kp, struct tc_edit_buf *buf)
{
	const unsigned char *target = pf->in_paste ? PASTE_END : PASTE_START;
	if (b == target[pf->match_len]) {
		pf->match[pf->match_len++] = b;
		if (pf->match_len == 6) {
			pf->match_len = 0;
			pf->in_paste = !pf->in_paste;
		}
		return false;
	}
	bool changed = false;
	if (pf->match_len > 0) {
		if (pf->in_paste) {
			pf_flush_to_buf(pf, buf);
			changed = true;
		} else {
			pf_flush_to_key(pf, kp);
		}
		/* Re-check b against index 0 of target. */
		if (b == target[0]) {
			pf->match[pf->match_len++] = b;
			return changed;
		}
	}
	if (pf->in_paste) {
		char c = (char)b;
		if (c == '\r')
			c = '\n';
		(void)tc_ebuf_insert(buf, &c, 1, false);
		return true;
	}
	(void)tam_key_feed(kp, &b, 1);
	return false;
}

enum overlay_mode {
	OV_NONE = 0,
	OV_GUIDANCE,
	OV_PALETTE,
};

struct edit_ctx {
	struct tc_editor *ed;
	struct tc_shell *sh;
	tam_tty_t *tty;
	struct tc_edit_buf *buf;
	struct tc_render_state *rs;
	enum overlay_mode ov_mode;
	bool verbatim_next;
	bool done;
	enum tc_edit_status status;
};

static void dispatch_widget(struct edit_ctx *ctx, enum tc_widget w)
{
	struct tc_edit_buf *b = ctx->buf;
	struct tc_clip_ring *clip = &ctx->ed->clip;

	switch (w) {
	case TC_W_CHAR_LEFT:
		tc_ebuf_move(b, tc_ebuf_prev_cp(b, b->cursor), b->has_mark);
		break;
	case TC_W_CHAR_RIGHT:
		tc_ebuf_move(b, tc_ebuf_next_cp(b, b->cursor), b->has_mark);
		break;
	case TC_W_WORD_LEFT:
		tc_ebuf_move(b, tc_ebuf_word_left_pos(b, b->cursor), b->has_mark);
		break;
	case TC_W_WORD_RIGHT:
		tc_ebuf_move(b, tc_ebuf_word_right_pos(b, b->cursor), b->has_mark);
		break;
	case TC_W_LINE_START:
		tc_ebuf_move(b, tc_ebuf_line_start_pos(b, b->cursor), b->has_mark);
		break;
	case TC_W_LINE_END:
		tc_ebuf_move(b, tc_ebuf_line_end_pos(b, b->cursor), b->has_mark);
		break;
	case TC_W_BUFFER_START:
		tc_ebuf_move(b, 0, b->has_mark);
		break;
	case TC_W_BUFFER_END:
		tc_ebuf_move(b, b->len, b->has_mark);
		break;
	case TC_W_UP_OR_HIST:
		(void)tc_ebuf_move_line_up(b, b->has_mark);
		break;
	case TC_W_DOWN_OR_HIST:
		(void)tc_ebuf_move_line_down(b, b->has_mark);
		break;
	case TC_W_SEL_CHAR_LEFT:
		tc_ebuf_move(b, tc_ebuf_prev_cp(b, b->cursor), true);
		break;
	case TC_W_SEL_CHAR_RIGHT:
		tc_ebuf_move(b, tc_ebuf_next_cp(b, b->cursor), true);
		break;
	case TC_W_SEL_WORD_LEFT:
		tc_ebuf_move(b, tc_ebuf_word_left_pos(b, b->cursor), true);
		break;
	case TC_W_SEL_WORD_RIGHT:
		tc_ebuf_move(b, tc_ebuf_word_right_pos(b, b->cursor), true);
		break;
	case TC_W_SEL_LINE_START:
		tc_ebuf_move(b, tc_ebuf_line_start_pos(b, b->cursor), true);
		break;
	case TC_W_SEL_LINE_END:
		tc_ebuf_move(b, tc_ebuf_line_end_pos(b, b->cursor), true);
		break;
	case TC_W_SEL_ALL:
		tc_ebuf_select_all(b);
		break;
	case TC_W_TOGGLE_MARK:
		tc_ebuf_toggle_mark(b);
		break;
	case TC_W_COPY: {
		const char *txt = NULL;
		size_t tlen = 0;
		if (tc_ebuf_copy(b, clip, &txt, &tlen) == 0 && txt && tlen > 0)
			(void)tc_render_osc52(ctx->tty, ctx->ed->is_linux_vt, txt, tlen);
		break;
	}
	case TC_W_CUT: {
		const char *txt = NULL;
		size_t tlen = 0;
		if (tc_ebuf_cut(b, clip, &txt, &tlen) == 0 && txt && tlen > 0)
			(void)tc_render_osc52(ctx->tty, ctx->ed->is_linux_vt, txt, tlen);
		break;
	}
	case TC_W_PASTE:
		(void)tc_ebuf_paste(b, clip);
		break;
	case TC_W_PASTE_PREV:
		(void)tc_ebuf_paste_prev(b, clip);
		break;
	case TC_W_UNDO:
		(void)tc_ebuf_undo(b);
		break;
	case TC_W_REDO:
		(void)tc_ebuf_redo(b);
		break;
	case TC_W_DEL_CHAR_LEFT:
		(void)tc_ebuf_delete_char_left(b);
		break;
	case TC_W_DEL_CHAR_RIGHT:
		(void)tc_ebuf_delete_char_right(b);
		break;
	case TC_W_DEL_WORD_LEFT:
		(void)tc_ebuf_delete_word_left(b, clip);
		break;
	case TC_W_DEL_WORD_RIGHT:
		(void)tc_ebuf_delete_word_right(b, clip);
		break;
	case TC_W_KILL_LINE:
		(void)tc_ebuf_kill_line(b, clip);
		break;
	case TC_W_DEL_TO_START:
		(void)tc_ebuf_delete_to_line_start(b, clip);
		break;
	case TC_W_DEL_TO_END:
		(void)tc_ebuf_delete_to_line_end(b, clip);
		break;
	case TC_W_ACCEPT_LINE:
		if (b->len > 0 && buffer_incomplete(b->data, b->len)) {
			(void)tc_ebuf_insert(b, "\n", 1, true);
		} else {
			ctx->done = true;
			ctx->status = TC_EDIT_OK;
		}
		break;
	case TC_W_INSERT_NEWLINE:
		(void)tc_ebuf_insert(b, "\n", 1, true);
		break;
	case TC_W_CLEAR_SCREEN:
		(void)tam_tty_write(ctx->tty, "\x1b[H\x1b[2J", 7);
		tc_render_init(ctx->rs);
		break;
	case TC_W_OPEN_EDITOR:
		(void)open_in_external_editor(ctx->ed, ctx->sh, ctx->tty, b);
		tc_render_init(ctx->rs);
		break;
	case TC_W_COMMENT_LINE:
		(void)tc_ebuf_comment_line(b);
		ctx->done = true;
		ctx->status = TC_EDIT_OK;
		break;
	case TC_W_INSERT_VERBATIM:
		ctx->verbatim_next = true;
		break;
	case TC_W_GUIDANCE:
		ctx->ov_mode = (ctx->ov_mode == OV_GUIDANCE) ? OV_NONE : OV_GUIDANCE;
		break;
	case TC_W_EXECUTE:
		ctx->ov_mode = (ctx->ov_mode == OV_PALETTE) ? OV_NONE : OV_PALETTE;
		break;
	case TC_W_CANCEL_MODE:
		if (ctx->ov_mode != OV_NONE)
			ctx->ov_mode = OV_NONE;
		else
			tc_ebuf_sel_clear(b);
		break;
	case TC_W_QUIT:
		if (b->len == 0) {
			ctx->done = true;
			ctx->status = TC_EDIT_EOF;
		}
		break;
	case TC_W_BREAK:
		tc_ebuf_clear(b);
		ctx->done = true;
		ctx->status = TC_EDIT_CANCEL;
		break;
	default:
		/* Future 0.2.1/0.2.3 widgets and TC_W_NOOP are clean no-ops. */
		break;
	}
}

enum tc_edit_status tc_editor_readline(struct tc_editor *ed,
				       struct tc_shell *sh,
				       const char *ps1, const char *ps2,
				       char **out_line, size_t *out_len)
{
	*out_line = NULL;
	*out_len = 0;

	tam_tty_t tty;
	if (tam_tty_open(&tty, ed->in_fd, ed->out_fd) < 0)
		return TC_EDIT_ERR;
	if (tam_tty_raw(&tty, true) < 0) {
		tam_tty_close(&tty);
		return TC_EDIT_ERR;
	}
	(void)tam_tty_paste(&tty, true);

	struct tc_edit_buf buf;
	tc_ebuf_init(&buf);
	struct tc_render_state rs;
	tc_render_init(&rs);
	tam_key_parser_t kp;
	tam_key_init(&kp);
	struct paste_filter pf;
	memset(&pf, 0, sizeof(pf));

	struct edit_ctx ctx = {
		.ed = ed,
		.sh = sh,
		.tty = &tty,
		.buf = &buf,
		.rs = &rs,
		.ov_mode = OV_NONE,
		.verbatim_next = false,
		.done = false,
		.status = TC_EDIT_OK,
	};

	char pal_query[64] = {0};
	size_t pal_len = 0;
	char pal_overlay[1024] = {0};
	tam_key_event_t chord[TC_EDIT_CHORD_MAX];
	size_t chord_len = 0;

	const char *ov = NULL;
	(void)tc_render_draw(&rs, &tty, &buf, ps1, ps2, ov);

	while (!ctx.done) {
		if (tc_sigwinch_seen) {
			tc_sigwinch_seen = 0;
			(void)tam_tty_size(&tty, NULL, NULL);
			(void)tc_render_draw(&rs, &tty, &buf, ps1, ps2, ov);
		}
		if (tc_sigint_seen) {
			tc_sigint_seen = 0;
			tc_ebuf_clear(&buf);
			ctx.done = true;
			ctx.status = TC_EDIT_CANCEL;
			break;
		}

		/* Zero-CPU idle wait (-1) unless disambiguating a pending ESC. */
		int timeout_ms = (tam_key_pending(&kp) > 0 || pf.match_len > 0) ? 50 : -1;
		unsigned char raw[64];
		int n = tam_tty_read(&tty, raw, sizeof(raw), timeout_ms);
		if (n < 0) {
			ctx.done = true;
			ctx.status = TC_EDIT_EOF;
			break;
		}
		bool dirty = false;
		if (n == 0) {
			if (tc_sigwinch_seen || tc_sigint_seen)
				continue;
			if (pf.match_len > 0) {
				if (pf.in_paste) {
					pf_flush_to_buf(&pf, &buf);
					dirty = true;
				} else {
					pf_flush_to_key(&pf, &kp);
				}
			}
			tam_key_event_t tev;
			if (tam_key_timeout(&kp, &tev) == 1) {
				if (ctx.ov_mode != OV_NONE) {
					ctx.ov_mode = OV_NONE;
					pal_len = 0;
					pal_query[0] = '\0';
				} else {
					tc_ebuf_sel_clear(&buf);
				}
				chord_len = 0;
				dirty = true;
			}
		} else {
			for (int i = 0; i < n; i++) {
				if (pf_feed_byte(&pf, raw[i], &kp, &buf))
					dirty = true;
			}
		}

		tam_key_event_t ev;
		while (!ctx.done && tam_key_next(&kp, &ev) == 1) {
			dirty = true;

			if (ctx.verbatim_next) {
				ctx.verbatim_next = false;
				if (ev.type == TAM_KEY_CHAR && ev.mods == 0) {
					(void)tc_ebuf_insert_cp(&buf, ev.codepoint, true);
				} else if (ev.type == TAM_KEY_TAB) {
					(void)tc_ebuf_insert(&buf, "\t", 1, true);
				} else if (ev.type == TAM_KEY_ENTER) {
					(void)tc_ebuf_insert(&buf, "\n", 1, true);
				} else if (ev.type == TAM_KEY_ESC) {
					(void)tc_ebuf_insert(&buf, "\x1b", 1, true);
				}
				continue;
			}

			if (ctx.ov_mode == OV_PALETTE) {
				if (ev.type == TAM_KEY_ESC) {
					ctx.ov_mode = OV_NONE;
					pal_len = 0;
					pal_query[0] = '\0';
				} else if (ev.type == TAM_KEY_ENTER) {
					enum tc_widget chosen =
						build_palette_overlay(pal_query, pal_overlay,
								      sizeof(pal_overlay));
					ctx.ov_mode = OV_NONE;
					pal_len = 0;
					pal_query[0] = '\0';
					if (chosen != TC_W_NONE && chosen != TC_W_EXECUTE)
						dispatch_widget(&ctx, chosen);
				} else if (ev.type == TAM_KEY_BACKSPACE) {
					if (pal_len > 0)
						pal_query[--pal_len] = '\0';
				} else if (ev.type == TAM_KEY_CHAR && ev.mods == 0 &&
					   ev.codepoint >= 0x20 && ev.codepoint < 0x7F) {
					if (pal_len + 1 < sizeof(pal_query)) {
						pal_query[pal_len++] = (char)ev.codepoint;
						pal_query[pal_len] = '\0';
					}
				}
				continue;
			}

			/* Plain printable characters insert directly when not in a chord. */
			if (chord_len == 0 && ev.type == TAM_KEY_CHAR && ev.mods == 0) {
				if (ctx.ov_mode == OV_GUIDANCE)
					ctx.ov_mode = OV_NONE;
				(void)tc_ebuf_insert_cp(&buf, ev.codepoint, true);
				continue;
			}

			if (chord_len < TC_EDIT_CHORD_MAX)
				chord[chord_len++] = ev;
			enum tc_widget w = TC_W_NONE;
			enum tc_bind_match m =
				tc_keymap_lookup(&ed->keymap, chord, chord_len, &w);
			if (m == TC_MATCH_EXACT) {
				chord_len = 0;
				if (ctx.ov_mode == OV_GUIDANCE && w != TC_W_GUIDANCE)
					ctx.ov_mode = OV_NONE;
				dispatch_widget(&ctx, w);
			} else if (m == TC_MATCH_NONE) {
				chord_len = 0;
			}
		}

		if (ctx.ov_mode == OV_GUIDANCE) {
			ov = guidance_text;
		} else if (ctx.ov_mode == OV_PALETTE) {
			(void)build_palette_overlay(pal_query, pal_overlay, sizeof(pal_overlay));
			ov = pal_overlay;
		} else {
			ov = NULL;
		}

		if (dirty && !ctx.done)
			(void)tc_render_draw(&rs, &tty, &buf, ps1, ps2, ov);
	}

	/* Commit final visual line (no overlay, no selection) before leaving raw mode. */
	if (ctx.status == TC_EDIT_OK) {
		(void)tc_render_finish(&rs, &tty, &buf, ps1, ps2);
		char *copy = malloc(buf.len + 1);
		if (!copy) {
			ctx.status = TC_EDIT_ERR;
		} else {
			if (buf.len > 0)
				memcpy(copy, buf.data, buf.len);
			copy[buf.len] = '\0';
			*out_line = copy;
			*out_len = buf.len;
		}
	} else {
		(void)tc_render_finish(&rs, &tty, &buf, ps1, ps2);
	}

	tc_ebuf_finish(&buf);
	tam_tty_close(&tty);
	return ctx.status;
}

int tc_bi_bind(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	struct tc_keymap local_km;
	struct tc_keymap *km = sh->editor ? &sh->editor->keymap : &local_km;
	if (!sh->editor)
		tc_keymap_init_defaults(&local_km);

	if (argc == 1) {
		for (size_t i = 0; i < km->count; i++) {
			char keybuf[128];
			if (tc_key_seq_format(&km->binds[i].seq, keybuf, sizeof(keybuf)) < 0)
				continue;
			const struct tc_widget_info *w = tc_widget_info_by_id(km->binds[i].widget);
			char out[256];
			int n = snprintf(out, sizeof(out), "bind %s %s\n",
					 keybuf, w ? w->name : "noop");
			if (n > 0 && tc_write_all(STDOUT_FILENO, out, (size_t)n) < 0)
				return 1;
		}
		return 0;
	}

	if (argc == 3 && strcmp(argv[1], "-u") == 0) {
		struct tc_key_seq seq;
		if (tc_key_seq_parse(argv[2], &seq) < 0) {
			tc_error(sh, line, "bind: invalid key sequence: %s", argv[2]);
			return TC_STATUS_USAGE;
		}
		(void)tc_keymap_unbind(km, &seq);
		return 0;
	}

	if (argc == 3) {
		struct tc_key_seq seq;
		if (tc_key_seq_parse(argv[1], &seq) < 0) {
			tc_error(sh, line, "bind: invalid key sequence: %s", argv[1]);
			return TC_STATUS_USAGE;
		}
		enum tc_widget w = tc_widget_by_name(argv[2]);
		if (w == TC_W_NONE) {
			tc_error(sh, line, "bind: unknown widget: %s", argv[2]);
			return TC_STATUS_USAGE;
		}
		if (tc_keymap_bind(km, &seq, w) < 0) {
			tc_error(sh, line, "bind: keymap full");
			return 1;
		}
		return 0;
	}

	tc_error(sh, line, "usage: bind [KEY WIDGET | -u KEY]");
	return TC_STATUS_USAGE;
}
