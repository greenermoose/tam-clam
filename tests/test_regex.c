/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Unit tests for the linear-time POSIX ERE engine (src/regex.c). */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "tam-clam/regex.h"

static int match_str(const char *pat, const char *s,
		     struct tc_re_match *g, size_t ng, size_t *out_ng)
{
	return tc_re_exec(pat, strlen(pat), s, strlen(s), g, ng, out_ng);
}

static void test_basic(void)
{
	struct tc_re_match g[8];
	size_t ng = 0;

	CHECK(match_str("abc", "xxabcyy", g, 8, &ng) == TC_RE_MATCH);
	CHECK(ng == 1);
	CHECK(g[0].start == 2);
	CHECK(g[0].end == 5);

	CHECK(match_str("abc", "ab", NULL, 0, NULL) == TC_RE_NOMATCH);
	CHECK(match_str("^abc$", "abc", NULL, 0, NULL) == TC_RE_MATCH);
	CHECK(match_str("^abc$", "xabc", NULL, 0, NULL) == TC_RE_NOMATCH);
	CHECK(match_str("^abc$", "abcx", NULL, 0, NULL) == TC_RE_NOMATCH);
	CHECK(match_str("a.c", "axc", NULL, 0, NULL) == TC_RE_MATCH);
	CHECK(match_str("a\\.c", "axc", NULL, 0, NULL) == TC_RE_NOMATCH);
	CHECK(match_str("a\\.c", "a.c", NULL, 0, NULL) == TC_RE_MATCH);
}

static void test_quantifiers_and_alt(void)
{
	struct tc_re_match g[8];
	size_t ng = 0;

	CHECK(match_str("a+b?", "aaab", g, 8, &ng) == TC_RE_MATCH);
	CHECK(g[0].start == 0);
	CHECK(g[0].end == 4);

	CHECK(match_str("cat|dog", "my dog ran", g, 8, &ng) == TC_RE_MATCH);
	CHECK(g[0].start == 3);
	CHECK(g[0].end == 6);

	CHECK(match_str("^a{2,4}$", "a", NULL, 0, NULL) == TC_RE_NOMATCH);
	CHECK(match_str("^a{2,4}$", "aa", NULL, 0, NULL) == TC_RE_MATCH);
	CHECK(match_str("^a{2,4}$", "aaaa", NULL, 0, NULL) == TC_RE_MATCH);
	CHECK(match_str("^a{2,4}$", "aaaaa", NULL, 0, NULL) == TC_RE_NOMATCH);
	CHECK(match_str("^a{3}$", "aaa", NULL, 0, NULL) == TC_RE_MATCH);
	CHECK(match_str("^a{3,}$", "aaaaa", NULL, 0, NULL) == TC_RE_MATCH);
	CHECK(match_str("^a{0}b$", "b", NULL, 0, NULL) == TC_RE_MATCH);
}

static void test_classes(void)
{
	CHECK(match_str("^[[:alpha:]_][[:alnum:]_]*$", "foo_123", NULL, 0, NULL) == TC_RE_MATCH);
	CHECK(match_str("^[[:alpha:]_][[:alnum:]_]*$", "1foo", NULL, 0, NULL) == TC_RE_NOMATCH);
	CHECK(match_str("^[^0-9]+$", "abc!", NULL, 0, NULL) == TC_RE_MATCH);
	CHECK(match_str("^[^0-9]+$", "ab3c", NULL, 0, NULL) == TC_RE_NOMATCH);
	CHECK(match_str("^[a-f0-9]+$", "deadbeef", NULL, 0, NULL) == TC_RE_MATCH);
	CHECK(match_str("^[]a]+$", "]a]", NULL, 0, NULL) == TC_RE_MATCH);
}

static void test_captures(void)
{
	struct tc_re_match g[8];
	size_t ng = 0;

	CHECK(match_str("^([0-9]{4})-([0-9]{2})-([0-9]{2})$", "2026-11-24",
			g, 8, &ng) == TC_RE_MATCH);
	CHECK(ng == 4);
	CHECK(g[0].start == 0 && g[0].end == 10);
	CHECK(g[1].start == 0 && g[1].end == 4);
	CHECK(g[2].start == 5 && g[2].end == 7);
	CHECK(g[3].start == 8 && g[3].end == 10);

	/* Optional group that does not participate. */
	CHECK(match_str("^(foo)(bar)?$", "foo", g, 8, &ng) == TC_RE_MATCH);
	CHECK(ng == 3);
	CHECK(g[1].start == 0 && g[1].end == 3);
	CHECK(g[2].start == SIZE_MAX);
}

static void test_errors_and_linear_time(void)
{
	CHECK(match_str("(unclosed", "a", NULL, 0, NULL) == TC_RE_ERR_SYNTAX);
	CHECK(match_str("[unclosed", "a", NULL, 0, NULL) == TC_RE_ERR_SYNTAX);
	CHECK(match_str("*bad", "a", NULL, 0, NULL) == TC_RE_ERR_SYNTAX);
	CHECK(match_str("a\\", "a", NULL, 0, NULL) == TC_RE_ERR_SYNTAX);
	CHECK(match_str("a{5,2}", "aaa", NULL, 0, NULL) == TC_RE_ERR_SYNTAX);
	CHECK(match_str("[[:bogus:]]", "a", NULL, 0, NULL) == TC_RE_ERR_SYNTAX);

	/* Classic ReDoS pattern must terminate instantaneously in linear time. */
	char evil[64];
	memset(evil, 'a', 60);
	evil[60] = 'c';
	evil[61] = '\0';
	CHECK(match_str("^(a+)+b$", evil, NULL, 0, NULL) == TC_RE_NOMATCH);
	CHECK(match_str("(a|aa)+b", evil, NULL, 0, NULL) == TC_RE_NOMATCH);
}

int main(void)
{
	test_basic();
	test_quantifiers_and_alt();
	test_classes();
	test_captures();
	test_errors_and_linear_time();
	CHECK_DONE("test_regex");
}
