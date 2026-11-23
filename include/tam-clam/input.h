/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Input sources: a string (-c), a script file, or standard input.
 *
 * Standard input is read one byte at a time, as POSIX requires, so a
 * command the shell runs can read the lines that follow it (for example
 * "cat" in "sh <<script"). Script files the shell opened itself are read
 * in blocks. */
#ifndef TAM_CLAM_INPUT_H
#define TAM_CLAM_INPUT_H

#include <stdbool.h>
#include <stddef.h>

struct tc_shell;

/* tc_input_getc returns a byte (0 to 255) or one of these. */
enum {
	TC_IN_EOF = -1,
	TC_IN_ERROR = -2, /* read failed; err holds errno */
	TC_IN_INTR = -3,  /* interrupted by SIGINT */
};

#define TC_INPUT_BUF 4096

struct tc_input {
	int fd;                 /* -1 for a string */
	const char *str;        /* string input */
	size_t str_len, str_pos;
	unsigned char buf[TC_INPUT_BUF];
	size_t len, pos;
	unsigned char *push;    /* LIFO pushback buffer for $(( disambiguation */
	size_t push_len, push_pos;
	bool bytewise;          /* read one byte at a time */
	bool eof;
	int err;
	unsigned long line;     /* line number of the next byte */
	bool prompt;            /* write prompts (interactive) */
	bool at_line_start;
	bool want_ps1;          /* next prompt is PS1, not PS2 */
	const struct tc_shell *sh; /* shell for PS1/PS2 lookup */
};

void tc_input_from_string(struct tc_input *in, const char *s, size_t len);
/* fd is owned by the caller. bytewise is for descriptors the shell shares
 * with the commands it runs. */
void tc_input_from_fd(struct tc_input *in, int fd, bool bytewise);
void tc_input_finish(struct tc_input *in);
/* Push bytes back so they are read before the remaining input. 0 or -1. */
int tc_input_pushback(struct tc_input *in, const char *s, size_t len);
int tc_input_getc(struct tc_input *in);
/* Ask for the primary prompt before the next line is read. */
void tc_input_new_command(struct tc_input *in);

#endif
