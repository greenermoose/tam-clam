/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The syntax tree. Nodes live in the shell's arena and are freed with it
 * when the command finishes. Later versions add pipelines, and-or lists,
 * redirections and compound commands as new node kinds. */
#ifndef TAM_CLAM_AST_H
#define TAM_CLAM_AST_H

#include <stddef.h>

struct tc_word {
	const char *text;  /* raw, quotes kept */
	size_t len;
	unsigned flags;    /* TC_WORD_* from lex.h */
	unsigned long line;
};

enum tc_node_kind {
	TC_NODE_SIMPLE,    /* words */
	TC_NODE_SEQ,       /* first ; rest */
};

struct tc_node {
	enum tc_node_kind kind;
	unsigned long line;
	union {
		struct {
			struct tc_word *words;
			size_t nwords;
		} simple;
		struct {
			struct tc_node *first, *rest;
		} seq;
	} u;
};

#endif
