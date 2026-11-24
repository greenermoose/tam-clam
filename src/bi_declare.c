/* SPDX-License-Identifier: GPL-3.0-or-later */
/* declare / typeset builtin and compound assignment execution for
 * declaration builtins (declare, typeset, readonly, export). */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tam-clam/arith.h"
#include "tam-clam/builtins.h"
#include "tam-clam/exec.h"
#include "tam-clam/expand.h"
#include "tam-clam/shell.h"
#include "tam-clam/var.h"

int tc_write_array_value(int fd, const struct tc_var_entry *e)
{
	if (tc_write_all(fd, "(", 1) < 0)
		return -1;
	if (e->flags & TC_VAR_ARRAY) {
		if (e->arr) {
			for (size_t i = 0; i < e->arr->len; i++) {
				char hdr[40];
				int n = snprintf(hdr, sizeof(hdr), "%s[%" PRId64 "]=",
						 i > 0 ? " " : "",
						 e->arr->elems[i].idx);
				if (n < 0 || tc_write_all(fd, hdr, (size_t)n) < 0 ||
				    tc_write_shell_quoted(fd, e->arr->elems[i].val) < 0)
					return -1;
			}
		}
	} else if (e->flags & TC_VAR_ASSOC) {
		const struct tc_assoc_slot **slots = NULL;
		size_t n = 0;
		if (e->assoc && tc_assoc_list(e->assoc, &slots, &n) != TC_ARR_OK)
			return -1;
		for (size_t i = 0; i < n; i++) {
			if ((i > 0 && tc_write_all(fd, " ", 1) < 0) ||
			    tc_write_all(fd, "[", 1) < 0 ||
			    tc_write_shell_quoted(fd, slots[i]->key) < 0 ||
			    tc_write_all(fd, "]=", 2) < 0 ||
			    tc_write_shell_quoted(fd, slots[i]->val) < 0) {
				free(slots);
				return -1;
			}
		}
		free(slots);
	}
	return tc_write_all(fd, ")", 1);
}

static int print_declare_entry(const struct tc_var_entry *e)
{
	char flags[8];
	size_t fl = 0;
	if (e->flags & TC_VAR_ARRAY)
		flags[fl++] = 'a';
	if (e->flags & TC_VAR_ASSOC)
		flags[fl++] = 'A';
	if (e->flags & TC_VAR_READONLY)
		flags[fl++] = 'r';
	if (e->flags & TC_VAR_EXPORT)
		flags[fl++] = 'x';
	if (fl == 0)
		flags[fl++] = '-';
	flags[fl] = '\0';

	if (tc_write_all(STDOUT_FILENO, "declare -", 9) < 0 ||
	    tc_write_all(STDOUT_FILENO, flags, fl) < 0 ||
	    tc_write_all(STDOUT_FILENO, " ", 1) < 0 ||
	    tc_write_all(STDOUT_FILENO, e->name, strlen(e->name)) < 0)
		return -1;
	if (e->flags & (TC_VAR_ARRAY | TC_VAR_ASSOC)) {
		if (tc_write_all(STDOUT_FILENO, "=", 1) < 0 ||
		    tc_write_array_value(STDOUT_FILENO, e) < 0)
			return -1;
	} else if (e->value != NULL) {
		if (tc_write_all(STDOUT_FILENO, "=", 1) < 0 ||
		    tc_write_shell_quoted(STDOUT_FILENO, e->value) < 0)
			return -1;
	}
	return tc_write_all(STDOUT_FILENO, "\n", 1);
}

static int print_declare_all(struct tc_shell *sh, unsigned filter,
			     const char *cmd, unsigned long line)
{
	const struct tc_var_entry **list = NULL;
	size_t n = 0;
	if (tc_var_list(&sh->vars, filter, &list, &n) != TC_VAR_OK) {
		tc_error(sh, line, "%s: out of memory", cmd);
		return 1;
	}
	int status = 0;
	for (size_t i = 0; i < n; i++) {
		if (print_declare_entry(list[i]) < 0) {
			tc_error(sh, line, "%s: write error: %s", cmd, strerror(errno));
			status = 1;
			break;
		}
	}
	free(list);
	return status;
}

static int apply_declare_scalar_arg(struct tc_shell *sh, const char *arg,
				    unsigned add_flags, unsigned clear_flags,
				    const char *cmd, unsigned long line)
{
	/* Check for NAME=val, NAME+=val, or NAME. */
	const char *eq = strchr(arg, '=');
	bool append = false;
	size_t nlen = eq ? (size_t)(eq - arg) : strlen(arg);
	if (eq && nlen > 0 && arg[nlen - 1] == '+') {
		append = true;
		nlen--;
	}

	if (!tc_var_valid_name(arg, nlen)) {
		tc_error(sh, line, "%s: `%s': not a valid identifier", cmd, arg);
		return 1;
	}

	char *name = malloc(nlen + 1);
	if (!name) {
		tc_error(sh, line, "%s: out of memory", cmd);
		return 1;
	}
	memcpy(name, arg, nlen);
	name[nlen] = '\0';

	int r = tc_var_declare(&sh->vars, name, add_flags, clear_flags);
	if (r == TC_VAR_ERR_TYPE) {
		tc_error(sh, line, "%s: %s: cannot convert between indexed and associative array",
			 cmd, name);
		free(name);
		return 1;
	}
	if (r == TC_VAR_ERR_READONLY) {
		tc_error(sh, line, "%s: %s: readonly variable", cmd, name);
		free(name);
		return 1;
	}
	if (r != TC_VAR_OK) {
		tc_error(sh, line, "%s: out of memory", cmd);
		free(name);
		return 1;
	}

	if (eq) {
		const char *val = eq + 1;
		unsigned set_fl = (add_flags & ~(TC_VAR_ARRAY | TC_VAR_ASSOC | TC_VAR_READONLY));
		if (sh->opt_allexport)
			set_fl |= TC_VAR_EXPORT;
		/* If -r was passed along with NAME=val, assign first, then mark readonly. */
		if (append)
			r = tc_var_append_scalar(&sh->vars, name, val, set_fl);
		else
			r = tc_var_set(&sh->vars, name, val, set_fl);
		if (r == TC_VAR_ERR_READONLY) {
			tc_error(sh, line, "%s: %s: readonly variable", cmd, name);
			free(name);
			return 1;
		}
		if (r != TC_VAR_OK) {
			tc_error(sh, line, "%s: out of memory", cmd);
			free(name);
			return 1;
		}
		if (add_flags & TC_VAR_READONLY)
			(void)tc_var_declare(&sh->vars, name, TC_VAR_READONLY, 0);
	}
	free(name);
	return 0;
}

static int parse_declare_flag(struct tc_shell *sh, const char *cmd,
			      unsigned long line, char sign, char opt,
			      unsigned *add_flags, unsigned *clear_flags,
			      bool *print_mode)
{
	bool enable = (sign == '-');
	switch (opt) {
	case 'a':
		if (enable)
			*add_flags |= TC_VAR_ARRAY;
		else
			*clear_flags |= TC_VAR_ARRAY;
		return 0;
	case 'A':
		if (enable)
			*add_flags |= TC_VAR_ASSOC;
		else
			*clear_flags |= TC_VAR_ASSOC;
		return 0;
	case 'r':
		if (enable)
			*add_flags |= TC_VAR_READONLY;
		else
			*clear_flags |= TC_VAR_READONLY;
		return 0;
	case 'x':
		if (enable)
			*add_flags |= TC_VAR_EXPORT;
		else
			*clear_flags |= TC_VAR_EXPORT;
		return 0;
	case 'g':
		/* Global scope (no-op until local scopes arrive in 0.3.1). */
		return 0;
	case 'p':
		if (enable)
			*print_mode = true;
		return 0;
	case 'i': case 'n': case 'l': case 'u': case 'f': case 'F': case 't':
		tc_error(sh, line, "%s: %c%c: option not supported yet; see ROADMAP.md",
			 cmd, sign, opt);
		return TC_STATUS_USAGE;
	default:
		tc_error(sh, line, "%s: %c%c: invalid option", cmd, sign, opt);
		return TC_STATUS_USAGE;
	}
}

int tc_bi_declare(struct tc_shell *sh, int argc, char **argv, unsigned long line)
{
	const char *cmd = argv[0];
	unsigned add_flags = 0, clear_flags = 0;
	bool print_mode = false;
	int i = 1;

	for (; i < argc && (argv[i][0] == '-' || argv[i][0] == '+') && argv[i][1] != '\0'; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		char sign = argv[i][0];
		for (const char *p = argv[i] + 1; *p; p++) {
			int fr = parse_declare_flag(sh, cmd, line, sign, *p,
						    &add_flags, &clear_flags, &print_mode);
			if (fr != 0)
				return fr;
		}
	}

	if ((add_flags & TC_VAR_ARRAY) && (add_flags & TC_VAR_ASSOC)) {
		tc_error(sh, line, "%s: cannot use -a and -A together", cmd);
		return TC_STATUS_USAGE;
	}

	if (print_mode) {
		if (i >= argc)
			return print_declare_all(sh, add_flags, cmd, line);
		int status = 0;
		for (; i < argc; i++) {
			const struct tc_var_entry *e = tc_var_find(&sh->vars, argv[i]);
			if (!e) {
				tc_error(sh, line, "%s: %s: not found", cmd, argv[i]);
				status = 1;
				continue;
			}
			if (print_declare_entry(e) < 0) {
				tc_error(sh, line, "%s: write error: %s", cmd, strerror(errno));
				return 1;
			}
		}
		return status;
	}

	if (i >= argc)
		return print_declare_all(sh, add_flags, cmd, line);

	int status = 0;
	for (; i < argc; i++) {
		int ar = apply_declare_scalar_arg(sh, argv[i], add_flags, clear_flags,
						  cmd, line);
		if (ar != 0)
			status = ar;
	}
	return status;
}

/* Execute a declaration builtin (declare/typeset/readonly/export) when one or
 * more AST words carry a compound decl_assign (e.g. declare -A m=([k]=v)). */
int tc_exec_decl_builtin(struct tc_shell *sh, const char *cmd,
			 const struct tc_word *words, size_t nwords,
			 unsigned long line)
{
	bool is_ro = (strcmp(cmd, "readonly") == 0);
	bool is_exp = (strcmp(cmd, "export") == 0);
	unsigned add_flags = is_ro ? TC_VAR_READONLY : (is_exp ? TC_VAR_EXPORT : 0);
	unsigned clear_flags = 0;
	bool print_mode = false;
	bool opts_done = false;
	int status = 0;

	for (size_t i = 1; i < nwords; i++) {
		if (words[i].decl_assign) {
			opts_done = true;
			const struct tc_assign *a = words[i].decl_assign;
			unsigned pre_flags = add_flags & ~TC_VAR_READONLY;
			if (pre_flags || clear_flags) {
				int dr = tc_var_declare(&sh->vars, a->name, pre_flags, clear_flags);
				if (dr == TC_VAR_ERR_TYPE) {
					tc_error(sh, a->line,
						 "%s: %s: cannot convert between indexed and associative array",
						 cmd, a->name);
					status = 1;
					continue;
				}
				if (dr == TC_VAR_ERR_READONLY) {
					tc_error(sh, a->line, "%s: %s: readonly variable", cmd, a->name);
					status = 1;
					continue;
				}
				if (dr != TC_VAR_OK) {
					tc_error(sh, a->line, "%s: out of memory", cmd);
					return 1;
				}
			}
			int ar = tc_exec_apply_assign(sh, a, pre_flags);
			if (ar != 0) {
				status = ar;
				continue;
			}
			if (add_flags & TC_VAR_READONLY)
				(void)tc_var_declare(&sh->vars, a->name, TC_VAR_READONLY, 0);
			continue;
		}

		char **expv = NULL;
		size_t expc = 0;
		int er = tc_expand_words(sh, &words[i], 1, &expv, &expc);
		if (er != TC_EXP_OK)
			return er == TC_EXP_ERR ? 1 : TC_STATUS_USAGE;

		for (size_t k = 0; k < expc; k++) {
			const char *arg = expv[k];
			if (!opts_done && (arg[0] == '-' || arg[0] == '+') && arg[1] != '\0') {
				if (strcmp(arg, "--") == 0) {
					opts_done = true;
					continue;
				}
				if (is_ro || is_exp) {
					if (strcmp(arg, "-p") == 0) {
						print_mode = true;
						continue;
					}
					tc_error(sh, line, "%s: %s: invalid option", cmd, arg);
					return TC_STATUS_USAGE;
				}
				for (const char *p = arg + 1; *p; p++) {
					int fr = parse_declare_flag(sh, cmd, line, arg[0], *p,
								    &add_flags, &clear_flags,
								    &print_mode);
					if (fr != 0)
						return fr;
				}
				if ((add_flags & TC_VAR_ARRAY) && (add_flags & TC_VAR_ASSOC)) {
					tc_error(sh, line, "%s: cannot use -a and -A together", cmd);
					return TC_STATUS_USAGE;
				}
				continue;
			}
			opts_done = true;
			if (print_mode) {
				const struct tc_var_entry *e = tc_var_find(&sh->vars, arg);
				if (!e) {
					tc_error(sh, line, "%s: %s: not found", cmd, arg);
					status = 1;
				} else if (print_declare_entry(e) < 0) {
					tc_error(sh, line, "%s: write error: %s", cmd, strerror(errno));
					return 1;
				}
			} else {
				int sr = apply_declare_scalar_arg(sh, arg, add_flags,
								  clear_flags, cmd, line);
				if (sr != 0)
					status = sr;
			}
		}
	}
	return status;
}
