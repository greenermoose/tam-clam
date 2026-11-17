/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <signal.h>
#include <stddef.h>

#include "tam-clam/signals.h"

/* The only global state in tam-clam (see arch/principles.md). */
volatile sig_atomic_t tc_sigint_seen;
static volatile sig_atomic_t tc_signals_changed;

static void on_sigint(int sig)
{
	(void)sig;
	tc_sigint_seen = 1;
}

int tc_signals_interactive(void)
{
	struct sigaction sa = {0};
	sigemptyset(&sa.sa_mask);
	/* No SA_RESTART: a read waiting at the prompt must return EINTR. */
	sa.sa_handler = on_sigint;
	if (sigaction(SIGINT, &sa, NULL) < 0)
		return -1;
	sa.sa_handler = SIG_IGN;
	if (sigaction(SIGQUIT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
		return -1;
	tc_signals_changed = 1;
	return 0;
}

void tc_signals_child(void)
{
	if (!tc_signals_changed)
		return;
	struct sigaction sa = {0};
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = SIG_DFL;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}
