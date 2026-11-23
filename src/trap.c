/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Signal and EXIT trap table, safe-point dispatch, and the trap builtin. */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tam-clam/builtins.h"
#include "tam-clam/exec.h"
#include "tam-clam/shell.h"
#include "tam-clam/trap.h"

struct sig_map {
	int sig;
	const char *name;
};

static const struct sig_map sig_table[] = {
	{0, "EXIT"},
#ifdef SIGHUP
	{SIGHUP, "HUP"},
#endif
#ifdef SIGINT
	{SIGINT, "INT"},
#endif
#ifdef SIGQUIT
	{SIGQUIT, "QUIT"},
#endif
#ifdef SIGILL
	{SIGILL, "ILL"},
#endif
#ifdef SIGTRAP
	{SIGTRAP, "TRAP"},
#endif
#ifdef SIGABRT
	{SIGABRT, "ABRT"},
#endif
#ifdef SIGBUS
	{SIGBUS, "BUS"},
#endif
#ifdef SIGFPE
	{SIGFPE, "FPE"},
#endif
#ifdef SIGKILL
	{SIGKILL, "KILL"},
#endif
#ifdef SIGUSR1
	{SIGUSR1, "USR1"},
#endif
#ifdef SIGSEGV
	{SIGSEGV, "SEGV"},
#endif
#ifdef SIGUSR2
	{SIGUSR2, "USR2"},
#endif
#ifdef SIGPIPE
	{SIGPIPE, "PIPE"},
#endif
#ifdef SIGALRM
	{SIGALRM, "ALRM"},
#endif
#ifdef SIGTERM
	{SIGTERM, "TERM"},
#endif
#ifdef SIGCHLD
	{SIGCHLD, "CHLD"},
#endif
#ifdef SIGCONT
	{SIGCONT, "CONT"},
#endif
#ifdef SIGSTOP
	{SIGSTOP, "STOP"},
#endif
#ifdef SIGTSTP
	{SIGTSTP, "TSTP"},
#endif
#ifdef SIGTTIN
	{SIGTTIN, "TTIN"},
#endif
#ifdef SIGTTOU
	{SIGTTOU, "TTOU"},
#endif
#ifdef SIGURG
	{SIGURG, "URG"},
#endif
#ifdef SIGXCPU
	{SIGXCPU, "XCPU"},
#endif
#ifdef SIGXFSZ
	{SIGXFSZ, "XFSZ"},
#endif
#ifdef SIGVTALRM
	{SIGVTALRM, "VTALRM"},
#endif
#ifdef SIGPROF
	{SIGPROF, "PROF"},
#endif
#ifdef SIGWINCH
	{SIGWINCH, "WINCH"},
#endif
#ifdef SIGSYS
	{SIGSYS, "SYS"},
#endif
};

static bool str_eq_ci(const char *a, const char *b)
{
	while (*a && *b) {
		if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
			return false;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

static bool is_unsigned_decimal(const char *s, int *out)
{
	if (!s || *s == '\0')
		return false;
	unsigned long v = 0;
	for (const char *p = s; *p; p++) {
		if (*p < '0' || *p > '9')
			return false;
		v = v * 10 + (unsigned long)(*p - '0');
		if (v >= TC_NSIG)
			return false;
	}
	if (out)
		*out = (int)v;
	return true;
}

int tc_trap_parse_sig(const char *spec)
{
	if (!spec || *spec == '\0')
		return -1;

	int num = -1;
	if (is_unsigned_decimal(spec, &num)) {
		if (num == 0)
			return 0;
		for (size_t i = 0; i < sizeof(sig_table) / sizeof(sig_table[0]); i++) {
			if (sig_table[i].sig == num)
				return num;
		}
		return (num > 0 && num < TC_NSIG) ? num : -1;
	}

	const char *name = spec;
	if ((name[0] == 'S' || name[0] == 's') &&
	    (name[1] == 'I' || name[1] == 'i') &&
	    (name[2] == 'G' || name[2] == 'g'))
		name += 3;

	for (size_t i = 0; i < sizeof(sig_table) / sizeof(sig_table[0]); i++) {
		if (str_eq_ci(name, sig_table[i].name))
			return sig_table[i].sig;
	}
	return -1;
}

const char *tc_trap_sig_name(int sig)
{
	for (size_t i = 0; i < sizeof(sig_table) / sizeof(sig_table[0]); i++) {
		if (sig_table[i].sig == sig)
			return sig_table[i].name;
	}
	return NULL;
}

void tc_traps_init(struct tc_traps *t, bool interactive)
{
	memset(t, 0, sizeof(*t));
	if (!interactive) {
		for (int sig = 1; sig < TC_NSIG; sig++) {
			if (tc_signals_query_ignored(sig))
				t->entries[sig].inherited_ign = true;
		}
	}
}

void tc_traps_finish(struct tc_traps *t)
{
	for (int sig = 0; sig < TC_NSIG; sig++) {
		free(t->entries[sig].action);
		t->entries[sig].action = NULL;
	}
	t->subshell_saved = false;
}

static void clear_subshell_saved(struct tc_traps *t)
{
	if (!t->subshell_saved)
		return;
	/* Clear inherited EXIT and caught traps; keep explicit ignores (""). */
	free(t->entries[0].action);
	t->entries[0].action = NULL;
	for (int sig = 1; sig < TC_NSIG; sig++) {
		if (t->entries[sig].action && t->entries[sig].action[0] != '\0') {
			free(t->entries[sig].action);
			t->entries[sig].action = NULL;
		}
	}
	t->subshell_saved = false;
}

void tc_traps_subshell_reset(struct tc_traps *t)
{
	/* Caught OS handlers are reset by tc_signals_child(); keep strings
	 * marked subshell_saved so `saved=$(trap)` sees parent traps. */
	t->subshell_saved = true;
}

bool tc_trap_is_caught(const struct tc_traps *t, int sig)
{
	if (t->subshell_saved || sig <= 0 || sig >= TC_NSIG)
		return false;
	return t->entries[sig].action && t->entries[sig].action[0] != '\0';
}

static char *dup_action(const char *s)
{
	size_t len = strlen(s);
	char *p = malloc(len + 1);
	if (p)
		memcpy(p, s, len + 1);
	return p;
}

static void run_trap_action(struct tc_shell *sh, const char *action)
{
	char *copy = dup_action(action);
	if (!copy)
		return;
	int saved_status = sh->status;
	sh->trap_depth++;
	bool ran_any = false;
	(void)tc_exec_string(sh, copy, strlen(copy), 0, &ran_any);
	sh->trap_depth--;
	free(copy);
	if (!sh->exit_requested && sh->flow == TC_FLOW_NONE)
		sh->status = saved_status;
}

void tc_trap_run_pending(struct tc_shell *sh)
{
	if (!tc_sig_any || sh->traps.subshell_saved)
		return;
	if (sh->trap_depth >= TC_TRAP_DEPTH_MAX)
		return;

	tc_sig_any = 0;
	for (int sig = 1; sig < TC_NSIG; sig++) {
		if (!tc_sig_pending[sig])
			continue;
		tc_sig_pending[sig] = 0;
		const char *action = sh->traps.entries[sig].action;
		if (action && action[0] != '\0') {
			run_trap_action(sh, action);
			if (sh->exit_requested || sh->flow != TC_FLOW_NONE)
				break;
		}
	}
}

void tc_trap_run_exit(struct tc_shell *sh)
{
	if (sh->in_exit_trap || sh->traps.subshell_saved)
		return;
	const char *action = sh->traps.entries[0].action;
	if (!action || action[0] == '\0')
		return;

	char *copy = dup_action(action);
	if (!copy)
		return;

	sh->in_exit_trap = true;
	int orig_status = sh->exit_requested ? sh->exit_status : sh->status;
	sh->status = orig_status;
	sh->exit_requested = false;
	sh->flow = TC_FLOW_NONE;

	bool ran_any = false;
	(void)tc_exec_string(sh, copy, strlen(copy), 0, &ran_any);
	free(copy);

	if (!sh->exit_requested) {
		sh->exit_requested = true;
		sh->exit_status = orig_status;
	}
	sh->status = sh->exit_status;
}

static int print_one_trap(const struct tc_traps *t, int sig)
{
	const char *action = t->entries[sig].action;
	if (!action)
		return 0;
	const char *name = tc_trap_sig_name(sig);
	char numbuf[16];
	if (!name) {
		snprintf(numbuf, sizeof(numbuf), "%d", sig);
		name = numbuf;
	}
	if (tc_write_all(STDOUT_FILENO, "trap -- ", 8) < 0 ||
	    tc_write_shell_quoted(STDOUT_FILENO, action) < 0 ||
	    tc_write_all(STDOUT_FILENO, " ", 1) < 0 ||
	    tc_write_all(STDOUT_FILENO, name, strlen(name)) < 0 ||
	    tc_write_all(STDOUT_FILENO, "\n", 1) < 0)
		return -1;
	return 0;
}

static int trap_special_fail(struct tc_shell *sh, int status)
{
	if (!sh->interactive && !sh->in_command_builtin) {
		sh->exit_requested = true;
		sh->exit_status = status;
	}
	return status;
}

static int set_one_trap(struct tc_shell *sh, int sig, const char *action,
			unsigned long line)
{
#ifdef SIGKILL
	if (sig == SIGKILL) {
		tc_error(sh, line, "trap: cannot trap KILL");
		return 1;
	}
#endif
#ifdef SIGSTOP
	if (sig == SIGSTOP) {
		tc_error(sh, line, "trap: cannot trap STOP");
		return 1;
	}
#endif

	struct tc_trap_entry *e = &sh->traps.entries[sig];
	/* POSIX: signals ignored on entry to a non-interactive shell cannot
	 * be trapped or reset. */
	if (sig > 0 && !sh->interactive && e->inherited_ign)
		return 0;

	if (!action) {
		/* Reset to default. */
		if (sig > 0) {
			enum tc_sig_disp d = TC_SIG_DFL;
			if (sh->interactive && sig == SIGINT)
				d = TC_SIG_CATCH;
			else if (sh->interactive && (sig == SIGQUIT || sig == SIGTERM))
				d = TC_SIG_IGN;
			if (tc_signals_set_handler(sig, d) < 0) {
				tc_error(sh, line, "trap: %d: %s", sig, strerror(errno));
				return 1;
			}
		}
		free(e->action);
		e->action = NULL;
		return 0;
	}

	char *copy = dup_action(action);
	if (!copy) {
		tc_error(sh, line, "trap: out of memory");
		return 1;
	}
	if (sig > 0) {
		enum tc_sig_disp d = (action[0] == '\0') ? TC_SIG_IGN : TC_SIG_CATCH;
		if (tc_signals_set_handler(sig, d) < 0) {
			free(copy);
			tc_error(sh, line, "trap: %d: %s", sig, strerror(errno));
			return 1;
		}
	}
	free(e->action);
	e->action = copy;
	return 0;
}

int tc_bi_trap(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	int i = 1;
	bool print_mode = false;
	for (; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		if (strcmp(argv[i], "-p") == 0) {
			print_mode = true;
			continue;
		}
		tc_error(sh, line, "trap: %s: invalid option", argv[i]);
		return trap_special_fail(sh, TC_STATUS_USAGE);
	}

	if (print_mode || i >= argc) {
		if (i >= argc) {
			for (int sig = 0; sig < TC_NSIG; sig++) {
				if (print_one_trap(&sh->traps, sig) < 0) {
					tc_error(sh, line, "trap: write error: %s",
						 strerror(errno));
					return 1;
				}
			}
			return 0;
		}
		int status = 0;
		for (; i < argc; i++) {
			int sig = tc_trap_parse_sig(argv[i]);
			if (sig < 0) {
				tc_error(sh, line, "trap: %s: invalid signal", argv[i]);
				status = 1;
				continue;
			}
			if (print_one_trap(&sh->traps, sig) < 0) {
				tc_error(sh, line, "trap: write error: %s",
					 strerror(errno));
				return 1;
			}
		}
		return status ? trap_special_fail(sh, status) : 0;
	}

	clear_subshell_saved(&sh->traps);

	/* POSIX: if the first operand is an unsigned decimal integer (or if
	 * there is only one operand), reset all listed conditions. */
	const char *action = NULL;
	int first_cond = i;
	if (argc - i == 1 || is_unsigned_decimal(argv[i], NULL)) {
		action = NULL;
		first_cond = i;
	} else {
		action = strcmp(argv[i], "-") == 0 ? NULL : argv[i];
		first_cond = i + 1;
	}

	int status = 0;
	for (int j = first_cond; j < argc; j++) {
		int sig = tc_trap_parse_sig(argv[j]);
		if (sig < 0) {
			tc_error(sh, line, "trap: %s: invalid signal", argv[j]);
			status = 1;
			continue;
		}
		if (set_one_trap(sh, sig, action, line) != 0)
			status = 1;
	}
	return status ? trap_special_fail(sh, status) : 0;
}
