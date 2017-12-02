#pragma once

/*
 * Match a string against a glob-like pattern.
 *
 * The pattern language is similar to glob(7) but much more limited
 * so that the matcher can run in linear time O(len(pattern) + len(string))
 * and have a small constant bound on stack and heap.
 *
 *   x        Any character other than a metachar will match itself.
 *            The metachars are  ( | ) * \
 *
 *   *x       Greedily match up to and including the very next 'x'
 *            character. If * ends the pattern, or appears as *| or *)
 *            then it will match the rest of the input string.
 *            Patterns containing ** or *( are invalid.
 *
 *   (a|b|c)  Match the first matching branch of a b or c. Once a branch is
 *            matched, the matching process continues after the closing ).
 *            Parentheses can be nested up to 4 deep.
 *
 *   \x       Match exactly the literal character 'x' which may be a metachar.
 *            The backslash permits converting metacharacters into literals.
 *
 * Returns 0 on successful match.
 * Returns -1 on match failure or a malformed pattern.
 */
int match(const char *pattern, const char *string);
