/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The syntax tree. Nodes and word parts live in the shell's arena and are
 * freed with it when the command finishes. */
#ifndef TAM_CLAM_AST_H
#define TAM_CLAM_AST_H

#include <stdbool.h>
#include <stddef.h>

enum tc_wpart_kind {
	TC_WPART_LITERAL,     /* unquoted literal text */
	TC_WPART_QUOTED,      /* quoted literal text ('...', $'...', \x, or "..." text) */
	TC_WPART_PARAM,       /* $x or ${...} */
	TC_WPART_CMDSUB,      /* $(...) or `...` */
	TC_WPART_ARITH,       /* $((...)) */
	TC_WPART_PROCSUB_IN,  /* <(...) process substitution (read end) */
	TC_WPART_PROCSUB_OUT, /* >(...) process substitution (write end) */
};

enum tc_param_op {
	TC_PARAM_PLAIN = 0,       /* $x, ${x} */
	TC_PARAM_LEN,             /* ${#x}, ${#a[@]}, ${#a[i]} */
	TC_PARAM_KEYS,            /* ${!a[@]}, ${!a[*]} */
	TC_PARAM_USE_DEFAULT,     /* ${x-w}, ${x:-w} */
	TC_PARAM_ASSIGN_DEFAULT,  /* ${x=w}, ${x:=w} */
	TC_PARAM_ERROR,           /* ${x?w}, ${x:?w} */
	TC_PARAM_USE_ALT,         /* ${x+w}, ${x:+w} */
	TC_PARAM_TRIM_SUFFIX_S,   /* ${x%pat} */
	TC_PARAM_TRIM_SUFFIX_L,   /* ${x%%pat} */
	TC_PARAM_TRIM_PREFIX_S,   /* ${x#pat} */
	TC_PARAM_TRIM_PREFIX_L,   /* ${x##pat} */
};

enum tc_sub_kind {
	TC_SUB_NONE = 0,
	TC_SUB_EXPR,              /* [expr] or [key] */
	TC_SUB_AT,                /* [@] */
	TC_SUB_STAR,              /* [*] */
};

struct tc_node;
struct tc_assign;

struct tc_wpart {
	enum tc_wpart_kind kind;
	bool quoted;              /* inside "..." (for PARAM/CMDSUB/ARITH) */
	const char *text;         /* LITERAL/QUOTED text, or PARAM name */
	size_t len;
	enum tc_param_op op;      /* TC_WPART_PARAM only */
	bool colon;               /* TC_WPART_PARAM only: :- := :? :+ */
	enum tc_sub_kind sub_kind;/* TC_WPART_PARAM only: subscript kind */
	struct tc_wpart *sub;     /* TC_WPART_PARAM only: subscript parts when TC_SUB_EXPR */
	struct tc_wpart *arg;     /* PARAM arg or ARITH inner word parts */
	unsigned arg_flags;       /* TC_WPART_PARAM only: TC_WORD_* flags of arg */
	struct tc_node *cmd;      /* TC_WPART_CMDSUB only: parsed AST subtree */
	struct tc_wpart *next;
};

struct tc_word {
	const char *text;         /* raw text, for messages and reserved checks */
	size_t len;
	struct tc_wpart *parts;   /* structured parts for expansion */
	unsigned flags;           /* TC_WORD_* from lex.h */
	struct tc_assign *decl_assign; /* compound assign arg for declare/readonly/export */
	unsigned long line;
	unsigned long col;
};

struct tc_array_item {
	bool has_sub;             /* [key]=val or [key]+=val */
	bool append;              /* [key]+=val */
	struct tc_wpart *sub;     /* subscript parts when has_sub */
	struct tc_word value;     /* element word */
	unsigned long line;
	unsigned long col;
};

struct tc_assign {
	const char *name;
	bool append;              /* += instead of = */
	bool has_sub;             /* name[sub]=val or name[sub]+=val */
	struct tc_wpart *sub;     /* subscript parts when has_sub */
	bool is_compound;         /* name=(...) or name+=(...) */
	struct tc_word value;     /* scalar RHS when !is_compound */
	struct tc_array_item *items; /* compound items when is_compound */
	size_t nitems;
	unsigned long line;
	unsigned long col;
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

struct tc_if_branch {
	struct tc_node *cond;
	struct tc_node *body;
};

struct tc_case_item {
	struct tc_word *patterns;
	size_t npatterns;
	struct tc_node *body; /* NULL when the clause body is empty */
};

enum tc_cond_kind {
	TC_COND_OR,     /* left || right */
	TC_COND_AND,    /* left && right */
	TC_COND_NOT,    /* ! left */
	TC_COND_UNARY,  /* op word (e.g. -f x, -z x, -v x, -o opt) */
	TC_COND_BINARY, /* left_word op right_word (==, !=, =~, <, >, -eq, ...) */
	TC_COND_WORD,   /* bare word (non-empty test) */
};

struct tc_cond_expr {
	enum tc_cond_kind kind;
	const char *op;           /* operator string for UNARY / BINARY */
	struct tc_cond_expr *lhs; /* OR / AND / NOT */
	struct tc_cond_expr *rhs; /* OR / AND */
	struct tc_word w1;        /* UNARY / BINARY lhs / WORD */
	struct tc_word w2;        /* BINARY rhs */
	unsigned long line;
	unsigned long col;
};

enum tc_node_kind {
	TC_NODE_SIMPLE,    /* assignments, words, and redirections */
	TC_NODE_PIPELINE,  /* !? cmd | cmd ... */
	TC_NODE_AND_OR,    /* pipeline (&& | ||) pipeline ... */
	TC_NODE_SEQ,       /* first ; rest */
	TC_NODE_REDIR,     /* compound command with trailing redirections */
	TC_NODE_GROUP,     /* { compound_list; } */
	TC_NODE_SUBSHELL,  /* ( compound_list ) */
	TC_NODE_IF,        /* if/elif/else/fi */
	TC_NODE_LOOP,      /* while / until */
	TC_NODE_FOR,       /* for name [in words...] do body done */
	TC_NODE_CASE,      /* case word in items... esac */
	TC_NODE_FUNCDEF,   /* name() compound_command */
	TC_NODE_BG,        /* cmd & */
	TC_NODE_COND,      /* [[ expr ]] */
};

struct tc_node {
	enum tc_node_kind kind;
	unsigned long line;
	unsigned long col;
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
		struct {
			struct tc_node *child;
			struct tc_redir *redirs;
			size_t nredirs;
		} redir;
		struct {
			struct tc_node *body;
		} group;
		struct {
			struct tc_if_branch *branches;
			size_t nbranches;
			struct tc_node *else_body;
		} if_cmd;
		struct {
			bool until;
			struct tc_node *cond;
			struct tc_node *body;
		} loop;
		struct {
			const char *name;
			bool has_in;
			struct tc_word *words;
			size_t nwords;
			struct tc_node *body;
		} for_cmd;
		struct {
			struct tc_word word;
			struct tc_case_item *items;
			size_t nitems;
		} case_cmd;
		struct {
			const char *name;
			struct tc_node *body;
		} funcdef;
		struct {
			struct tc_node *child;
		} bg;
		struct {
			struct tc_cond_expr *expr;
		} cond;
	} u;
};

#endif
