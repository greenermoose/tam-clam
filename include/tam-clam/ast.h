/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The syntax tree. Nodes and word parts live in the shell's arena and are
 * freed with it when the command finishes. */
#ifndef TAM_CLAM_AST_H
#define TAM_CLAM_AST_H

#include <stdbool.h>
#include <stddef.h>

enum tc_wpart_kind {
	TC_WPART_LITERAL, /* unquoted literal text */
	TC_WPART_QUOTED,  /* quoted literal text ('...', \x, or "..." text) */
	TC_WPART_PARAM,   /* $x or ${...} */
};

enum tc_param_op {
	TC_PARAM_PLAIN = 0,       /* $x, ${x} */
	TC_PARAM_LEN,             /* ${#x} */
	TC_PARAM_USE_DEFAULT,     /* ${x-w}, ${x:-w} */
	TC_PARAM_ASSIGN_DEFAULT,  /* ${x=w}, ${x:=w} */
	TC_PARAM_ERROR,           /* ${x?w}, ${x:?w} */
	TC_PARAM_USE_ALT,         /* ${x+w}, ${x:+w} */
	TC_PARAM_TRIM_SUFFIX_S,   /* ${x%pat} */
	TC_PARAM_TRIM_SUFFIX_L,   /* ${x%%pat} */
	TC_PARAM_TRIM_PREFIX_S,   /* ${x#pat} */
	TC_PARAM_TRIM_PREFIX_L,   /* ${x##pat} */
};

struct tc_wpart {
	enum tc_wpart_kind kind;
	bool quoted;              /* inside "..." (for TC_WPART_PARAM) */
	const char *text;         /* LITERAL/QUOTED text, or PARAM name */
	size_t len;
	enum tc_param_op op;      /* TC_WPART_PARAM only */
	bool colon;               /* TC_WPART_PARAM only: :- := :? :+ */
	struct tc_wpart *arg;     /* TC_WPART_PARAM only: word parts for arg */
	unsigned arg_flags;       /* TC_WPART_PARAM only: TC_WORD_* flags of arg */
	struct tc_wpart *next;
};

struct tc_word {
	const char *text;         /* raw text, for messages and reserved checks */
	size_t len;
	struct tc_wpart *parts;   /* structured parts for expansion */
	unsigned flags;           /* TC_WORD_* from lex.h */
	unsigned long line;
};

struct tc_assign {
	const char *name;
	struct tc_word value;     /* word parts after '=' */
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
	const char *heredoc_body;  /* raw body for quoted heredoc */
	size_t heredoc_len;
	struct tc_wpart *heredoc_parts; /* parsed parts for unquoted heredoc */
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
	TC_NODE_SIMPLE,    /* assignments, words, and redirections */
	TC_NODE_PIPELINE,  /* !? cmd | cmd ... */
	TC_NODE_AND_OR,    /* pipeline (&& | ||) pipeline ... */
	TC_NODE_SEQ,       /* first ; rest */
};

struct tc_node {
	enum tc_node_kind kind;
	unsigned long line;
	union {
		struct {
			struct tc_assign *assigns;
			size_t nassigns;
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
