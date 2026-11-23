/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tam-clam/input.h"
#include "tam-clam/shell.h"
#include "tam-clam/signals.h"

static void input_common(struct tc_input *in)
{
	in->len = in->pos = 0;
	in->push = NULL;
	in->push_len = in->push_pos = 0;
	in->alias_depth = 0;
	in->alias_ended_blank = false;
	in->eof = false;
	in->err = 0;
	in->line = 1;
	in->prompt = false;
	in->at_line_start = true;
	in->want_ps1 = true;
	in->sh = NULL;
}

void tc_input_finish(struct tc_input *in)
{
	while (in->alias_depth > 0) {
		in->alias_depth--;
		free(in->alias_stack[in->alias_depth].name);
		free(in->alias_stack[in->alias_depth].buf);
	}
	free(in->push);
	in->push = NULL;
	in->push_len = in->push_pos = 0;
}

int tc_input_push_alias(struct tc_input *in, const char *name,
			const char *val, size_t len)
{
	if (in->alias_depth >= TC_ALIAS_DEPTH_MAX)
		return -2;
	size_t nlen = strlen(name);
	char *ncopy = malloc(nlen + 1);
	char *vcopy = len ? malloc(len) : NULL;
	if (!ncopy || (len && !vcopy)) {
		free(ncopy);
		free(vcopy);
		return -1;
	}
	memcpy(ncopy, name, nlen + 1);
	if (len)
		memcpy(vcopy, val, len);
	struct tc_alias_frame *f = &in->alias_stack[in->alias_depth++];
	f->name = ncopy;
	f->buf = vcopy;
	f->len = len;
	f->pos = 0;
	f->done = false;
	f->ends_blank = (len > 0 && (val[len - 1] == ' ' || val[len - 1] == '\t'));
	return 0;
}

bool tc_input_alias_active(const struct tc_input *in, const char *name)
{
	for (size_t i = 0; i < in->alias_depth; i++) {
		if (strcmp(in->alias_stack[i].name, name) == 0)
			return true;
	}
	return false;
}

void tc_input_pop_done_aliases(struct tc_input *in)
{
	while (in->alias_depth > 0 && in->alias_stack[in->alias_depth - 1].done) {
		in->alias_depth--;
		free(in->alias_stack[in->alias_depth].name);
		free(in->alias_stack[in->alias_depth].buf);
	}
}

int tc_input_ungetc_byte(struct tc_input *in, unsigned char c)
{
	for (size_t i = in->alias_depth; i > 0; i--) {
		struct tc_alias_frame *f = &in->alias_stack[i - 1];
		if (!f->done && f->pos > 0 && (unsigned char)f->buf[f->pos - 1] == c) {
			f->pos--;
			return 0;
		}
		if (!f->done)
			break;
	}
	char ch = (char)c;
	return tc_input_pushback(in, &ch, 1);
}

int tc_input_pushback(struct tc_input *in, const char *s, size_t len)
{
	if (len == 0)
		return 0;
	size_t rem = in->push_len - in->push_pos;
	if (len > SIZE_MAX - rem)
		return -1;
	unsigned char *nb = malloc(len + rem);
	if (!nb)
		return -1;
	memcpy(nb, s, len);
	if (rem > 0)
		memcpy(nb + len, in->push + in->push_pos, rem);
	free(in->push);
	in->push = nb;
	in->push_len = len + rem;
	in->push_pos = 0;
	for (size_t i = 0; i < len; i++) {
		if (s[i] == '\n' && in->line > 1)
			in->line--;
	}
	return 0;
}

void tc_input_from_string(struct tc_input *in, const char *s, size_t len)
{
	input_common(in);
	in->fd = -1;
	in->str = s;
	in->str_len = len;
	in->str_pos = 0;
	in->bytewise = false;
}

void tc_input_from_fd(struct tc_input *in, int fd, bool bytewise)
{
	input_common(in);
	in->fd = fd;
	in->str = NULL;
	in->str_len = in->str_pos = 0;
	in->bytewise = bytewise;
}

void tc_input_new_command(struct tc_input *in)
{
	in->want_ps1 = true;
}

static void write_prompt(struct tc_input *in)
{
	const char *p = NULL;
	if (in->sh)
		p = tc_var_get(&in->sh->vars, in->want_ps1 ? "PS1" : "PS2");
	else
		p = in->want_ps1 ? "$ " : "> ";
	in->want_ps1 = false;
	if (p && *p)
		(void)tc_write_all(STDERR_FILENO, p, strlen(p));
}

static int fill(struct tc_input *in)
{
	if (in->eof)
		return TC_IN_EOF;
	if (in->prompt && in->at_line_start)
		write_prompt(in);
	size_t want = in->bytewise ? 1 : sizeof(in->buf);
	for (;;) {
		ssize_t n = read(in->fd, in->buf, want);
		if (n > 0) {
			in->len = (size_t)n;
			in->pos = 0;
			return TC_OK;
		}
		if (n == 0) {
			in->eof = true;
			return TC_IN_EOF;
		}
		if (errno == EINTR) {
			if (tc_sigint_seen)
				return TC_IN_INTR;
			continue;
		}
		in->err = errno;
		return TC_IN_ERROR;
	}
}

int tc_input_getc(struct tc_input *in)
{
	for (size_t i = in->alias_depth; i > 0; i--) {
		struct tc_alias_frame *f = &in->alias_stack[i - 1];
		if (f->done)
			continue;
		if (f->pos < f->len)
			return (unsigned char)f->buf[f->pos++];
		f->done = true;
		if (f->ends_blank)
			in->alias_ended_blank = true;
	}

	int c;
	if (in->push && in->push_pos < in->push_len) {
		c = in->push[in->push_pos++];
		if (in->push_pos == in->push_len) {
			free(in->push);
			in->push = NULL;
			in->push_len = in->push_pos = 0;
		}
	} else if (in->fd < 0) {
		if (in->str_pos >= in->str_len)
			return TC_IN_EOF;
		c = (unsigned char)in->str[in->str_pos++];
	} else {
		if (in->pos >= in->len) {
			int r = fill(in);
			if (r != TC_OK)
				return r;
		}
		c = in->buf[in->pos++];
	}
	in->at_line_start = (c == '\n');
	if (c == '\n')
		in->line++;
	return c;
}
