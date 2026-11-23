/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <string.h>

#include "check.h"
#include "tam-clam/pattern.h"

static bool match_raw(const char *p, const char *s)
{
	struct tc_pat pat = { .text = p, .quoted = NULL, .len = strlen(p) };
	return tc_pat_match(&pat, s, strlen(s));
}

int main(void)
{
	/* Basic literals, *, ? */
	CHECK(match_raw("abc", "abc"));
	CHECK(!match_raw("abc", "abd"));
	CHECK(!match_raw("abc", "ab"));
	CHECK(match_raw("*", ""));
	CHECK(match_raw("*", "hello world"));
	CHECK(match_raw("a*c", "ac"));
	CHECK(match_raw("a*c", "abbbbc"));
	CHECK(!match_raw("a*c", "abbbbd"));
	CHECK(match_raw("a?c", "abc"));
	CHECK(!match_raw("a?c", "ac"));
	CHECK(!match_raw("a?c", "abbc"));
	CHECK(match_raw("?*?", "ab"));
	CHECK(!match_raw("?*?", "a"));

	/* Bracket expressions */
	CHECK(match_raw("[abc]", "b"));
	CHECK(!match_raw("[abc]", "d"));
	CHECK(match_raw("[!abc]", "d"));
	CHECK(!match_raw("[!abc]", "a"));
	CHECK(match_raw("[^abc]", "z"));
	CHECK(!match_raw("[^abc]", "c"));
	CHECK(match_raw("[a-z]", "m"));
	CHECK(!match_raw("[a-z]", "M"));
	CHECK(match_raw("[]a]", "]"));
	CHECK(match_raw("[]a]", "a"));
	CHECK(match_raw("[!]]", "x"));
	CHECK(!match_raw("[!]]", "]"));
	CHECK(match_raw("[a-]", "-"));
	CHECK(match_raw("[a-]", "a"));

	/* POSIX character classes */
	CHECK(match_raw("[[:digit:]][[:digit:]]", "42"));
	CHECK(!match_raw("[[:digit:]][[:digit:]]", "4a"));
	CHECK(match_raw("[[:alpha:]_]*", "_foo123"));
	CHECK(match_raw("[[:xdigit:]][[:xdigit:]]", "f0"));
	CHECK(!match_raw("[[:xdigit:]]", "G"));
	CHECK(match_raw("[[:space:]]", "\t"));

	/* Unclosed bracket is literal '[' */
	CHECK(match_raw("[abc", "[abc"));
	CHECK(!match_raw("[abc", "a"));

	/* Escaping via unquoted backslash */
	CHECK(match_raw("\\*", "*"));
	CHECK(!match_raw("\\*", "abc"));
	CHECK(match_raw("\\?", "?"));
	CHECK(!match_raw("\\?", "x"));

	/* Per-byte quoting metadata (quoted metacharacters are literal) */
	{
		uint8_t q[3] = {1, 0, 1};
		struct tc_pat pat = { .text = "*?*", .quoted = q, .len = 3 };
		CHECK(tc_pat_match(&pat, "*x*", 3));
		CHECK(!tc_pat_match(&pat, "ax*", 3));
		CHECK(!tc_pat_match(&pat, "*xa", 3));
		CHECK(tc_pat_has_meta(&pat));
	}
	{
		uint8_t q[3] = {1, 1, 1};
		struct tc_pat pat = { .text = "*?[", .quoted = q, .len = 3 };
		CHECK(tc_pat_match(&pat, "*?[", 3));
		CHECK(!tc_pat_match(&pat, "abc", 3));
		CHECK(!tc_pat_has_meta(&pat));
	}
	{
		struct tc_pat pat = { .text = "[unclosed", .quoted = NULL, .len = 9 };
		CHECK(!tc_pat_has_meta(&pat));
	}
	{
		struct tc_pat pat = { .text = "[a-z]", .quoted = NULL, .len = 5 };
		CHECK(tc_pat_has_meta(&pat));
	}

	/* Prefix and suffix trimming (${x#pat}, ${x##pat}, ${x%pat}, ${x%%pat}) */
	{
		struct tc_pat p1 = { .text = "*/", .quoted = NULL, .len = 2 };
		const char *s = "/usr/local/bin/sh";
		size_t slen = strlen(s);
		CHECK(tc_pat_trim_prefix(&p1, s, slen, false) == 1);
		CHECK(tc_pat_trim_prefix(&p1, s, slen, true) == 15);
	}
	{
		struct tc_pat p2 = { .text = ".*", .quoted = NULL, .len = 2 };
		const char *s = "archive.tar.gz";
		size_t slen = strlen(s);
		CHECK(tc_pat_trim_suffix(&p2, s, slen, false) == 3);
		CHECK(tc_pat_trim_suffix(&p2, s, slen, true) == 7);
	}

	/* Adversarial backtracking test: must complete in linear time. */
	{
		char pat_s[128];
		size_t pos = 0;
		for (int i = 0; i < 30; i++) {
			pat_s[pos++] = 'a';
			pat_s[pos++] = '*';
		}
		pat_s[pos++] = 'b';
		pat_s[pos] = '\0';

		char subj[128];
		memset(subj, 'a', 100);
		subj[100] = '\0';

		struct tc_pat pat = { .text = pat_s, .quoted = NULL, .len = pos };
		CHECK(!tc_pat_match(&pat, subj, 100));
		subj[99] = 'b';
		CHECK(tc_pat_match(&pat, subj, 100));
	}

	CHECK_DONE("test_pattern");
}
