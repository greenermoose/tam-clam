/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for the 0.2.0 line editor: UTF-8 buffer, selection, clipboard
 * ring, undo/redo, and keymap/chord parsing (src/edit/buf.c, keymap.c). */
#include <locale.h>
#include <string.h>

#include "check.h"
#include "tam-clam/edit.h"

static void test_utf8_and_movement(void)
{
	struct tc_edit_buf b;
	tc_ebuf_init(&b);

	/* ASCII + multi-byte UTF-8 + wide CJK characters. */
	const char *s = "echo 日本語 café";
	CHECK(tc_ebuf_insert(&b, s, strlen(s), true) == 0);
	CHECK(b.len == strlen(s));
	CHECK(b.cursor == b.len);

	/* Step back over 'é' (2 bytes), then delete it. */
	size_t p = tc_ebuf_prev_cp(&b, b.cursor);
	CHECK(b.cursor - p == 2);
	CHECK(tc_ebuf_delete_char_left(&b) == 0);
	CHECK(strcmp(b.data, "echo 日本語 caf") == 0);

	/* Word movement. */
	size_t w1 = tc_ebuf_word_left_pos(&b, b.cursor);
	CHECK(strcmp(b.data + w1, "caf") == 0);
	size_t w2 = tc_ebuf_word_left_pos(&b, w1);
	CHECK(strcmp(b.data + w2, "日本語 caf") == 0);
	size_t w3 = tc_ebuf_word_right_pos(&b, 0);
	CHECK(w3 == 4);

	/* Line start/end on multi-line buffer. */
	CHECK(tc_ebuf_set(&b, "line1\nline2\nline3", 17) == 0);
	tc_ebuf_move(&b, 8, false); /* inside "line2" */
	CHECK(tc_ebuf_line_start_pos(&b, b.cursor) == 6);
	CHECK(tc_ebuf_line_end_pos(&b, b.cursor) == 11);
	CHECK(tc_ebuf_move_line_up(&b, false));
	CHECK(b.cursor == 2);
	CHECK(!tc_ebuf_move_line_up(&b, false));
	CHECK(tc_ebuf_move_line_down(&b, false));
	CHECK(b.cursor == 8);
	CHECK(tc_ebuf_move_line_down(&b, false));
	CHECK(b.cursor == 14);
	CHECK(!tc_ebuf_move_line_down(&b, false));

	tc_ebuf_finish(&b);
}

static void test_selection_clipboard_undo(void)
{
	struct tc_edit_buf b;
	struct tc_clip_ring clip;
	tc_ebuf_init(&b);
	tc_clip_init(&clip);

	CHECK(tc_ebuf_insert(&b, "alpha beta gamma", 16, true) == 0);

	/* Select "gamma" with Shift-style movement and copy. */
	tc_ebuf_move(&b, tc_ebuf_word_left_pos(&b, b.cursor), true);
	size_t s0 = 0, s1 = 0;
	CHECK(tc_ebuf_sel_range(&b, &s0, &s1));
	CHECK(s0 == 11 && s1 == 16);

	const char *copied = NULL;
	size_t clen = 0;
	CHECK(tc_ebuf_copy(&b, &clip, &copied, &clen) == 0);
	CHECK(clen == 5 && memcmp(copied, "gamma", 5) == 0);

	/* Cut "gamma", then undo and redo. */
	CHECK(tc_ebuf_cut(&b, &clip, &copied, &clen) == 0);
	CHECK(strcmp(b.data, "alpha beta ") == 0);
	CHECK(tc_ebuf_undo(&b) == 1);
	CHECK(strcmp(b.data, "alpha beta gamma") == 0);
	CHECK(tc_ebuf_redo(&b) == 1);
	CHECK(strcmp(b.data, "alpha beta ") == 0);

	/* Push a second clip entry and test paste + paste-prev cycling. */
	CHECK(tc_clip_push(&clip, "FIRST", 5) == 0);
	CHECK(tc_clip_push(&clip, "SECOND", 6) == 0);
	CHECK(tc_ebuf_paste(&b, &clip) == 0);
	CHECK(strcmp(b.data, "alpha beta SECOND") == 0);
	CHECK(tc_ebuf_paste_prev(&b, &clip) == 0);
	CHECK(strcmp(b.data, "alpha beta FIRST") == 0);

	/* Select all + replace by typing. */
	tc_ebuf_select_all(&b);
	CHECK(tc_ebuf_insert(&b, "fresh", 5, true) == 0);
	CHECK(strcmp(b.data, "fresh") == 0);

	/* Comment multi-line buffer. */
	CHECK(tc_ebuf_set(&b, "a\nb", 3) == 0);
	CHECK(tc_ebuf_comment_line(&b) == 0);
	CHECK(strcmp(b.data, "# a\n# b") == 0);

	/* Toggle mark (TERM=linux Alt+M). */
	CHECK(tc_ebuf_set(&b, "hello", 5) == 0);
	tc_ebuf_move(&b, 1, false);
	tc_ebuf_toggle_mark(&b);
	tc_ebuf_move(&b, 4, b.has_mark);
	CHECK(tc_ebuf_sel_range(&b, &s0, &s1) && s0 == 1 && s1 == 4);
	tc_ebuf_toggle_mark(&b);
	CHECK(!tc_ebuf_sel_range(&b, &s0, &s1));

	tc_clip_finish(&clip);
	tc_ebuf_finish(&b);
}

static void test_keymap_and_chords(void)
{
	struct tc_keymap km;
	tc_keymap_init_defaults(&km);

	/* Verify core keys/keybindings.md bindings. */
	struct tc_key_seq seq;
	enum tc_widget w = TC_W_NONE;

	CHECK(tc_key_seq_parse("Ctrl+Z", &seq) == 0);
	CHECK(tc_keymap_lookup(&km, seq.keys, seq.len, &w) == TC_MATCH_EXACT);
	CHECK(w == TC_W_UNDO);

	CHECK(tc_key_seq_parse("Ctrl+Y", &seq) == 0);
	CHECK(tc_keymap_lookup(&km, seq.keys, seq.len, &w) == TC_MATCH_EXACT);
	CHECK(w == TC_W_REDO);

	CHECK(tc_key_seq_parse("Ctrl+X", &seq) == 0);
	CHECK(tc_keymap_lookup(&km, seq.keys, seq.len, &w) == TC_MATCH_EXACT);
	CHECK(w == TC_W_CUT);

	CHECK(tc_key_seq_parse("Ctrl+C", &seq) == 0);
	CHECK(tc_keymap_lookup(&km, seq.keys, seq.len, &w) == TC_MATCH_EXACT);
	CHECK(w == TC_W_COPY);

	CHECK(tc_key_seq_parse("Ctrl+V", &seq) == 0);
	CHECK(tc_keymap_lookup(&km, seq.keys, seq.len, &w) == TC_MATCH_EXACT);
	CHECK(w == TC_W_PASTE);

	CHECK(tc_key_seq_parse("Ctrl+G", &seq) == 0);
	CHECK(tc_keymap_lookup(&km, seq.keys, seq.len, &w) == TC_MATCH_EXACT);
	CHECK(w == TC_W_GUIDANCE);

	CHECK(tc_key_seq_parse("Ctrl+\\", &seq) == 0);
	CHECK(seq.keys[0].codepoint == '|' && (seq.keys[0].mods & TAM_MOD_CTRL));
	CHECK(tc_keymap_lookup(&km, seq.keys, seq.len, &w) == TC_MATCH_EXACT);
	CHECK(w == TC_W_BREAK);

	char fmt[64];
	CHECK(tc_key_seq_format(&seq, fmt, sizeof(fmt)) > 0);
	CHECK(strcmp(fmt, "Ctrl+\\") == 0);

	/* TERM=linux fallbacks. */
	CHECK(tc_key_seq_parse("Alt+,", &seq) == 0);
	CHECK(tc_keymap_lookup(&km, seq.keys, seq.len, &w) == TC_MATCH_EXACT);
	CHECK(w == TC_W_WORD_LEFT);

	CHECK(tc_key_seq_parse("Alt+M", &seq) == 0);
	CHECK(tc_keymap_lookup(&km, seq.keys, seq.len, &w) == TC_MATCH_EXACT);
	CHECK(w == TC_W_TOGGLE_MARK);

	/* Multi-key chord binding (e.g., optional readline-style Ctrl+X Ctrl+E). */
	CHECK(tc_key_seq_parse("Ctrl+B Ctrl+E", &seq) == 0);
	CHECK(seq.len == 2);
	CHECK(tc_keymap_bind(&km, &seq, TC_W_OPEN_EDITOR) == 0);
	CHECK(tc_keymap_lookup(&km, seq.keys, 1, &w) == TC_MATCH_EXACT); /* Ctrl+B noop */
	CHECK(tc_keymap_unbind(&km, &(struct tc_key_seq){.keys = {seq.keys[0]}, .len = 1}));
	CHECK(tc_keymap_lookup(&km, seq.keys, 1, &w) == TC_MATCH_PREFIX);
	CHECK(tc_keymap_lookup(&km, seq.keys, 2, &w) == TC_MATCH_EXACT);
	CHECK(w == TC_W_OPEN_EDITOR);

	/* Format round-trip for multi-key chord. */
	CHECK(tc_key_seq_format(&seq, fmt, sizeof(fmt)) > 0);
	CHECK(strcmp(fmt, "Ctrl+B Ctrl+E") == 0);

	/* Meta is rejected for shell bindings. */
	CHECK(tc_key_seq_parse("Meta+K", &seq) < 0);

	/* One-handed chord composer widget & reverse key lookup. */
	CHECK(tc_widget_by_name("chord-compose") == TC_W_CHORD_COMPOSE);
	CHECK(tc_keymap_first_binding(&km, TC_W_CUT, fmt, sizeof(fmt)) > 0);
	CHECK(strcmp(fmt, "Ctrl+X") == 0);
}

int main(void)
{
	(void)setlocale(LC_CTYPE, "");
	test_utf8_and_movement();
	test_selection_clipboard_undo();
	test_keymap_and_chords();
	CHECK_DONE("test_edit");
}
