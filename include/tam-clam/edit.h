/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Interactive line editor built on libtam (tam_tty, tam_key, tam_uni).
 * Follows arch/keys.md (and tamlinux/arch/keybindings.md). */
#ifndef TAM_CLAM_EDIT_H
#define TAM_CLAM_EDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <termios.h>

#include "tam_key.h"
#include "tam_tty.h"

struct tc_shell;

#define TC_EDIT_BUF_MAX    65536
#define TC_EDIT_UNDO_MAX   128
#define TC_EDIT_CLIP_MAX   16
#define TC_EDIT_CHORD_MAX  4
#define TC_EDIT_BINDS_MAX  192

enum tc_edit_status {
	TC_EDIT_OK = 0,
	TC_EDIT_EOF,
	TC_EDIT_CANCEL,
	TC_EDIT_ERR,
};

/* Canonical widget IDs matching arch/keys.md. */
enum tc_widget {
	TC_W_NONE = 0,
	TC_W_CHAR_LEFT,
	TC_W_CHAR_RIGHT,
	TC_W_WORD_LEFT,
	TC_W_WORD_RIGHT,
	TC_W_LINE_START,
	TC_W_LINE_END,
	TC_W_BUFFER_START,
	TC_W_BUFFER_END,
	TC_W_UP_OR_HIST,
	TC_W_DOWN_OR_HIST,
	TC_W_SEL_CHAR_LEFT,
	TC_W_SEL_CHAR_RIGHT,
	TC_W_SEL_WORD_LEFT,
	TC_W_SEL_WORD_RIGHT,
	TC_W_SEL_LINE_START,
	TC_W_SEL_LINE_END,
	TC_W_SEL_ALL,
	TC_W_TOGGLE_MARK,
	TC_W_CUT,
	TC_W_COPY,
	TC_W_PASTE,
	TC_W_PASTE_PREV,
	TC_W_UNDO,
	TC_W_REDO,
	TC_W_DEL_CHAR_LEFT,
	TC_W_DEL_CHAR_RIGHT,
	TC_W_DEL_WORD_LEFT,
	TC_W_DEL_WORD_RIGHT,
	TC_W_KILL_LINE,
	TC_W_DEL_TO_START,
	TC_W_DEL_TO_END,
	TC_W_ACCEPT_LINE,
	TC_W_INSERT_NEWLINE,
	TC_W_CLEAR_SCREEN,
	TC_W_OPEN_EDITOR,
	TC_W_COMMENT_LINE,
	TC_W_INSERT_VERBATIM,
	TC_W_EXECUTE,
	TC_W_GUIDANCE,
	TC_W_CANCEL_MODE,
	TC_W_QUIT,
	TC_W_BREAK,
	TC_W_FIND,
	TC_W_FIND_NEXT,
	TC_W_FIND_PREV,
	TC_W_INSERT_LAST_ARG,
	TC_W_COMPLETE_NEXT,
	TC_W_COMPLETE_PREV,
	TC_W_NOOP,
	TC_W_COUNT
};

struct tc_widget_info {
	enum tc_widget id;
	const char *name;
	const char *desc;
};

/* Pure UTF-8 editable buffer with selection and undo/redo snapshots. */
struct tc_edit_snap {
	char *data;
	size_t len;
	size_t cursor;
	bool has_mark;
	size_t mark;
};

struct tc_edit_buf {
	char *data;
	size_t len;
	size_t cap;
	size_t cursor;
	bool has_mark;
	size_t mark;
	struct tc_edit_snap undo_stack[TC_EDIT_UNDO_MAX];
	size_t undo_len;
	struct tc_edit_snap redo_stack[TC_EDIT_UNDO_MAX];
	size_t redo_len;
	/* Last pasted span for Alt+V (paste-prev) cycling. */
	bool last_was_paste;
	size_t paste_start;
	size_t paste_end;
	size_t paste_ring_idx;
};

/* Persistent clipboard/kill ring across prompts. */
struct tc_clip_ring {
	char *items[TC_EDIT_CLIP_MAX];
	size_t lens[TC_EDIT_CLIP_MAX];
	size_t count;
	size_t head; /* index of most recent entry when count > 0 */
};

/* Key chord sequence + widget binding. */
struct tc_key_seq {
	tam_key_event_t keys[TC_EDIT_CHORD_MAX];
	size_t len;
};

struct tc_key_bind {
	struct tc_key_seq seq;
	enum tc_widget widget;
};

struct tc_keymap {
	struct tc_key_bind binds[TC_EDIT_BINDS_MAX];
	size_t count;
};

enum tc_bind_match {
	TC_MATCH_NONE = 0,
	TC_MATCH_PREFIX,
	TC_MATCH_EXACT,
};

/* Persistent interactive line editor state owned by main/shell. */
struct tc_editor {
	int in_fd;
	int out_fd;
	bool is_linux_vt;
	bool has_orig_termios;
	struct termios orig_termios;
	struct tc_keymap keymap;
	struct tc_clip_ring clip;
	char *last_cmd;       /* raw interactive command text for history */
	size_t last_cmd_len;
};

void tc_editor_append_last_cmd(struct tc_editor *ed, const char *line, size_t len);
void tc_editor_clear_last_cmd(struct tc_editor *ed);

/* --- Buffer API (src/edit/buf.c) --- */
void tc_ebuf_init(struct tc_edit_buf *b);
void tc_ebuf_finish(struct tc_edit_buf *b);
void tc_ebuf_clear(struct tc_edit_buf *b);
int tc_ebuf_set(struct tc_edit_buf *b, const char *s, size_t len);

size_t tc_ebuf_prev_cp(const struct tc_edit_buf *b, size_t pos);
size_t tc_ebuf_next_cp(const struct tc_edit_buf *b, size_t pos);
size_t tc_ebuf_word_left_pos(const struct tc_edit_buf *b, size_t pos);
size_t tc_ebuf_word_right_pos(const struct tc_edit_buf *b, size_t pos);
size_t tc_ebuf_line_start_pos(const struct tc_edit_buf *b, size_t pos);
size_t tc_ebuf_line_end_pos(const struct tc_edit_buf *b, size_t pos);

bool tc_ebuf_sel_range(const struct tc_edit_buf *b, size_t *start, size_t *end);
void tc_ebuf_sel_clear(struct tc_edit_buf *b);
void tc_ebuf_sel_begin(struct tc_edit_buf *b);
void tc_ebuf_select_all(struct tc_edit_buf *b);
void tc_ebuf_toggle_mark(struct tc_edit_buf *b);

void tc_ebuf_move(struct tc_edit_buf *b, size_t new_pos, bool extend_sel);
bool tc_ebuf_move_line_up(struct tc_edit_buf *b, bool extend_sel);
bool tc_ebuf_move_line_down(struct tc_edit_buf *b, bool extend_sel);

int tc_ebuf_insert(struct tc_edit_buf *b, const char *s, size_t len, bool record_undo);
int tc_ebuf_insert_cp(struct tc_edit_buf *b, uint32_t cp, bool record_undo);
int tc_ebuf_delete_range(struct tc_edit_buf *b, size_t start, size_t end, bool record_undo);
int tc_ebuf_delete_char_left(struct tc_edit_buf *b);
int tc_ebuf_delete_char_right(struct tc_edit_buf *b);
int tc_ebuf_delete_word_left(struct tc_edit_buf *b, struct tc_clip_ring *clip);
int tc_ebuf_delete_word_right(struct tc_edit_buf *b, struct tc_clip_ring *clip);
int tc_ebuf_delete_to_line_start(struct tc_edit_buf *b, struct tc_clip_ring *clip);
int tc_ebuf_delete_to_line_end(struct tc_edit_buf *b, struct tc_clip_ring *clip);
int tc_ebuf_kill_line(struct tc_edit_buf *b, struct tc_clip_ring *clip);
int tc_ebuf_comment_line(struct tc_edit_buf *b);

int tc_ebuf_undo(struct tc_edit_buf *b);
int tc_ebuf_redo(struct tc_edit_buf *b);

void tc_clip_init(struct tc_clip_ring *r);
void tc_clip_finish(struct tc_clip_ring *r);
int tc_clip_push(struct tc_clip_ring *r, const char *s, size_t len);
const char *tc_clip_get(const struct tc_clip_ring *r, size_t age, size_t *out_len);

int tc_ebuf_copy(struct tc_edit_buf *b, struct tc_clip_ring *clip,
		 const char **copied, size_t *copied_len);
int tc_ebuf_cut(struct tc_edit_buf *b, struct tc_clip_ring *clip,
		const char **cut_text, size_t *cut_len);
int tc_ebuf_paste(struct tc_edit_buf *b, const struct tc_clip_ring *clip);
int tc_ebuf_paste_prev(struct tc_edit_buf *b, const struct tc_clip_ring *clip);

/* --- Keymap & widget API (src/edit/keymap.c) --- */
const struct tc_widget_info *tc_widget_info_by_id(enum tc_widget id);
enum tc_widget tc_widget_by_name(const char *name);
size_t tc_widget_count(void);
const struct tc_widget_info *tc_widget_info_at(size_t idx);

int tc_key_seq_parse(const char *spec, struct tc_key_seq *out);
int tc_key_seq_format(const struct tc_key_seq *seq, char *buf, size_t cap);
void tc_keymap_init_defaults(struct tc_keymap *km);
int tc_keymap_bind(struct tc_keymap *km, const struct tc_key_seq *seq, enum tc_widget w);
bool tc_keymap_unbind(struct tc_keymap *km, const struct tc_key_seq *seq);
enum tc_bind_match tc_keymap_lookup(const struct tc_keymap *km,
				    const tam_key_event_t *keys, size_t len,
				    enum tc_widget *out_widget);

/* --- Renderer API (src/edit/render.c) --- */
struct tc_render_state {
	int prev_rows;
	int prev_cursor_row;
	bool active;
};

void tc_render_init(struct tc_render_state *rs);
int tc_render_draw(struct tc_render_state *rs, tam_tty_t *tty,
		   const struct tc_edit_buf *buf,
		   const char *ps1, const char *ps2,
		   const char *rps1,
		   const unsigned char *styles,
		   const char *ghost,
		   const char *overlay);
int tc_render_finish(struct tc_render_state *rs, tam_tty_t *tty,
		     const struct tc_edit_buf *buf,
		     const char *ps1, const char *ps2);
int tc_render_osc52(tam_tty_t *tty, bool is_linux_vt,
		    const char *data, size_t len);

/* --- Editor lifecycle & readline API (src/edit/edit.c) --- */
int tc_editor_init(struct tc_editor *ed, int in_fd, int out_fd);
void tc_editor_finish(struct tc_editor *ed);
void tc_editor_configure_child_termios(struct tc_editor *ed);
void tc_editor_restore_orig_termios(struct tc_editor *ed);

/* Read one command (possibly multi-line). On TC_EDIT_OK, *out_line is
 * heap-allocated (caller frees) without a trailing newline. */
enum tc_edit_status tc_editor_readline(struct tc_editor *ed,
				       struct tc_shell *sh,
				       const char *ps1, const char *ps2,
				       const char *rps1,
				       char **out_line, size_t *out_len);

/* Builtin: bind [KEY WIDGET | -u KEY] (src/edit/edit.c). */
int tc_bi_bind(struct tc_shell *sh, int argc, char **argv, unsigned long line);

#endif
