/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <signal.h>
#include <string.h>

#include "check.h"
#include "tam-clam/shell.h"
#include "tam-clam/trap.h"

int main(void)
{
	/* Signal name and number parsing. */
	CHECK(tc_trap_parse_sig("EXIT") == 0);
	CHECK(tc_trap_parse_sig("exit") == 0);
	CHECK(tc_trap_parse_sig("0") == 0);
	CHECK(tc_trap_parse_sig("INT") == SIGINT);
	CHECK(tc_trap_parse_sig("SIGINT") == SIGINT);
	CHECK(tc_trap_parse_sig("sigint") == SIGINT);
	CHECK(tc_trap_parse_sig("USR1") == SIGUSR1);
	CHECK(tc_trap_parse_sig("SIGUSR1") == SIGUSR1);
	CHECK(tc_trap_parse_sig("TERM") == SIGTERM);
	CHECK(tc_trap_parse_sig("HUP") == SIGHUP);
	CHECK(tc_trap_parse_sig("NOSUCH") == -1);
	CHECK(tc_trap_parse_sig("") == -1);
	CHECK(tc_trap_parse_sig("-1") == -1);
	CHECK(tc_trap_parse_sig("999") == -1);

	/* Canonical names. */
	CHECK(strcmp(tc_trap_sig_name(0), "EXIT") == 0);
	CHECK(strcmp(tc_trap_sig_name(SIGINT), "INT") == 0);
	CHECK(strcmp(tc_trap_sig_name(SIGUSR1), "USR1") == 0);

	/* Trap table lifecycle and subshell reset. */
	struct tc_shell sh;
	tc_shell_init(&sh, "test");
	sh.interactive = true;

	char *set_argv[] = {"trap", "echo caught", "USR1", "EXIT", NULL};
	CHECK(tc_bi_trap(&sh, 4, set_argv, 1) == 0);
	CHECK(tc_trap_is_caught(&sh.traps, SIGUSR1));
	CHECK(sh.traps.entries[0].action != NULL &&
	      strcmp(sh.traps.entries[0].action, "echo caught") == 0);

	/* Subshell reset preserves display strings until modified, but marks
	 * them inactive for dispatch. */
	tc_traps_subshell_reset(&sh.traps);
	CHECK(sh.traps.subshell_saved);
	CHECK(!tc_trap_is_caught(&sh.traps, SIGUSR1));
	CHECK(sh.traps.entries[SIGUSR1].action != NULL);

	/* Modifying a trap in subshell clears inherited parent traps first. */
	char *set2_argv[] = {"trap", "echo new", "USR2", NULL};
	CHECK(tc_bi_trap(&sh, 3, set2_argv, 2) == 0);
	CHECK(!sh.traps.subshell_saved);
	CHECK(sh.traps.entries[0].action == NULL);
	CHECK(sh.traps.entries[SIGUSR1].action == NULL);
	CHECK(tc_trap_is_caught(&sh.traps, SIGUSR2));

	/* Resetting USR2 via numeric-first or '-' clears it. */
	char *rst_argv[] = {"trap", "-", "USR2", NULL};
	CHECK(tc_bi_trap(&sh, 3, rst_argv, 3) == 0);
	CHECK(!tc_trap_is_caught(&sh.traps, SIGUSR2));
	CHECK(sh.traps.entries[SIGUSR2].action == NULL);

	/* Trapping KILL/STOP fails. */
	char *kill_argv[] = {"trap", "echo no", "KILL", NULL};
	CHECK(tc_bi_trap(&sh, 3, kill_argv, 4) != 0);

	tc_shell_finish(&sh);
	CHECK_DONE("test_trap");
}
