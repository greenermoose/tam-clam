/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit and resilience tests for 0.2.1 history (src/history/history.c):
 * round-trip escaping, damaged/truncated file recovery, concurrent O_APPEND
 * writers, /dev/full handling, private mode, prefix/substr search, last-arg. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "check.h"
#include "tam-clam/history.h"

static void make_tmp_path(char *buf, size_t cap, const char *tag)
{
	snprintf(buf, cap, "/tmp/tam-clam-hist-%s-%ld.hist", tag, (long)getpid());
	unlink(buf);
}

static void test_roundtrip_and_search(void)
{
	char path[128];
	make_tmp_path(path, sizeof(path), "rt");

	struct tc_history h;
	tc_hist_init(&h, path, false);
	CHECK(tc_hist_add(&h, "echo hello world", 16, 0, "/tmp/my dir\\x") == 0);
	CHECK(tc_hist_add(&h, "printf 'a\\nb'\necho 日本語", 28, 42, "/home/u") == 0);
	CHECK(tc_hist_add(&h, "git status", 10, 0, "/repo") == 0);
	CHECK(tc_hist_add(&h, "git diff --stat", 15, 0, "/repo") == 0);
	/* Leading space and blank lines are ignored. */
	CHECK(tc_hist_add(&h, " secret-cmd", 11, 0, "/") == 0);
	CHECK(tc_hist_add(&h, "   \t ", 5, 0, "/") == 0);
	CHECK(h.count == 4);
	tc_hist_finish(&h);

	/* Reload from disk and verify exact round-trip. */
	tc_hist_init(&h, path, false);
	CHECK(tc_hist_load(&h) == 0);
	CHECK(h.count == 4);
	CHECK(strcmp(h.entries[0].cwd, "/tmp/my dir\\x") == 0);
	CHECK(strcmp(h.entries[0].cmd, "echo hello world") == 0);
	CHECK(h.entries[0].status == 0);
	CHECK(strcmp(h.entries[1].cmd, "printf 'a\\nb'\necho 日本語") == 0);
	CHECK(h.entries[1].status == 42);

	/* Prefix search. */
	CHECK(tc_hist_find_prefix(&h, "git ", 4, (ssize_t)h.count - 1, -1) == 3);
	CHECK(tc_hist_find_prefix(&h, "git ", 4, 2, -1) == 2);
	CHECK(tc_hist_find_prefix(&h, "git ", 4, 1, -1) == -1);
	CHECK(tc_hist_find_prefix(&h, "git ", 4, 2, 1) == 2);
	CHECK(tc_hist_find_prefix(&h, "git ", 4, 3, 1) == 3);

	/* Case-insensitive substring search. */
	CHECK(tc_hist_find_substr(&h, "DIFF", (ssize_t)h.count - 1, -1) == 3);
	CHECK(tc_hist_find_substr(&h, "HELLO", (ssize_t)h.count - 1, -1) == 0);
	CHECK(tc_hist_find_substr(&h, "nope", (ssize_t)h.count - 1, -1) == -1);

	/* Last-arg extraction. */
	size_t wlen = 0;
	char *w = tc_hist_last_arg("cp 'a b' \"dest file\"", 20, &wlen);
	CHECK(w != NULL && strcmp(w, "\"dest file\"") == 0 && wlen == 11);
	free(w);
	w = tc_hist_last_arg("echo foo  ", 10, &wlen);
	CHECK(w != NULL && strcmp(w, "foo") == 0);
	free(w);

	tc_hist_finish(&h);
	unlink(path);
}

static void test_damaged_and_truncated(void)
{
	char path[128];
	make_tmp_path(path, sizeof(path), "dmg");

	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	CHECK(fd >= 0);
	static const char data[] =
		"garbage line without magic\n"
		"v1 1700000001 0 /tmp echo good-one\n"
		"v1 notanumber 0 /tmp bad-ts\n"
		"v1 1700000002 999 /tmp bad-status\n"
		"v1 1700000003 0 /tmp bad-escape\\q\n"
		"v1 1700000004 0 - echo good-two\n"
		"v1 1700000005 0 /tmp truncated-no-newline";
	CHECK(write(fd, data, sizeof(data) - 1) == (ssize_t)(sizeof(data) - 1));
	close(fd);

	struct tc_history h;
	tc_hist_init(&h, path, false);
	CHECK(tc_hist_load(&h) == 0);
	CHECK(h.count == 2);
	CHECK(strcmp(h.entries[0].cmd, "echo good-one") == 0);
	CHECK(strcmp(h.entries[1].cmd, "echo good-two") == 0);
	CHECK(strcmp(h.entries[1].cwd, "") == 0);
	tc_hist_finish(&h);
	unlink(path);
}

static void test_concurrent_writers(void)
{
	char path[128];
	make_tmp_path(path, sizeof(path), "conc");

	pid_t p1 = fork();
	if (p1 == 0) {
		struct tc_history h;
		tc_hist_init(&h, path, false);
		for (int i = 0; i < 100; i++) {
			char cmd[64];
			int n = snprintf(cmd, sizeof(cmd), "echo child1-%d", i);
			if (tc_hist_add(&h, cmd, (size_t)n, 0, "/tmp") != 0)
				_exit(1);
		}
		tc_hist_finish(&h);
		_exit(0);
	}
	pid_t p2 = fork();
	if (p2 == 0) {
		struct tc_history h;
		tc_hist_init(&h, path, false);
		for (int i = 0; i < 100; i++) {
			char cmd[64];
			int n = snprintf(cmd, sizeof(cmd), "echo child2-%d", i);
			if (tc_hist_add(&h, cmd, (size_t)n, 0, "/tmp") != 0)
				_exit(1);
		}
		tc_hist_finish(&h);
		_exit(0);
	}

	int s1 = 0, s2 = 0;
	CHECK(waitpid(p1, &s1, 0) == p1 && WIFEXITED(s1) && WEXITSTATUS(s1) == 0);
	CHECK(waitpid(p2, &s2, 0) == p2 && WIFEXITED(s2) && WEXITSTATUS(s2) == 0);

	struct tc_history h;
	tc_hist_init(&h, path, false);
	CHECK(tc_hist_load(&h) == 0);
	CHECK(h.count == 200);
	int c1 = 0, c2 = 0;
	for (size_t i = 0; i < h.count; i++) {
		if (strncmp(h.entries[i].cmd, "echo child1-", 12) == 0)
			c1++;
		else if (strncmp(h.entries[i].cmd, "echo child2-", 12) == 0)
			c2++;
	}
	CHECK(c1 == 100 && c2 == 100);
	tc_hist_finish(&h);
	unlink(path);
}

static void test_private_and_full_disk(void)
{
	char path[128];
	make_tmp_path(path, sizeof(path), "priv");

	struct tc_history h;
	tc_hist_init(&h, path, true);
	CHECK(tc_hist_add(&h, "echo private-only", 17, 0, "/") == 0);
	CHECK(h.count == 1);
	struct stat st;
	CHECK(stat(path, &st) < 0); /* file must not be created */
	tc_hist_finish(&h);

	/* Full disk (/dev/full) returns error cleanly without losing in-memory entry. */
	if (access("/dev/full", W_OK) == 0) {
		tc_hist_init(&h, "/dev/full", false);
		CHECK(tc_hist_add(&h, "echo enospc", 11, 0, "/") < 0);
		CHECK(h.count == 1);
		CHECK(strcmp(h.entries[0].cmd, "echo enospc") == 0);
		tc_hist_finish(&h);
	}
}

/* A history file over 8 MiB loads its newest entries, not its oldest. */
static void test_large_file_loads_newest(void)
{
	char path[128];
	make_tmp_path(path, sizeof(path), "big");
	FILE *f = fopen(path, "w");
	CHECK(f != NULL);
	/* About 10 MiB of old entries, then a recognisable last one. */
	for (int i = 0; i < 200000; i++)
		fprintf(f, "v1 1700000001 0 /tmp echo old-entry-%06d-padding-padding\n", i);
	fprintf(f, "v1 1700000009 0 /tmp echo newest-entry\n");
	CHECK(fclose(f) == 0);

	struct tc_history h;
	tc_hist_init(&h, path, false);
	CHECK(tc_hist_load(&h) == 0);
	CHECK(h.count > 0);
	CHECK(strcmp(h.entries[h.count - 1].cmd, "echo newest-entry") == 0);
	/* The first entry kept is a whole line, not the tail of a cut one. */
	CHECK(strncmp(h.entries[0].cmd, "echo old-entry-", 15) == 0);
	tc_hist_finish(&h);
	unlink(path);
}

int main(void)
{
	test_roundtrip_and_search();
	test_damaged_and_truncated();
	test_concurrent_writers();
	test_private_and_full_disk();
	test_large_file_loads_newest();
	CHECK_DONE("test_history");
}
