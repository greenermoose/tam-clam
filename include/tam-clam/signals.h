/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Signal handling. Handlers only set a flag; the shell acts on the flag
 * at a safe point. These flags are the only global state in tam-clam. */
#ifndef TAM_CLAM_SIGNALS_H
#define TAM_CLAM_SIGNALS_H

#include <signal.h>

extern volatile sig_atomic_t tc_sigint_seen;

/* Interactive shells catch SIGINT and ignore SIGQUIT and SIGTERM. */
int tc_signals_interactive(void);
/* In a child, before exec: undo what tc_signals_interactive changed. */
void tc_signals_child(void);

#endif
