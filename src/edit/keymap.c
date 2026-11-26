/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Widget registry, physical key chord parser/formatter, and default keymap
 * implementing keys/keybindings.md (following tam-idol/keys/keybindings.md). */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "tam-clam/edit.h"

static const struct tc_widget_info widgets[] = {
	{TC_W_CHAR_LEFT,       "char-left",               "Move one character left"},
	{TC_W_CHAR_RIGHT,      "char-right",              "Move one character right"},
	{TC_W_WORD_LEFT,       "word-left",               "Move one word left"},
	{TC_W_WORD_RIGHT,      "word-right",              "Move one word right"},
	{TC_W_LINE_START,      "line-start",              "Move to start of line"},
	{TC_W_LINE_END,        "line-end",                "Move to end of line"},
	{TC_W_BUFFER_START,    "buffer-start",            "Move to start of buffer"},
	{TC_W_BUFFER_END,      "buffer-end",              "Move to end of buffer"},
	{TC_W_UP_OR_HIST,      "up-or-history-prev",      "Move up a line (or history)"},
	{TC_W_DOWN_OR_HIST,    "down-or-history-next",    "Move down a line (or history)"},
	{TC_W_SEL_CHAR_LEFT,   "select-char-left",        "Extend selection left"},
	{TC_W_SEL_CHAR_RIGHT,  "select-char-right",       "Extend selection right"},
	{TC_W_SEL_WORD_LEFT,   "select-word-left",        "Extend selection one word left"},
	{TC_W_SEL_WORD_RIGHT,  "select-word-right",       "Extend selection one word right"},
	{TC_W_SEL_LINE_START,  "select-line-start",       "Extend selection to line start"},
	{TC_W_SEL_LINE_END,    "select-line-end",         "Extend selection to line end"},
	{TC_W_SEL_ALL,         "select-all",              "Select entire command buffer"},
	{TC_W_TOGGLE_MARK,     "toggle-mark",             "Toggle keyboard selection mark"},
	{TC_W_CUT,             "cut",                     "Cut selection (or line) to clipboard"},
	{TC_W_COPY,            "copy",                    "Copy selection (or line) to clipboard"},
	{TC_W_PASTE,           "paste",                   "Paste from clipboard"},
	{TC_W_PASTE_PREV,      "paste-prev",              "Cycle older clipboard entry"},
	{TC_W_UNDO,            "undo",                    "Undo last edit"},
	{TC_W_REDO,            "redo",                    "Redo last undone edit"},
	{TC_W_DEL_CHAR_LEFT,   "delete-char-left",        "Delete character before cursor"},
	{TC_W_DEL_CHAR_RIGHT,  "delete-char-right",       "Delete character after cursor"},
	{TC_W_DEL_WORD_LEFT,   "delete-word-left",        "Delete word before cursor"},
	{TC_W_DEL_WORD_RIGHT,  "delete-word-right",       "Delete word after cursor"},
	{TC_W_KILL_LINE,       "kill-line",               "Clear current prompt buffer"},
	{TC_W_DEL_TO_START,    "delete-to-line-start",    "Delete to start of line"},
	{TC_W_DEL_TO_END,      "delete-to-line-end",      "Delete to end of line"},
	{TC_W_ACCEPT_LINE,     "accept-line",             "Submit command (or continue line)"},
	{TC_W_INSERT_NEWLINE,  "insert-newline",          "Insert literal newline"},
	{TC_W_CLEAR_SCREEN,    "clear-screen",            "Clear screen and redraw prompt"},
	{TC_W_OPEN_EDITOR,     "open-in-editor",          "Edit buffer in $VISUAL/$EDITOR"},
	{TC_W_COMMENT_LINE,    "comment-line",            "Comment out buffer and submit"},
	{TC_W_INSERT_VERBATIM, "insert-verbatim",         "Insert next key literally"},
	{TC_W_EXECUTE,         "execute",                 "Open named-widget command palette"},
	{TC_W_CHORD_COMPOSE,   "chord-compose",           "Open one-handed chord composer & command menu"},
	{TC_W_GUIDANCE,        "guidance",                "Show keybinding guidance"},
	{TC_W_CANCEL_MODE,     "cancel-mode",             "Dismiss overlay or selection"},
	{TC_W_QUIT,            "quit",                    "Exit tam-clam on empty prompt"},
	{TC_W_BREAK,           "break",                   "Cancel current buffer"},
	{TC_W_FIND,            "find",                    "Incremental history search (0.2.1)"},
	{TC_W_FIND_NEXT,       "find-next",               "Next history match (0.2.1)"},
	{TC_W_FIND_PREV,       "find-prev",               "Previous history match (0.2.1)"},
	{TC_W_INSERT_LAST_ARG, "insert-last-arg",         "Insert last arg of prev cmd (0.2.1)"},
	{TC_W_COMPLETE_NEXT,   "complete-next",           "Complete / cycle next (0.2.3)"},
	{TC_W_COMPLETE_PREV,   "complete-prev",           "Cycle completion prev (0.2.3)"},
	{TC_W_NOOP,            "noop",                    "Do nothing"},
};

size_t tc_widget_count(void)
{
	return sizeof(widgets) / sizeof(widgets[0]);
}

const struct tc_widget_info *tc_widget_info_at(size_t idx)
{
	if (idx >= tc_widget_count())
		return NULL;
	return &widgets[idx];
}

const struct tc_widget_info *tc_widget_info_by_id(enum tc_widget id)
{
	for (size_t i = 0; i < tc_widget_count(); i++) {
		if (widgets[i].id == id)
			return &widgets[i];
	}
	return NULL;
}

enum tc_widget tc_widget_by_name(const char *name)
{
	if (!name)
		return TC_W_NONE;
	for (size_t i = 0; i < tc_widget_count(); i++) {
		if (strcmp(widgets[i].name, name) == 0)
			return widgets[i].id;
	}
	return TC_W_NONE;
}

/* Parse a single chord like "Ctrl+Shift+Left", "Alt+#", "Ctrl+\\" with
 * libtam's key names. One rule is the shell's own: Ctrl+Shift+letter is
 * Ctrl+letter, because that is all a terminal sends for it. */
static int parse_one_key(const char *s, size_t len, tam_key_event_t *ev)
{
	if (len == 0 || len >= 64)
		return -1;
	char name[64];
	memcpy(name, s, len);
	name[len] = '\0';
	if (!tam_key_name_parse(name, ev))
		return -1;
	if (ev->type == TAM_KEY_CHAR && (ev->mods & TAM_MOD_CTRL) &&
	    (ev->mods & TAM_MOD_SHIFT) && ev->codepoint >= 'a' && ev->codepoint <= 'z')
		ev->mods &= ~(unsigned)TAM_MOD_SHIFT;
	return 0;
}

int tc_key_seq_parse(const char *spec, struct tc_key_seq *out)
{
	if (!spec || !out)
		return -1;
	memset(out, 0, sizeof(*out));
	const char *p = spec;
	while (*p) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		const char *start = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		if (out->len >= TC_EDIT_CHORD_MAX)
			return -1;
		if (parse_one_key(start, (size_t)(p - start), &out->keys[out->len]) < 0)
			return -1;
		out->len++;
	}
	return out->len > 0 ? 0 : -1;
}

static int format_one_key(const tam_key_event_t *ev, char *buf, size_t cap)
{
	size_t n = tam_key_name_format(ev, buf, cap);
	return n < cap ? (int)n : -1;
}

int tc_key_seq_format(const struct tc_key_seq *seq, char *buf, size_t cap)
{
	if (!seq || !buf || cap == 0 || seq->len == 0)
		return -1;
	size_t pos = 0;
	for (size_t i = 0; i < seq->len; i++) {
		if (i > 0) {
			if (pos + 1 >= cap)
				return -1;
			buf[pos++] = ' ';
		}
		int n = format_one_key(&seq->keys[i], buf + pos, cap - pos);
		if (n < 0)
			return -1;
		pos += (size_t)n;
	}
	return (int)pos;
}

static bool key_eq(const tam_key_event_t *a, const tam_key_event_t *b)
{
	return a->type == b->type && a->codepoint == b->codepoint && a->mods == b->mods;
}

static bool seq_eq(const struct tc_key_seq *a, const struct tc_key_seq *b)
{
	if (a->len != b->len)
		return false;
	for (size_t i = 0; i < a->len; i++) {
		if (!key_eq(&a->keys[i], &b->keys[i]))
			return false;
	}
	return true;
}

int tc_keymap_bind(struct tc_keymap *km, const struct tc_key_seq *seq, enum tc_widget w)
{
	if (!km || !seq || seq->len == 0)
		return -1;
	for (size_t i = 0; i < km->count; i++) {
		if (seq_eq(&km->binds[i].seq, seq)) {
			km->binds[i].widget = w;
			return 0;
		}
	}
	if (km->count >= TC_EDIT_BINDS_MAX)
		return -1;
	km->binds[km->count].seq = *seq;
	km->binds[km->count].widget = w;
	km->count++;
	return 0;
}

bool tc_keymap_unbind(struct tc_keymap *km, const struct tc_key_seq *seq)
{
	if (!km || !seq || seq->len == 0)
		return false;
	for (size_t i = 0; i < km->count; i++) {
		if (seq_eq(&km->binds[i].seq, seq)) {
			memmove(&km->binds[i], &km->binds[i + 1],
				(km->count - i - 1) * sizeof(km->binds[0]));
			km->count--;
			return true;
		}
	}
	return false;
}

enum tc_bind_match tc_keymap_lookup(const struct tc_keymap *km,
				    const tam_key_event_t *keys, size_t len,
				    enum tc_widget *out_widget)
{
	if (!km || !keys || len == 0)
		return TC_MATCH_NONE;
	bool prefix = false;
	for (size_t i = 0; i < km->count; i++) {
		const struct tc_key_seq *s = &km->binds[i].seq;
		if (s->len < len)
			continue;
		bool ok = true;
		for (size_t j = 0; j < len; j++) {
			if (!key_eq(&s->keys[j], &keys[j])) {
				ok = false;
				break;
			}
		}
		if (!ok)
			continue;
		if (s->len == len) {
			if (out_widget)
				*out_widget = km->binds[i].widget;
			return TC_MATCH_EXACT;
		}
		prefix = true;
	}
	return prefix ? TC_MATCH_PREFIX : TC_MATCH_NONE;
}

int tc_keymap_first_binding(const struct tc_keymap *km, enum tc_widget w,
			    char *buf, size_t cap)
{
	if (!km || !buf || cap == 0)
		return 0;
	buf[0] = '\0';
	for (size_t i = 0; i < km->count; i++) {
		if (km->binds[i].widget == w) {
			int n = tc_key_seq_format(&km->binds[i].seq, buf, cap);
			return n > 0 ? n : 0;
		}
	}
	return 0;
}

static void bind_def(struct tc_keymap *km, const char *spec, enum tc_widget w)
{
	struct tc_key_seq seq;
	if (tc_key_seq_parse(spec, &seq) == 0)
		(void)tc_keymap_bind(km, &seq, w);
}

void tc_keymap_init_defaults(struct tc_keymap *km)
{
	memset(km, 0, sizeof(*km));

	/* Universal Ctrl map (keys/keybindings.md / tam-idol/keys/keybindings.md). */
	bind_def(km, "Ctrl+A", TC_W_SEL_ALL);
	bind_def(km, "Ctrl+B", TC_W_NOOP);
	bind_def(km, "Ctrl+C", TC_W_COPY);
	bind_def(km, "Ctrl+D", TC_W_NOOP);
	bind_def(km, "Ctrl+E", TC_W_EXECUTE);
	bind_def(km, "Ctrl+F", TC_W_FIND);
	bind_def(km, "Ctrl+G", TC_W_GUIDANCE);
	bind_def(km, "Ctrl+K", TC_W_KILL_LINE);
	bind_def(km, "Ctrl+L", TC_W_CLEAR_SCREEN);
	bind_def(km, "Ctrl+N", TC_W_NOOP);
	bind_def(km, "Ctrl+O", TC_W_NOOP);
	bind_def(km, "Ctrl+P", TC_W_NOOP);
	bind_def(km, "Ctrl+Q", TC_W_QUIT);
	bind_def(km, "Ctrl+R", TC_W_NOOP);
	bind_def(km, "Ctrl+S", TC_W_NOOP);
	bind_def(km, "Ctrl+T", TC_W_NOOP);
	bind_def(km, "Ctrl+U", TC_W_NOOP);
	bind_def(km, "Ctrl+V", TC_W_PASTE);
	bind_def(km, "Ctrl+W", TC_W_NOOP);
	bind_def(km, "Ctrl+X", TC_W_CUT);
	bind_def(km, "Ctrl+Y", TC_W_REDO);
	bind_def(km, "Ctrl+Z", TC_W_UNDO);
	bind_def(km, "Ctrl+\\", TC_W_BREAK);

	/* Navigation (primary + TERM=linux Alt fallbacks). */
	bind_def(km, "Left",       TC_W_CHAR_LEFT);
	bind_def(km, "Right",      TC_W_CHAR_RIGHT);
	bind_def(km, "Ctrl+Left",  TC_W_WORD_LEFT);
	bind_def(km, "Ctrl+Right", TC_W_WORD_RIGHT);
	bind_def(km, "Alt+,",      TC_W_WORD_LEFT);
	bind_def(km, "Alt+.",      TC_W_WORD_RIGHT);
	bind_def(km, "Home",       TC_W_LINE_START);
	bind_def(km, "End",        TC_W_LINE_END);
	bind_def(km, "Ctrl+Home",  TC_W_BUFFER_START);
	bind_def(km, "Ctrl+End",   TC_W_BUFFER_END);
	bind_def(km, "Alt+<",      TC_W_BUFFER_START);
	bind_def(km, "Alt+>",      TC_W_BUFFER_END);
	bind_def(km, "Up",         TC_W_UP_OR_HIST);
	bind_def(km, "Down",       TC_W_DOWN_OR_HIST);

	/* Selection. */
	bind_def(km, "Shift+Left",       TC_W_SEL_CHAR_LEFT);
	bind_def(km, "Shift+Right",      TC_W_SEL_CHAR_RIGHT);
	bind_def(km, "Ctrl+Shift+Left",  TC_W_SEL_WORD_LEFT);
	bind_def(km, "Ctrl+Shift+Right", TC_W_SEL_WORD_RIGHT);
	bind_def(km, "Shift+Home",       TC_W_SEL_LINE_START);
	bind_def(km, "Shift+End",        TC_W_SEL_LINE_END);
	bind_def(km, "Alt+M",            TC_W_TOGGLE_MARK);

	/* Deletion. */
	bind_def(km, "Backspace",      TC_W_DEL_CHAR_LEFT);
	bind_def(km, "Delete",         TC_W_DEL_CHAR_RIGHT);
	bind_def(km, "Ctrl+Backspace", TC_W_DEL_WORD_LEFT);
	bind_def(km, "Alt+Backspace",  TC_W_DEL_WORD_LEFT);
	bind_def(km, "Ctrl+Delete",    TC_W_DEL_WORD_RIGHT);
	bind_def(km, "Alt+D",          TC_W_DEL_WORD_RIGHT);
	bind_def(km, "Alt+K",          TC_W_DEL_TO_END);
	bind_def(km, "Alt+Shift+K",    TC_W_DEL_TO_START);

	/* Shell-specific. */
	bind_def(km, "Enter",       TC_W_ACCEPT_LINE);
	bind_def(km, "Alt+Enter",   TC_W_INSERT_NEWLINE);
	bind_def(km, "Escape",      TC_W_CANCEL_MODE);
	bind_def(km, "Alt+E",       TC_W_OPEN_EDITOR);
	bind_def(km, "Alt+V",       TC_W_PASTE_PREV);
	bind_def(km, "Alt+Q",       TC_W_INSERT_VERBATIM);
	bind_def(km, "Alt+#",       TC_W_COMMENT_LINE);
	bind_def(km, "Alt+L",       TC_W_INSERT_LAST_ARG);
	bind_def(km, "Alt+N",       TC_W_FIND_NEXT);
	bind_def(km, "Alt+Shift+N", TC_W_FIND_PREV);
	bind_def(km, "Tab",         TC_W_COMPLETE_NEXT);
	bind_def(km, "Shift+Tab",   TC_W_COMPLETE_PREV);
}
