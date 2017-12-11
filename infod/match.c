#include <stdlib.h>
#include "match.h"

#define MAX_PAREN 4

char CHECK[] = "";

/* Compare the UTF-8 characters at two pointers for equality. */
static int
utf8eq(const char *a, const char *b)
{
	if (*a != *b)
		return 0;
	if ((*a & 0xc0) != 0xc0)
		return 1;
	a++; b++;
	while ((*a & 0xc0) == 0x80) {
		if (*a != *b)
			return 0;
		a++; b++;
	}
	return 1;
}

/* Advance pointer over a UTF-8 character */
static void
utf8inc(const char **ap)
{
	if ((*(*ap)++ & 0xc0) == 0xc0)
		while ((**ap & 0xc0) == 0x80)
			++*ap;
}

/*
 * Match pattern against string.
 * If string is the special pointer CHECK, then the
 * pattern parameter is only checked for errors.
 * Returns 1 on success
 * Returns 0 on mismatch
 * Returns -1 on invalid pattern
 */
int
do_match(const char *pattern, const char *string)
{
	char p;
	struct {
	    const char *start; /* string position at ( */
	    int failed;         /* iff mismatch after '(' or '|' */
	    const char *success;/* unfailed string position at '|' */
	} parens[MAX_PAREN], *paren = NULL;

	while ((p = *pattern++)) {
		if (p == '*') {
			char n = *pattern;	/* '*n' */
			if (n == '*' || n == '(')
				return -1;	/* malformed pattern */
			else if (!n || n == '|' || n == ')') {
				/* greedy to end of string */
				if (string != CHECK)
					string = "";
			} else if (n == '?') {
				/* pattern *? is equivalent to ? */
			} else {
				if (n == '\\') {	/* '*\n' */
					if (!(n = pattern[1]))
						return -1; /* \ at end */
				}
				if (string != CHECK)
				    while (*string && !utf8eq(string, pattern))
					utf8inc(&string); /* greedy forward */
			}
		} else if (p == '(') {
			/* open new paren */
			if (!paren)
				paren = &parens[0];
			else {
				if (paren >= &parens[MAX_PAREN - 1])
					return -1;	/* too many parens */
				paren++;
			}
			paren->failed = 0;
			paren->success = NULL;
			paren->start = string;
		} else if (p == '|') {
			if (!paren)
				return -1;	/* | without ( */
			if (!paren->failed && !paren->success)
				paren->success = string;
			if (string != CHECK)
				string = paren->start;
			paren->failed = 0;
		} else if (p == ')') {
			if (!paren)
				return -1;	/* ) without ( */
			if (!paren->failed && !paren->success)
				paren->success = string;
			if (paren == &parens[0]) {
				/* outer-level paren */
				if (string != CHECK)
					string = paren->success;
				if (!string)
					return 0; /* mismatch at outer ) */
				paren = NULL;
			} else {
				if (paren->success) {
					if (string != CHECK)
						string = paren->success;
				} else
					(paren - 1)->failed = 1;
				paren--;
			}
		} else {
			int any = 0;
			if (p == '\\') {
				p = *pattern++;
				if (!p)
					return -1; /* \ at end of pattern */
			} else if (p == '?')
				any = 1;
			if (string == CHECK)
				; /* only doing validity check */
			else if (any ? *string : *string == p) {
				if (any)
					utf8inc(&string);
				else
					string++;
			} else if (paren)
				paren->failed = 1;
			else
				return 0;	/* char mismatch */
		}
	}
	if (paren)
		return -1;	/* unclosed ( */
	if (string == CHECK)
		return 1;	/* passed checks */
	return *string == '\0';	/* pass iff string exhausted */
}

int
match(const char *pattern, const char *string)
{
	return do_match(pattern, string);
}

int
match_isvalid(const char *pattern)
{
	return do_match(pattern, CHECK) == 1;
}
