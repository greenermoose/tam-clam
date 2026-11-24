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
struct tc_editor;

/* tc_input_getc returns a byte (0 to 255) or one of these. */
enum {
	TC_IN_EOF = -1,
	TC_IN_ERROR = -2, /* read failed; err holds errno */
	TC_IN_INTR = -3,  /* interrupted by SIGINT */
};

#define TC_INPUT_BUF 4096
#define TC_ALIAS_DEPTH_MAX 64

struct tc_alias_frame {
	char *name;
	char *buf;
	size_t len;
	size_t pos;
	bool done;
	bool ends_blank;
};

struct tc_input {
	int fd;                 /* -1 for a string */
	const char *str;        /* string input */
	size_t str_len, str_pos;
	unsigned char buf[TC_INPUT_BUF];
	size_t len, pos;
	unsigned char *push;    /* LIFO pushback buffer for $(( disambiguation */
	size_t push_len, push_pos;
	struct tc_alias_frame alias_stack[TC_ALIAS_DEPTH_MAX];
	size_t alias_depth;
	bool alias_ended_blank; /* a frame ending in blank just finished */
	bool bytewise;          /* read one byte at a time */
	bool eof;
	int err;
	unsigned long line;     /* line number of the next byte */
	unsigned long col;      /* 1-based column of the last returned byte */
	bool prompt;            /* write prompts (interactive) */
	bool at_line_start;
	bool want_ps1;          /* next prompt is PS1, not PS2 */
	struct tc_shell *sh;    /* shell for PS1/PS2 lookup and $EDITOR */
	struct tc_editor *ed;   /* interactive line editor, or NULL */
	char *ed_line;          /* buffered line from line editor */
	size_t ed_len, ed_pos;
};

void tc_input_from_string(struct tc_input *in, const char *s, size_t len);
/* fd is owned by the caller. bytewise is for descriptors the shell shares
 * with the commands it runs. */
void tc_input_from_fd(struct tc_input *in, int fd, bool bytewise);
void tc_input_finish(struct tc_input *in);
/* Push bytes back so they are read before the remaining input. 0 or -1. */
int tc_input_pushback(struct tc_input *in, const char *s, size_t len);
/* Push a single lookahead byte back to whichever source produced it. 0 or -1. */
int tc_input_ungetc_byte(struct tc_input *in, unsigned char c);
/* Push an alias expansion frame onto the input stack. 0 ok, -1 OOM, -2 depth. */
int tc_input_push_alias(struct tc_input *in, const char *name,
			const char *val, size_t len);
/* True if alias name is currently active on the expansion stack. */
bool tc_input_alias_active(const struct tc_input *in, const char *name);
/* Pop finished alias frames from the top of the stack. */
void tc_input_pop_done_aliases(struct tc_input *in);
int tc_input_getc(struct tc_input *in);
/* Ask for the primary prompt before the next line is read. */
void tc_input_new_command(struct tc_input *in);

#endif
