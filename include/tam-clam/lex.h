/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The lexer turns input into POSIX tokens (XCU 2.3): words and operators.
 * A word keeps its quotes; expand.c removes them. Flags record what a
 * word contains, so the parser can refuse what tam-clam does not support
 * yet instead of running it wrongly. */
#ifndef TAM_CLAM_LEX_H
#define TAM_CLAM_LEX_H

#include <stddef.h>

#include "tam-clam/shell.h"

/* The longest word the lexer accepts. */
#define TC_WORD_MAX ((size_t)1 << 20)

enum tc_token_kind {
	TC_TOK_WORD,
	TC_TOK_NEWLINE,
	TC_TOK_END,       /* end of input */
	/* Operators, XCU 2.10.1. */
	TC_TOK_SEMI,      /* ; */
	TC_TOK_AMP,       /* & */
	TC_TOK_PIPE,      /* | */
	TC_TOK_AND_IF,    /* && */
	TC_TOK_OR_IF,     /* || */
	TC_TOK_DSEMI,     /* ;; */
	TC_TOK_LPAREN,    /* ( */
	TC_TOK_RPAREN,    /* ) */
	TC_TOK_LESS,      /* < */
	TC_TOK_GREAT,     /* > */
	TC_TOK_DLESS,     /* << */
	TC_TOK_DGREAT,    /* >> */
	TC_TOK_LESSAND,   /* <& */
	TC_TOK_GREATAND,  /* >& */
	TC_TOK_LESSGREAT, /* <> */
	TC_TOK_DLESSDASH, /* <<- */
	TC_TOK_CLOBBER,   /* >| */
};

/* What a word contains, outside single quotes. */
enum {
	TC_WORD_QUOTED = 1 << 0,  /* some part is quoted */
	TC_WORD_DOLLAR = 1 << 1,  /* $ : parameter, command or arithmetic */
	TC_WORD_BACKQ = 1 << 2,   /* ` : command substitution */
	TC_WORD_GLOB = 1 << 3,    /* unquoted * ? [ */
	TC_WORD_TILDE = 1 << 4,   /* unquoted ~ at the start */
	TC_WORD_ASSIGN = 1 << 5,  /* NAME= at the start */
};

struct tc_token {
	enum tc_token_kind kind;
	const char *text;  /* WORD: raw text, in the arena, NUL-terminated */
	size_t len;
	unsigned flags;
	unsigned long line;
};

struct tc_lexer {
	struct tc_shell *sh;
	struct tc_input *in;
	int peek;          /* one byte of lookahead, or -10 for none */
	char *word;        /* tam_str buffer, reused for each word */
};

int tc_lexer_init(struct tc_lexer *lx, struct tc_shell *sh, struct tc_input *in);
void tc_lexer_finish(struct tc_lexer *lx);
/* Read the next token. TC_OK, or an error result from shell.h. */
int tc_lex_next(struct tc_lexer *lx, struct tc_token *tok);
/* Forget the lookahead byte, after an interrupt. */
void tc_lexer_reset(struct tc_lexer *lx);
/* Throw away the rest of the current line, after an error. */
void tc_lex_skip_line(struct tc_lexer *lx);
/* The text of an operator token, for messages. */
const char *tc_token_name(enum tc_token_kind kind);

#endif
