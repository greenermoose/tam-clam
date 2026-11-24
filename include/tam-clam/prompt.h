/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Safe prompt escape expansion (PS1/PS2/RPS1/PS0) and precmd/preexec hooks
 * (0.2.5). Prompts are never re-parsed as code, and clock escapes evaluate
 * only when the prompt is drawn (no background timers or polling). */
#ifndef TAM_CLAM_PROMPT_H
#define TAM_CLAM_PROMPT_H

struct tc_shell;

#define TC_PROMPT_MAX 4096

/* Expand prompt escapes in raw into a heap-allocated NUL-terminated string.
 * Caller frees. Returns NULL on OOM. */
char *tc_prompt_expand(const struct tc_shell *sh, const char *raw);

/* Run precmd function (if defined) and PROMPT_COMMAND (if set) before PS1,
 * preserving sh->status and guarding against recursion. */
void tc_prompt_run_precmd(struct tc_shell *sh);

/* Run preexec function with $1=cmd (if defined) and print expanded PS0
 * (if set) to stderr before executing an interactive command. */
void tc_prompt_run_preexec(struct tc_shell *sh, const char *cmd);

#endif
