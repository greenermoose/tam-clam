/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <signal.h>
#include <stddef.h>

#include "tam-clam/signals.h"

/* The only global state in tam-clam (see arch/principles.md). */
volatile sig_atomic_t tc_sigint_seen;
volatile sig_atomic_t tc_sig_pending[TC_NSIG];
volatile sig_atomic_t tc_sig_any;

static volatile sig_atomic_t sig_caught[TC_NSIG];
static volatile sig_atomic_t interactive_ign;

static void on_signal(int sig)
{
	if (sig > 0 && sig < TC_NSIG) {
		tc_sig_pending[sig] = 1;
		tc_sig_any = 1;
	}
	if (sig == SIGINT)
		tc_sigint_seen = 1;
}

int tc_signals_interactive(void)
{
	if (tc_signals_set_handler(SIGINT, TC_SIG_CATCH) < 0)
		return -1;
	if (tc_signals_set_handler(SIGQUIT, TC_SIG_IGN) < 0 ||
	    tc_signals_set_handler(SIGTERM, TC_SIG_IGN) < 0)
		return -1;
	interactive_ign = 1;
	return 0;
}

int tc_signals_set_handler(int sig, enum tc_sig_disp disp)
{
	if (sig <= 0 || sig >= TC_NSIG)
		return -1;
	struct sigaction sa = {0};
	sigemptyset(&sa.sa_mask);
	/* No SA_RESTART: blocking reads and waitpid must return EINTR. */
	switch (disp) {
	case TC_SIG_DFL:
		sa.sa_handler = SIG_DFL;
		break;
	case TC_SIG_IGN:
		sa.sa_handler = SIG_IGN;
		break;
	case TC_SIG_CATCH:
		sa.sa_handler = on_signal;
		break;
	}
	if (sigaction(sig, &sa, NULL) < 0)
		return -1;
	sig_caught[sig] = (disp == TC_SIG_CATCH) ? 1 : 0;
	if (disp != TC_SIG_CATCH)
		tc_sig_pending[sig] = 0;
	return 0;
}

bool tc_signals_query_ignored(int sig)
{
	if (sig <= 0 || sig >= TC_NSIG)
		return false;
	struct sigaction old;
	if (sigaction(sig, NULL, &old) < 0)
		return false;
	return old.sa_handler == SIG_IGN;
}

void tc_signals_clear_pending(void)
{
	tc_sigint_seen = 0;
	tc_sig_any = 0;
	for (int i = 0; i < TC_NSIG; i++)
		tc_sig_pending[i] = 0;
}

void tc_signals_child(void)
{
	struct sigaction sa = {0};
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = SIG_DFL;

	for (int sig = 1; sig < TC_NSIG; sig++) {
		if (sig_caught[sig]) {
			sigaction(sig, &sa, NULL);
			sig_caught[sig] = 0;
		}
	}
	if (interactive_ign) {
		sigaction(SIGQUIT, &sa, NULL);
		sigaction(SIGTERM, &sa, NULL);
		interactive_ign = 0;
	}
	tc_signals_clear_pending();
}

void tc_signals_bg_child(void)
{
	tc_signals_child();
	/* POSIX XCU 2.11: non-interactive asynchronous lists without job
	 * control ignore SIGINT and SIGQUIT. */
	struct sigaction sa = {0};
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = SIG_IGN;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
}
