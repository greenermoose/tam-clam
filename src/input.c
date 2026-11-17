/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "tam-clam/input.h"
#include "tam-clam/shell.h"
#include "tam-clam/signals.h"

static void input_common(struct tc_input *in)
{
	in->len = in->pos = 0;
	in->eof = false;
	in->err = 0;
	in->line = 1;
	in->prompt = false;
	in->at_line_start = true;
	in->want_ps1 = true;
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
	/* PS1 and PS2 become variables in 0.0.3; these are their defaults. */
	const char *p = in->want_ps1 ? "$ " : "> ";
	in->want_ps1 = false;
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
	int c;
	if (in->fd < 0) {
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
