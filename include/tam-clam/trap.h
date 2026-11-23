/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Signal and EXIT trap table, safe-point dispatch, and the trap builtin. */
#ifndef TAM_CLAM_TRAP_H
#define TAM_CLAM_TRAP_H

#include <stdbool.h>

#include "tam-clam/signals.h"

struct tc_shell;

#define TC_TRAP_DEPTH_MAX 64

struct tc_trap_entry {
	char *action;        /* NULL = default, "" = ignore, non-empty = eval */
	bool inherited_ign;  /* was SIG_IGN on entry to non-interactive shell */
};

struct tc_traps {
	struct tc_trap_entry entries[TC_NSIG];
	/* POSIX $(trap): in a fresh subshell, display parent's traps until
	 * the subshell modifies any trap, even though OS handlers were reset. */
	bool subshell_saved;
};

void tc_traps_init(struct tc_traps *t, bool interactive);
void tc_traps_finish(struct tc_traps *t);

/* Reset caught traps in a forked subshell (OS handlers to SIG_DFL, while
 * keeping display strings for $(trap) until modified). */
void tc_traps_subshell_reset(struct tc_traps *t);

/* Parse a POSIX signal name (with/without SIG, case-insensitive) or number.
 * Returns 0 for EXIT, 1..TC_NSIG-1 for OS signals, or -1 on error. */
int tc_trap_parse_sig(const char *spec);

/* Canonical POSIX name (without SIG prefix, e.g. "EXIT", "INT", "USR1"),
 * or NULL if unknown. */
const char *tc_trap_sig_name(int sig);

/* True if signal sig has a non-empty trap action registered. */
bool tc_trap_is_caught(const struct tc_traps *t, int sig);

/* Dispatch any pending trapped signals at a safe point. */
void tc_trap_run_pending(struct tc_shell *sh);

/* Run the EXIT trap (if set and not already running). Updates sh->exit_status
 * only if the trap itself invokes exit. */
void tc_trap_run_exit(struct tc_shell *sh);

/* The POSIX special builtin `trap`. */
int tc_bi_trap(struct tc_shell *sh, int argc, char **argv, unsigned long line);

#endif
