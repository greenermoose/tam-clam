/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The syntax tree. Nodes live in the shell's arena and are freed with it
 * when the command finishes. */
#ifndef TAM_CLAM_AST_H
#define TAM_CLAM_AST_H

#include <stdbool.h>
#include <stddef.h>

struct tc_word {
	const char *text;  /* raw, quotes kept */
	size_t len;
	unsigned flags;    /* TC_WORD_* from lex.h */
	unsigned long line;
};

enum tc_redir_kind {
	TC_REDIR_IN,        /* < */
	TC_REDIR_OUT,       /* > */
	TC_REDIR_CLOBBER,   /* >| */
	TC_REDIR_APPEND,    /* >> */
	TC_REDIR_RDWR,      /* <> */
	TC_REDIR_DUP_IN,    /* <& */
	TC_REDIR_DUP_OUT,   /* >& */
	TC_REDIR_HEREDOC,   /* << and <<- */
};

struct tc_redir {
	enum tc_redir_kind kind;
	int fd;                    /* -1 for default (0 for input, 1 for output) */
	struct tc_word target;     /* file, dup fd/"-", or heredoc delimiter */
	const char *heredoc_body;  /* populated after NEWLINE for TC_REDIR_HEREDOC */
	size_t heredoc_len;
	bool strip_tabs;           /* <<- */
	bool heredoc_quoted;       /* delimiter had quotes */
	unsigned long line;
};

enum tc_andor_op {
	TC_ANDOR_FIRST = 0,
	TC_ANDOR_AND,       /* && */
	TC_ANDOR_OR,        /* || */
};

struct tc_andor_item {
	enum tc_andor_op op;
	struct tc_node *cmd;
};

enum tc_node_kind {
	TC_NODE_SIMPLE,    /* words and redirections */
	TC_NODE_PIPELINE,  /* !? cmd | cmd ... */
	TC_NODE_AND_OR,    /* pipeline (&& | ||) pipeline ... */
	TC_NODE_SEQ,       /* first ; rest */
};

struct tc_node {
	enum tc_node_kind kind;
	unsigned long line;
	union {
		struct {
			struct tc_word *words;
			size_t nwords;
			struct tc_redir *redirs;
			size_t nredirs;
		} simple;
		struct {
			struct tc_node **cmds;
			size_t ncmds;
			bool bang;
		} pipeline;
		struct {
			struct tc_andor_item *items;
			size_t nitems;
		} and_or;
		struct {
			struct tc_node *first, *rest;
		} seq;
	} u;
};

#endif
