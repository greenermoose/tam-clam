/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pure UTF-8 editable buffer, selection, undo/redo, and clipboard ring. */
#include <stdlib.h>
#include <string.h>

#include "tam-clam/edit.h"
#include "tam_uni.h"

static void snap_free(struct tc_edit_snap *s)
{
	free(s->data);
	s->data = NULL;
	s->len = 0;
	s->cursor = 0;
	s->has_mark = false;
	s->mark = 0;
}

static void stack_clear(struct tc_edit_snap *stack, size_t *len)
{
	for (size_t i = 0; i < *len; i++)
		snap_free(&stack[i]);
	*len = 0;
}

static int snap_capture(const struct tc_edit_buf *b, struct tc_edit_snap *out)
{
	out->data = NULL;
	out->len = b->len;
	out->cursor = b->cursor;
	out->has_mark = b->has_mark;
	out->mark = b->mark;
	if (b->len > 0) {
		out->data = malloc(b->len);
		if (!out->data)
			return -1;
		memcpy(out->data, b->data, b->len);
	}
	return 0;
}

static int push_undo(struct tc_edit_buf *b)
{
	struct tc_edit_snap s;
	if (snap_capture(b, &s) < 0)
		return -1;
	if (b->undo_len == TC_EDIT_UNDO_MAX) {
		snap_free(&b->undo_stack[0]);
		memmove(&b->undo_stack[0], &b->undo_stack[1],
			(TC_EDIT_UNDO_MAX - 1) * sizeof(b->undo_stack[0]));
		b->undo_len--;
	}
	b->undo_stack[b->undo_len++] = s;
	stack_clear(b->redo_stack, &b->redo_len);
	return 0;
}

static int ensure_cap(struct tc_edit_buf *b, size_t need)
{
	if (need > TC_EDIT_BUF_MAX)
		return -1;
	if (need + 1 <= b->cap)
		return 0;
	size_t ncap = b->cap ? b->cap : 128;
	while (ncap < need + 1) {
		if (ncap > TC_EDIT_BUF_MAX / 2) {
			ncap = TC_EDIT_BUF_MAX + 1;
			break;
		}
		ncap *= 2;
	}
	char *nd = realloc(b->data, ncap);
	if (!nd)
		return -1;
	b->data = nd;
	b->cap = ncap;
	return 0;
}

void tc_ebuf_init(struct tc_edit_buf *b)
{
	memset(b, 0, sizeof(*b));
}

void tc_ebuf_finish(struct tc_edit_buf *b)
{
	free(b->data);
	stack_clear(b->undo_stack, &b->undo_len);
	stack_clear(b->redo_stack, &b->redo_len);
	memset(b, 0, sizeof(*b));
}

void tc_ebuf_clear(struct tc_edit_buf *b)
{
	b->len = 0;
	b->cursor = 0;
	b->has_mark = false;
	b->mark = 0;
	b->last_was_paste = false;
	if (b->data)
		b->data[0] = '\0';
	stack_clear(b->undo_stack, &b->undo_len);
	stack_clear(b->redo_stack, &b->redo_len);
}

int tc_ebuf_set(struct tc_edit_buf *b, const char *s, size_t len)
{
	if (ensure_cap(b, len) < 0)
		return -1;
	if (len > 0)
		memcpy(b->data, s, len);
	b->len = len;
	b->data[len] = '\0';
	b->cursor = len;
	b->has_mark = false;
	b->mark = 0;
	b->last_was_paste = false;
	return 0;
}

size_t tc_ebuf_prev_cp(const struct tc_edit_buf *b, size_t pos)
{
	if (pos == 0 || !b->data)
		return 0;
	if (pos > b->len)
		pos = b->len;
	return tam_utf8_prev(b->data, pos);
}

size_t tc_ebuf_next_cp(const struct tc_edit_buf *b, size_t pos)
{
	if (!b->data || pos >= b->len)
		return b->len;
	uint32_t cp = 0;
	size_t n = tam_utf8_decode(b->data + pos, b->len - pos, &cp);
	if (n == 0)
		n = 1;
	return pos + n <= b->len ? pos + n : b->len;
}

static bool is_word_byte(unsigned char c)
{
	if (c >= 0x80)
		return true;
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '_';
}

size_t tc_ebuf_word_left_pos(const struct tc_edit_buf *b, size_t pos)
{
	if (pos == 0 || !b->data)
		return 0;
	if (pos > b->len)
		pos = b->len;
	while (pos > 0) {
		size_t prev = tc_ebuf_prev_cp(b, pos);
		unsigned char c = (unsigned char)b->data[prev];
		if (c != ' ' && c != '\t' && c != '\n')
			break;
		pos = prev;
	}
	if (pos == 0)
		return 0;
	size_t p0 = tc_ebuf_prev_cp(b, pos);
	bool word = is_word_byte((unsigned char)b->data[p0]);
	while (pos > 0) {
		size_t prev = tc_ebuf_prev_cp(b, pos);
		unsigned char c = (unsigned char)b->data[prev];
		if (c == ' ' || c == '\t' || c == '\n')
			break;
		if (is_word_byte(c) != word)
			break;
		pos = prev;
	}
	return pos;
}

size_t tc_ebuf_word_right_pos(const struct tc_edit_buf *b, size_t pos)
{
	if (!b->data || pos >= b->len)
		return b->len;
	while (pos < b->len) {
		unsigned char c = (unsigned char)b->data[pos];
		if (c != ' ' && c != '\t' && c != '\n')
			break;
		pos = tc_ebuf_next_cp(b, pos);
	}
	if (pos >= b->len)
		return b->len;
	bool word = is_word_byte((unsigned char)b->data[pos]);
	while (pos < b->len) {
		unsigned char c = (unsigned char)b->data[pos];
		if (c == ' ' || c == '\t' || c == '\n')
			break;
		if (is_word_byte(c) != word)
			break;
		pos = tc_ebuf_next_cp(b, pos);
	}
	return pos;
}

size_t tc_ebuf_line_start_pos(const struct tc_edit_buf *b, size_t pos)
{
	if (!b->data || pos == 0)
		return 0;
	if (pos > b->len)
		pos = b->len;
	while (pos > 0 && b->data[pos - 1] != '\n')
		pos--;
	return pos;
}

size_t tc_ebuf_line_end_pos(const struct tc_edit_buf *b, size_t pos)
{
	if (!b->data || pos >= b->len)
		return b->len;
	while (pos < b->len && b->data[pos] != '\n')
		pos++;
	return pos;
}

bool tc_ebuf_sel_range(const struct tc_edit_buf *b, size_t *start, size_t *end)
{
	if (!b->has_mark || b->mark == b->cursor)
		return false;
	if (b->mark < b->cursor) {
		*start = b->mark;
		*end = b->cursor;
	} else {
		*start = b->cursor;
		*end = b->mark;
	}
	return true;
}

void tc_ebuf_sel_clear(struct tc_edit_buf *b)
{
	b->has_mark = false;
	b->mark = 0;
}

void tc_ebuf_sel_begin(struct tc_edit_buf *b)
{
	if (!b->has_mark) {
		b->has_mark = true;
		b->mark = b->cursor;
	}
}

void tc_ebuf_select_all(struct tc_edit_buf *b)
{
	b->last_was_paste = false;
	if (b->len == 0) {
		tc_ebuf_sel_clear(b);
		return;
	}
	b->has_mark = true;
	b->mark = 0;
	b->cursor = b->len;
}

void tc_ebuf_toggle_mark(struct tc_edit_buf *b)
{
	b->last_was_paste = false;
	if (b->has_mark) {
		tc_ebuf_sel_clear(b);
	} else {
		b->has_mark = true;
		b->mark = b->cursor;
	}
}

void tc_ebuf_move(struct tc_edit_buf *b, size_t new_pos, bool extend_sel)
{
	b->last_was_paste = false;
	if (new_pos > b->len)
		new_pos = b->len;
	if (extend_sel)
		tc_ebuf_sel_begin(b);
	else
		tc_ebuf_sel_clear(b);
	b->cursor = new_pos;
}

bool tc_ebuf_move_line_up(struct tc_edit_buf *b, bool extend_sel)
{
	size_t ls = tc_ebuf_line_start_pos(b, b->cursor);
	if (ls == 0)
		return false;
	int col = b->data ? tam_uni_str_width(b->data + ls, b->cursor - ls) : 0;
	size_t prev_end = ls - 1;
	size_t prev_start = tc_ebuf_line_start_pos(b, prev_end);
	size_t pos = prev_start;
	int w = 0;
	while (pos < prev_end) {
		uint32_t cp = 0;
		size_t n = tam_utf8_decode(b->data + pos, prev_end - pos, &cp);
		if (n == 0)
			n = 1;
		int cw = tam_uni_width(cp);
		if (cw < 0)
			cw = 0;
		if (w + cw > col)
			break;
		w += cw;
		pos += n;
	}
	tc_ebuf_move(b, pos, extend_sel);
	return true;
}

bool tc_ebuf_move_line_down(struct tc_edit_buf *b, bool extend_sel)
{
	size_t le = tc_ebuf_line_end_pos(b, b->cursor);
	if (le >= b->len)
		return false;
	size_t ls = tc_ebuf_line_start_pos(b, b->cursor);
	int col = b->data ? tam_uni_str_width(b->data + ls, b->cursor - ls) : 0;
	size_t next_start = le + 1;
	size_t next_end = tc_ebuf_line_end_pos(b, next_start);
	size_t pos = next_start;
	int w = 0;
	while (pos < next_end) {
		uint32_t cp = 0;
		size_t n = tam_utf8_decode(b->data + pos, next_end - pos, &cp);
		if (n == 0)
			n = 1;
		int cw = tam_uni_width(cp);
		if (cw < 0)
			cw = 0;
		if (w + cw > col)
			break;
		w += cw;
		pos += n;
	}
	tc_ebuf_move(b, pos, extend_sel);
	return true;
}

static void delete_range_raw(struct tc_edit_buf *b, size_t start, size_t end)
{
	if (end <= start || end > b->len)
		return;
	memmove(b->data + start, b->data + end, b->len - end);
	b->len -= (end - start);
	b->data[b->len] = '\0';
	b->cursor = start;
	tc_ebuf_sel_clear(b);
}

int tc_ebuf_delete_range(struct tc_edit_buf *b, size_t start, size_t end, bool record_undo)
{
	b->last_was_paste = false;
	if (end <= start || end > b->len)
		return 0;
	if (record_undo && push_undo(b) < 0)
		return -1;
	delete_range_raw(b, start, end);
	return 0;
}

int tc_ebuf_insert(struct tc_edit_buf *b, const char *s, size_t len, bool record_undo)
{
	b->last_was_paste = false;
	size_t s0 = 0, s1 = 0;
	bool has_sel = tc_ebuf_sel_range(b, &s0, &s1);
	size_t del_len = has_sel ? (s1 - s0) : 0;
	if (len > TC_EDIT_BUF_MAX || b->len - del_len > TC_EDIT_BUF_MAX - len)
		return -1;
	if (record_undo && push_undo(b) < 0)
		return -1;
	if (ensure_cap(b, b->len - del_len + len) < 0)
		return -1;
	if (has_sel)
		delete_range_raw(b, s0, s1);
	if (len > 0) {
		memmove(b->data + b->cursor + len, b->data + b->cursor, b->len - b->cursor);
		memcpy(b->data + b->cursor, s, len);
		b->len += len;
		b->cursor += len;
		b->data[b->len] = '\0';
	}
	return 0;
}

int tc_ebuf_insert_cp(struct tc_edit_buf *b, uint32_t cp, bool record_undo)
{
	char utf8[4];
	size_t n = tam_utf8_encode(cp, utf8);
	return tc_ebuf_insert(b, utf8, n, record_undo);
}

int tc_ebuf_delete_char_left(struct tc_edit_buf *b)
{
	size_t s0 = 0, s1 = 0;
	if (tc_ebuf_sel_range(b, &s0, &s1))
		return tc_ebuf_delete_range(b, s0, s1, true);
	if (b->cursor == 0)
		return 0;
	size_t prev = tc_ebuf_prev_cp(b, b->cursor);
	return tc_ebuf_delete_range(b, prev, b->cursor, true);
}

int tc_ebuf_delete_char_right(struct tc_edit_buf *b)
{
	size_t s0 = 0, s1 = 0;
	if (tc_ebuf_sel_range(b, &s0, &s1))
		return tc_ebuf_delete_range(b, s0, s1, true);
	if (b->cursor >= b->len)
		return 0;
	size_t next = tc_ebuf_next_cp(b, b->cursor);
	return tc_ebuf_delete_range(b, b->cursor, next, true);
}

int tc_ebuf_delete_word_left(struct tc_edit_buf *b, struct tc_clip_ring *clip)
{
	size_t s0 = 0, s1 = 0;
	if (tc_ebuf_sel_range(b, &s0, &s1)) {
		if (clip && tc_clip_push(clip, b->data + s0, s1 - s0) < 0)
			return -1;
		return tc_ebuf_delete_range(b, s0, s1, true);
	}
	size_t start = tc_ebuf_word_left_pos(b, b->cursor);
	if (start == b->cursor)
		return 0;
	if (clip && tc_clip_push(clip, b->data + start, b->cursor - start) < 0)
		return -1;
	return tc_ebuf_delete_range(b, start, b->cursor, true);
}

int tc_ebuf_delete_word_right(struct tc_edit_buf *b, struct tc_clip_ring *clip)
{
	size_t s0 = 0, s1 = 0;
	if (tc_ebuf_sel_range(b, &s0, &s1)) {
		if (clip && tc_clip_push(clip, b->data + s0, s1 - s0) < 0)
			return -1;
		return tc_ebuf_delete_range(b, s0, s1, true);
	}
	size_t end = tc_ebuf_word_right_pos(b, b->cursor);
	if (end == b->cursor)
		return 0;
	if (clip && tc_clip_push(clip, b->data + b->cursor, end - b->cursor) < 0)
		return -1;
	return tc_ebuf_delete_range(b, b->cursor, end, true);
}

int tc_ebuf_delete_to_line_start(struct tc_edit_buf *b, struct tc_clip_ring *clip)
{
	size_t ls = tc_ebuf_line_start_pos(b, b->cursor);
	if (ls == b->cursor)
		return 0;
	if (clip && tc_clip_push(clip, b->data + ls, b->cursor - ls) < 0)
		return -1;
	return tc_ebuf_delete_range(b, ls, b->cursor, true);
}

int tc_ebuf_delete_to_line_end(struct tc_edit_buf *b, struct tc_clip_ring *clip)
{
	size_t le = tc_ebuf_line_end_pos(b, b->cursor);
	if (le == b->cursor)
		return 0;
	if (clip && tc_clip_push(clip, b->data + b->cursor, le - b->cursor) < 0)
		return -1;
	return tc_ebuf_delete_range(b, b->cursor, le, true);
}

int tc_ebuf_kill_line(struct tc_edit_buf *b, struct tc_clip_ring *clip)
{
	if (b->len == 0)
		return 0;
	if (clip && tc_clip_push(clip, b->data, b->len) < 0)
		return -1;
	return tc_ebuf_delete_range(b, 0, b->len, true);
}

int tc_ebuf_comment_line(struct tc_edit_buf *b)
{
	b->last_was_paste = false;
	if (push_undo(b) < 0)
		return -1;
	size_t lines = 1;
	for (size_t i = 0; i < b->len; i++) {
		if (b->data[i] == '\n')
			lines++;
	}
	size_t add = lines * 2;
	if (b->len > TC_EDIT_BUF_MAX - add)
		return -1;
	char *nb = malloc(b->len + add + 1);
	if (!nb)
		return -1;
	size_t out = 0;
	nb[out++] = '#';
	nb[out++] = ' ';
	for (size_t i = 0; i < b->len; i++) {
		nb[out++] = b->data[i];
		if (b->data[i] == '\n') {
			nb[out++] = '#';
			nb[out++] = ' ';
		}
	}
	nb[out] = '\0';
	int rc = tc_ebuf_set(b, nb, out);
	free(nb);
	return rc;
}

static int apply_snap(struct tc_edit_buf *b, const struct tc_edit_snap *s)
{
	if (ensure_cap(b, s->len) < 0)
		return -1;
	if (s->len > 0)
		memcpy(b->data, s->data, s->len);
	b->len = s->len;
	if (b->data)
		b->data[b->len] = '\0';
	b->cursor = s->cursor;
	b->has_mark = s->has_mark;
	b->mark = s->mark;
	b->last_was_paste = false;
	return 0;
}

int tc_ebuf_undo(struct tc_edit_buf *b)
{
	if (b->undo_len == 0)
		return 0;
	struct tc_edit_snap cur;
	if (snap_capture(b, &cur) < 0)
		return -1;
	struct tc_edit_snap prev = b->undo_stack[b->undo_len - 1];
	if (apply_snap(b, &prev) < 0) {
		snap_free(&cur);
		return -1;
	}
	b->undo_len--;
	snap_free(&prev);
	if (b->redo_len == TC_EDIT_UNDO_MAX) {
		snap_free(&b->redo_stack[0]);
		memmove(&b->redo_stack[0], &b->redo_stack[1],
			(TC_EDIT_UNDO_MAX - 1) * sizeof(b->redo_stack[0]));
		b->redo_len--;
	}
	b->redo_stack[b->redo_len++] = cur;
	return 1;
}

int tc_ebuf_redo(struct tc_edit_buf *b)
{
	if (b->redo_len == 0)
		return 0;
	struct tc_edit_snap cur;
	if (snap_capture(b, &cur) < 0)
		return -1;
	struct tc_edit_snap next = b->redo_stack[b->redo_len - 1];
	if (apply_snap(b, &next) < 0) {
		snap_free(&cur);
		return -1;
	}
	b->redo_len--;
	snap_free(&next);
	if (b->undo_len == TC_EDIT_UNDO_MAX) {
		snap_free(&b->undo_stack[0]);
		memmove(&b->undo_stack[0], &b->undo_stack[1],
			(TC_EDIT_UNDO_MAX - 1) * sizeof(b->undo_stack[0]));
		b->undo_len--;
	}
	b->undo_stack[b->undo_len++] = cur;
	return 1;
}

void tc_clip_init(struct tc_clip_ring *r)
{
	memset(r, 0, sizeof(*r));
}

void tc_clip_finish(struct tc_clip_ring *r)
{
	for (size_t i = 0; i < TC_EDIT_CLIP_MAX; i++)
		free(r->items[i]);
	memset(r, 0, sizeof(*r));
}

int tc_clip_push(struct tc_clip_ring *r, const char *s, size_t len)
{
	if (!r || len == 0)
		return 0;
	char *copy = malloc(len + 1);
	if (!copy)
		return -1;
	memcpy(copy, s, len);
	copy[len] = '\0';
	size_t slot = r->count == 0 ? 0 : (r->head + 1) % TC_EDIT_CLIP_MAX;
	free(r->items[slot]);
	r->items[slot] = copy;
	r->lens[slot] = len;
	r->head = slot;
	if (r->count < TC_EDIT_CLIP_MAX)
		r->count++;
	return 0;
}

const char *tc_clip_get(const struct tc_clip_ring *r, size_t age, size_t *out_len)
{
	if (!r || r->count == 0 || age >= r->count)
		return NULL;
	size_t idx = (r->head + TC_EDIT_CLIP_MAX - age) % TC_EDIT_CLIP_MAX;
	if (out_len)
		*out_len = r->lens[idx];
	return r->items[idx];
}

int tc_ebuf_copy(struct tc_edit_buf *b, struct tc_clip_ring *clip,
		 const char **copied, size_t *copied_len)
{
	b->last_was_paste = false;
	size_t s0 = 0, s1 = 0;
	if (!tc_ebuf_sel_range(b, &s0, &s1)) {
		s0 = tc_ebuf_line_start_pos(b, b->cursor);
		s1 = tc_ebuf_line_end_pos(b, b->cursor);
	}
	if (s1 <= s0) {
		if (copied)
			*copied = NULL;
		if (copied_len)
			*copied_len = 0;
		return 0;
	}
	if (clip && tc_clip_push(clip, b->data + s0, s1 - s0) < 0)
		return -1;
	if (copied)
		*copied = tc_clip_get(clip, 0, copied_len);
	return 0;
}

int tc_ebuf_cut(struct tc_edit_buf *b, struct tc_clip_ring *clip,
		const char **cut_text, size_t *cut_len)
{
	b->last_was_paste = false;
	size_t s0 = 0, s1 = 0;
	if (!tc_ebuf_sel_range(b, &s0, &s1)) {
		s0 = tc_ebuf_line_start_pos(b, b->cursor);
		s1 = tc_ebuf_line_end_pos(b, b->cursor);
	}
	if (s1 <= s0) {
		if (cut_text)
			*cut_text = NULL;
		if (cut_len)
			*cut_len = 0;
		return 0;
	}
	if (clip && tc_clip_push(clip, b->data + s0, s1 - s0) < 0)
		return -1;
	if (tc_ebuf_delete_range(b, s0, s1, true) < 0)
		return -1;
	if (cut_text)
		*cut_text = tc_clip_get(clip, 0, cut_len);
	return 0;
}

int tc_ebuf_paste(struct tc_edit_buf *b, const struct tc_clip_ring *clip)
{
	size_t len = 0;
	const char *txt = tc_clip_get(clip, 0, &len);
	if (!txt || len == 0)
		return 0;
	size_t s0 = 0, s1 = 0;
	size_t start = tc_ebuf_sel_range(b, &s0, &s1) ? s0 : b->cursor;
	if (tc_ebuf_insert(b, txt, len, true) < 0)
		return -1;
	b->last_was_paste = true;
	b->paste_start = start;
	b->paste_end = start + len;
	b->paste_ring_idx = 0;
	return 0;
}

int tc_ebuf_paste_prev(struct tc_edit_buf *b, const struct tc_clip_ring *clip)
{
	if (!b->last_was_paste || !clip || clip->count < 2)
		return 0;
	if (b->paste_end > b->len || b->paste_start > b->paste_end)
		return 0;
	size_t next_age = (b->paste_ring_idx + 1) % clip->count;
	size_t len = 0;
	const char *txt = tc_clip_get(clip, next_age, &len);
	if (!txt)
		return 0;
	size_t start = b->paste_start;
	size_t old_len = b->paste_end - start;
	if (len > TC_EDIT_BUF_MAX || b->len - old_len > TC_EDIT_BUF_MAX - len)
		return -1;
	if (push_undo(b) < 0)
		return -1;
	if (ensure_cap(b, b->len - old_len + len) < 0)
		return -1;
	delete_range_raw(b, start, start + old_len);
	memmove(b->data + start + len, b->data + start, b->len - start);
	memcpy(b->data + start, txt, len);
	b->len += len;
	b->cursor = start + len;
	b->data[b->len] = '\0';
	b->last_was_paste = true;
	b->paste_start = start;
	b->paste_end = start + len;
	b->paste_ring_idx = next_age;
	return 0;
}
