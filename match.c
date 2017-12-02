#include <stdlib.h>
#include "match.h"

#define MAX_PAREN 4
int
match(const char *pattern, const char *string)
{
	char p;
	struct {
	    const char *string; /* string position at ( */
	    int failed;         /* mismatch after '(' or '|' */
	    const char *success;/* unfailed string position at '|' */
	} parens[MAX_PAREN], *paren = NULL;

	while ((p = *pattern++)) {
		if (p == '*') {
			char n = *pattern;	/* '*n' */
			if (!n || n == '|' || n == ')')
				string = "";	/* greedy to end of string */
			else if (n == '*' || n == '(')
				return -1;	/* malformed pattern */
			else {
				if (n == '\\') {	/* '*\n' */
					if (!(n = pattern[1]))
						return -1; /* \ at end */
				}
				while (*string && *string != n)
					string++;	/* greedy forward */
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
			/* record string position at ( for restart */
			paren->string = string;
		} else if (p == '|') {
			if (!paren)
				return -1;	/* | without ( */
			if (!paren->failed && !paren->success)
				paren->success = string;
			/* restart again */
			string = paren->string;
			paren->failed = 0;
		} else if (p == ')') {
			if (!paren)
				return -1;	/* ) without ( */
			if (!paren->failed && !paren->success)
				paren->success = string;
			if (paren == &parens[0]) {
				/* outer-level paren */
				string = paren->success;
				paren = NULL;
				if (!string)
					return -1; /* failed on outer ) */
			} else {
				if (paren->success)
					string = paren->success;
				else
					(paren - 1)->failed = 1;
				paren--;
			}
		} else {
			if (p == '\\') {
				p = *pattern++;
				if (!p)
					return -1; /* \ at end of pattern */
			}
			if (*string == p)
				string++;
			else if (paren)
				paren->failed = 1;
			else
				return -1;	/* \x mismatch */
		}
	}
	if (paren)
		return -1;	/* unclosed ( */
	if (*string)
		return -1;	/* string unexhausted */
	return 0;
}
