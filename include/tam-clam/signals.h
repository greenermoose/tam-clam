/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Signal handling. Handlers only set flags; the shell acts on them at safe
 * points. These flags are the only global state in tam-clam. */
#ifndef TAM_CLAM_SIGNALS_H
#define TAM_CLAM_SIGNALS_H

#include <signal.h>
#include <stdbool.h>

/* Slot 0 is EXIT; OS signals occupy 1..TC_NSIG-1. */
#define TC_NSIG 64

enum tc_sig_disp {
	TC_SIG_DFL = 0,
	TC_SIG_IGN,
	TC_SIG_CATCH,
};

extern volatile sig_atomic_t tc_sigint_seen;
extern volatile sig_atomic_t tc_sigwinch_seen;
extern volatile sig_atomic_t tc_sig_pending[TC_NSIG];
extern volatile sig_atomic_t tc_sig_any;

/* Interactive shells catch SIGINT and ignore SIGQUIT and SIGTERM. */
int tc_signals_interactive(void);
/* Set disposition for an OS signal (1..TC_NSIG-1). 0 or -1. */
int tc_signals_set_handler(int sig, enum tc_sig_disp disp);
/* True if sig is currently SIG_IGN in the OS. */
bool tc_signals_query_ignored(int sig);
/* Clear all pending signal flags (used in forked children). */
void tc_signals_clear_pending(void);
/* In a child, before exec: reset caught signals to SIG_DFL; keep SIG_IGN
 * except interactive-ignored SIGQUIT/SIGTERM. */
void tc_signals_child(void);
/* In a background child (&) when job control is off: reset caught signals,
 * and ignore SIGINT and SIGQUIT per POSIX XCU 2.11. */
void tc_signals_bg_child(void);

#endif
