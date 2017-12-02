#include <assert.h>
#include "match.h"

#define PASS(pattern, string) \
	assert(match(pattern, string) == 0)
#define FAIL(pattern, string) \
	assert(match(pattern, string) == -1)

int
main()
{
	/* Simple */
	PASS("", "");
	PASS("x", "x");
	FAIL("x", "y");
	FAIL("", "y");
	FAIL("x", "");

	/* Escapes */
	PASS("\\x", "x");
	PASS("\\(", "(");

	/* Simple wildcard */
	PASS("*", "");
	PASS("*", "foo");
	PASS("*.", "foo.");
	FAIL("*.", "foo..");

	/* Double wildcard */
	PASS("*a*", "abba");
	PASS("*a*", "baba");
	PASS("*a*", "a");
	PASS("*a*", "aa");
	FAIL("*a*", "b");
	FAIL("*a*", "");

	/* Parentheses match */
	PASS("()", "");		FAIL("()", "x");
	PASS("(a)", "a");	FAIL("(a)", "x");	FAIL("(a)", "");
	PASS("(a|b)", "a");
	PASS("(a|b)", "b");	FAIL("(a|b)", "x");	FAIL("(a|b)", "");
	PASS("(a|b|c)", "a");
	PASS("(a|b|c)", "b");
	PASS("(a|b|c)", "c");	FAIL("(a|b|c)", "x");	FAIL("(a|b|c)", "");

	/* Nested parentheses */
	PASS("(a|b(c|d)e|f)g", "bdeg");
	FAIL("(a|b(c|d)e|f)g", "beg");
	FAIL("(a|b(c|d)e|f)g", "bfg");

	/* Malformed */
	FAIL("(", "");
	FAIL(")", "");
	FAIL("|", "");
	FAIL("\\", "\\");
	FAIL("**", "");
}
