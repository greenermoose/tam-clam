/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A recursive-descent parser written from the POSIX grammar (XCU 2.10).
 * It parses one complete command at a time, so each runs before the next
 * is read, as POSIX requires. */
#ifndef TAM_CLAM_PARSE_H
#define TAM_CLAM_PARSE_H

#include "tam-clam/ast.h"
#include "tam-clam/lex.h"

/* The most simple commands one complete command may hold. */
#define TC_WORDS_MAX 65536
/* Maximum nesting depth of compound commands and function definitions. */
#define TC_PARSE_DEPTH_MAX 128

struct tc_heredoc_pending;

struct tc_parser {
	struct tc_shell *sh;
	struct tc_lexer lx;
	struct tc_token tok;      /* the current token */
	int have_tok;
	struct tc_token peek_tok; /* 1-token lookahead for WORD '(' */
	int have_peek;
	unsigned depth;           /* current compound nesting depth */
	struct tc_heredoc_pending *hd_head;
	struct tc_heredoc_pending *hd_tail;
};

int tc_parser_init(struct tc_parser *p, struct tc_shell *sh, struct tc_input *in);
void tc_parser_finish(struct tc_parser *p);
/* Parse the next complete command into the shell's arena. TC_OK with
 * *out NULL is a blank line. TC_END at the end of input. */
int tc_parse_next(struct tc_parser *p, struct tc_node **out);
/* After an interrupt: forget any lookahead. */
void tc_parser_reset(struct tc_parser *p);
/* After an error: drop the rest of the line (interactive use). */
void tc_parser_recover(struct tc_parser *p);

/* Parse a $(...) command substitution body directly from lx up to the
 * matching ')'. Used by the lexer so command substitutions are parsed once
 * at parse time. */
int tc_parse_cmdsub(struct tc_lexer *lx, unsigned long start_line,
		    struct tc_node **out);

/* Parse a `...` command substitution body from an unescaped string. */
int tc_parse_cmdsub_string(struct tc_shell *sh, const char *src, size_t len,
			   unsigned long start_line, unsigned depth,
			   struct tc_node **out);

#endif
