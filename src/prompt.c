/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Safe prompt escape expansion and precmd/PROMPT_COMMAND/preexec/PS0 hooks. */
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tam-clam/exec.h"
#include "tam-clam/func.h"
#include "tam-clam/input.h"
#include "tam-clam/parse.h"
#include "tam-clam/prompt.h"
#include "tam-clam/shell.h"

#ifndef TC_VERSION
#define TC_VERSION "unknown"
#endif

struct pbuf {
	char *data;
	size_t len;
	size_t cap;
	bool oom;
};

static void pb_add(struct pbuf *pb, const char *s, size_t n)
{
	if (pb->oom || n == 0 || pb->len >= TC_PROMPT_MAX)
		return;
	if (n > TC_PROMPT_MAX - pb->len)
		n = TC_PROMPT_MAX - pb->len;
	if (pb->len + n + 1 > pb->cap) {
		size_t ncap = pb->cap ? pb->cap * 2 : 128;
		while (ncap < pb->len + n + 1)
			ncap *= 2;
		char *nd = realloc(pb->data, ncap);
		if (!nd) {
			pb->oom = true;
			return;
		}
		pb->data = nd;
		pb->cap = ncap;
	}
	memcpy(pb->data + pb->len, s, n);
	pb->len += n;
	pb->data[pb->len] = '\0';
}

static void pb_str(struct pbuf *pb, const char *s)
{
	if (s)
		pb_add(pb, s, strlen(s));
}

static void pb_ch(struct pbuf *pb, char c)
{
	pb_add(pb, &c, 1);
}

enum tc_cwd_mode {
	TC_CWD_FULL,
	TC_CWD_BASE,
	TC_CWD_TWO,
};

static void append_cwd_two(struct pbuf *pb, const char *s, size_t len)
{
	while (len > 1 && s[len - 1] == '/')
		len--;

	size_t slashes = 0;
	size_t second_last = 0;
	size_t last = 0;
	for (size_t i = 0; i < len; i++) {
		if (s[i] == '/') {
			slashes++;
			second_last = last;
			last = i;
		}
	}

	/* Absolute paths have a leading '/', so 3 slashes means >2 components. */
	size_t threshold = (s[0] == '/') ? 3 : 2;
	if (slashes >= threshold) {
		pb_str(pb, "... ");
		pb_add(pb, s + second_last + 1, len - (second_last + 1));
	} else {
		pb_add(pb, s, len);
	}
}

static void append_cwd(const struct tc_shell *sh, struct pbuf *pb,
		       enum tc_cwd_mode mode)
{
	const char *pwd = sh ? tc_var_get(&sh->vars, "PWD") : NULL;
	char *dyn = NULL;
	if (!pwd || !*pwd) {
		dyn = tc_getcwd();
		pwd = dyn ? dyn : "?";
	}
	const char *home = sh ? tc_var_get(&sh->vars, "HOME") : NULL;
	size_t hlen = (home && home[0] == '/') ? strlen(home) : 0;
	while (hlen > 1 && home[hlen - 1] == '/')
		hlen--;

	bool under_home = hlen > 0 && strncmp(pwd, home, hlen) == 0 &&
			  (pwd[hlen] == '\0' || pwd[hlen] == '/');

	if (mode == TC_CWD_BASE) {
		if (hlen > 0 && strcmp(pwd, home) == 0) {
			pb_ch(pb, '~');
		} else if (strcmp(pwd, "/") == 0) {
			pb_ch(pb, '/');
		} else {
			const char *sl = strrchr(pwd, '/');
			pb_str(pb, (sl && sl[1]) ? sl + 1 : pwd);
		}
	} else if (mode == TC_CWD_TWO) {
		if (under_home) {
			struct pbuf tmp = {0};
			pb_ch(&tmp, '~');
			pb_str(&tmp, pwd + hlen);
			if (!tmp.oom && tmp.data)
				append_cwd_two(pb, tmp.data, tmp.len);
			else
				append_cwd_two(pb, pwd, strlen(pwd));
			free(tmp.data);
		} else {
			append_cwd_two(pb, pwd, strlen(pwd));
		}
	} else {
		if (under_home) {
			pb_ch(pb, '~');
			pb_str(pb, pwd + hlen);
		} else {
			pb_str(pb, pwd);
		}
	}
	free(dyn);
}

char *tc_prompt_expand(const struct tc_shell *sh, const char *raw)
{
	struct pbuf pb = {0};
	pb.data = malloc(64);
	if (!pb.data)
		return NULL;
	pb.cap = 64;
	pb.data[0] = '\0';

	if (!raw)
		return pb.data;

	for (size_t i = 0; raw[i] != '\0'; i++) {
		if (raw[i] != '\\' || raw[i + 1] == '\0') {
			pb_ch(&pb, raw[i]);
			continue;
		}
		char c = raw[++i];
		switch (c) {
		case 'u': {
			const char *u = sh ? tc_var_get(&sh->vars, "USER") : NULL;
			if (!u || !*u) {
				struct passwd *pw = getpwuid(geteuid());
				u = pw ? pw->pw_name : "user";
			}
			pb_str(&pb, u);
			break;
		}
		case 'h':
		case 'H': {
			char host[256];
			if (gethostname(host, sizeof(host) - 1) == 0) {
				host[sizeof(host) - 1] = '\0';
				if (c == 'h') {
					char *dot = strchr(host, '.');
					if (dot)
						*dot = '\0';
				}
				pb_str(&pb, host);
			}
			break;
		}
		case 'w':
			append_cwd(sh, &pb, TC_CWD_FULL);
			break;
		case 'W':
			append_cwd(sh, &pb, TC_CWD_BASE);
			break;
		case '2':
			append_cwd(sh, &pb, TC_CWD_TWO);
			break;
		case '$':
			pb_ch(&pb, geteuid() == 0 ? '#' : '$');
			break;
		case '?': {
			char num[16];
			int n = snprintf(num, sizeof(num), "%d", sh ? sh->status : 0);
			if (n > 0)
				pb_add(&pb, num, (size_t)n);
			break;
		}
		case 'j': {
			char num[32];
			int n = snprintf(num, sizeof(num), "%zu", sh ? sh->jobs.count : 0);
			if (n > 0)
				pb_add(&pb, num, (size_t)n);
			break;
		}
		case 't':
		case 'T':
		case 'A':
		case 'd': {
			time_t now = time(NULL);
			struct tm tmv;
			if (localtime_r(&now, &tmv)) {
				char tb[64];
				const char *fmt = (c == 't') ? "%H:%M:%S" :
						  (c == 'T') ? "%I:%M:%S" :
						  (c == 'A') ? "%H:%M" :
							       "%a %b %d";
				size_t tn = strftime(tb, sizeof(tb), fmt, &tmv);
				if (tn > 0)
					pb_add(&pb, tb, tn);
			}
			break;
		}
		case 's':
			pb_str(&pb, "tam-clam");
			break;
		case 'v':
			pb_str(&pb, TC_VERSION);
			break;
		case 'n':
			pb_ch(&pb, '\n');
			break;
		case 'r':
			pb_ch(&pb, '\r');
			break;
		case 'e':
			pb_ch(&pb, '\x1b');
			break;
		case '\\':
			pb_ch(&pb, '\\');
			break;
		case '[':
		case ']':
			/* Non-printing delimiter markers: stripped (render.c skips SGR). */
			break;
		default:
			pb_ch(&pb, '\\');
			pb_ch(&pb, c);
			break;
		}
	}

	if (pb.oom) {
		free(pb.data);
		return NULL;
	}
	return pb.data;
}

static void run_hook_func(struct tc_shell *sh, struct tc_func *fn)
{
	if (!fn || sh->func_depth >= sh->func_limit)
		return;
	tc_func_ref(fn);
	sh->func_depth++;
	enum tc_flow saved_flow = sh->flow;
	unsigned saved_fc = sh->flow_count;
	sh->flow = TC_FLOW_NONE;
	sh->flow_count = 0;

	(void)tc_exec(sh, fn->body);

	sh->flow = saved_flow;
	sh->flow_count = saved_fc;
	sh->func_depth--;
	tc_func_unref(fn);
}

void tc_prompt_run_precmd(struct tc_shell *sh)
{
	if (!sh || !sh->interactive || sh->in_prompt_hook)
		return;
	sh->in_prompt_hook = true;
	int saved_status = sh->status;

	struct tc_func *fn = tc_func_find(&sh->funcs, "precmd");
	if (fn)
		run_hook_func(sh, fn);

	const char *pc = tc_var_get(&sh->vars, "PROMPT_COMMAND");
	if (pc && *pc) {
		struct tc_input *saved_in = sh->in;
		struct tc_input in;
		tc_input_from_string(&in, pc, strlen(pc));
		in.sh = sh;
		sh->in = &in;
		struct tc_parser p;
		if (tc_parser_init(&p, sh, &in) == TC_OK) {
			for (;;) {
				struct tc_node *node = NULL;
				int r = tc_parse_next(&p, &node);
				if (r != TC_OK)
					break;
				if (node)
					(void)tc_exec(sh, node);
				tc_arena_reset(&sh->arena);
				if (sh->exit_requested)
					break;
			}
			tc_parser_finish(&p);
		}
		tc_input_finish(&in);
		tc_arena_reset(&sh->arena);
		sh->in = saved_in;
	}

	sh->status = saved_status;
	sh->in_prompt_hook = false;
}

void tc_prompt_run_preexec(struct tc_shell *sh, const char *cmd)
{
	if (!sh || !sh->interactive || sh->in_prompt_hook)
		return;
	sh->in_prompt_hook = true;
	int saved_status = sh->status;

	struct tc_func *fn = tc_func_find(&sh->funcs, "preexec");
	if (fn) {
		char **old_p = sh->params;
		size_t old_n = sh->nparams;
		sh->params = NULL;
		sh->nparams = 0;
		char *arg1 = (char *)(cmd ? cmd : "");
		if (tc_shell_set_params(sh, 1, &arg1) == TC_OK)
			run_hook_func(sh, fn);
		(void)tc_shell_set_params(sh, 0, NULL);
		sh->params = old_p;
		sh->nparams = old_n;
	}

	const char *ps0 = tc_var_get(&sh->vars, "PS0");
	if (ps0 && *ps0) {
		char *exp = tc_prompt_expand(sh, ps0);
		if (exp) {
			if (exp[0])
				(void)tc_write_all(STDERR_FILENO, exp, strlen(exp));
			free(exp);
		}
	}

	sh->status = saved_status;
	sh->in_prompt_hook = false;
}
