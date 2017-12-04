#pragma once

/*
 * A string matcher using simplified glob-like patterns.
 *
 * The pattern language is similar to glob(7) but very much more limited
 * so that the matcher can run in linear time O(len(pattern) + len(string))
 * and maintain a small constant bound on stack and heap use.
 *
 * Elements of the pattern are:
 *
 *   x        Any character other than a metachar will match itself.
 *            The metachars are  ( | ) * \
 *
 *   *x       Matches the shortest substring up to and including the very
 *            next 'x' character in the string.
 *
 *   *        If * ends the pattern, or is followed by | or )
 *            then it will match the rest of the input string.
 *            Patterns containing ** or *( are invalid.
 *
 *   (a|b|c)  Matches the first matching branch of a b or c. Once a branch is
 *            matched, subsequent branches are skipped and the matching
 *            process continues after the closing ).
 *            Parentheses can be nested up to 4 deep.
 *
 *   \x       Matches exactly the character x which may be a metachar.
 *
 * Returns 0 on a successful, whole-string match.
 * Returns -1 on either a match failure or when the pattern is invalid.
 */
int match(const char *pattern, const char *string);

