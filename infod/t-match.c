#include <assert.h>
#include "match.h"

#define PASS(pattern, string) \
	assert(match_isvalid(pattern)); \
	assert(match(pattern, string))
#define FAIL(pattern, string) \
	assert(match_isvalid(pattern)); \
	assert(!match(pattern, string))
#define INVALID(pattern) \
	assert(!match_isvalid(pattern))

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

	/* Simple wildcard (*) */
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

	/* Simple anychar (?) */
	FAIL("?", "");
	PASS("?", "x");
	FAIL("?", "xx");
	PASS("a?c", "abc");
	FAIL("a?c", "ac");
	PASS("ab?", "abc");
	FAIL("ab?", "ab");

	/* '*?' is the same as '?' */
	FAIL("*?", "");
	PASS("*?", "x");
	FAIL("*?", "xx");
	PASS("a*?c", "abc");
	FAIL("a*?c", "ac");
	PASS("ab*?", "abc");
	FAIL("ab*?", "ab");

	/* UTF-8 characters and ? */
	PASS("€", "€");
	PASS("x?y", "x€y");
	FAIL("x?y", "xせんy");
	PASS("x??y", "xせんy");
	PASS("x*y", "xせんy");
	PASS("x*€", "xせ₫€");

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
	INVALID("(");
	INVALID(")");
	INVALID("|");
	INVALID("\\");
	INVALID("**");
}
