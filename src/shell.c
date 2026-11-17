/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tam-clam/input.h"
#include "tam-clam/shell.h"

void tc_shell_init(struct tc_shell *sh, const char *name)
{
	memset(sh, 0, sizeof(*sh));
	sh->name = name;
	tc_arena_init(&sh->arena, TC_ARENA_MAX);
	ssize_t n = readlink("/proc/self/exe", sh->self_path, sizeof(sh->self_path) - 1);
	sh->self_path[n > 0 ? n : 0] = '\0';
}

void tc_shell_finish(struct tc_shell *sh)
{
	tc_arena_finish(&sh->arena);
}

int tc_write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

void tc_error(const struct tc_shell *sh, unsigned long line, const char *fmt, ...)
{
	char buf[1024];
	int n;
	if (line && !sh->interactive)
		n = snprintf(buf, sizeof(buf), "%s: line %lu: ", sh->name, line);
	else
		n = snprintf(buf, sizeof(buf), "%s: ", sh->name);
	if (n < 0)
		return;
	size_t used = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;

	va_list ap;
	va_start(ap, fmt);
	n = vsnprintf(buf + used, sizeof(buf) - used, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	used += (size_t)n < sizeof(buf) - used ? (size_t)n : sizeof(buf) - used - 1;
	/* A message too long for the buffer is cut, but still ends a line. */
	if (used == sizeof(buf) - 1)
		used--;
	buf[used++] = '\n';
	(void)tc_write_all(STDERR_FILENO, buf, used);
}
