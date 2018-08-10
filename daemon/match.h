#pragma once

/*
 * A string matcher using simplified glob-like patterns.
 * The pattern language is defined in PROTOCOL.
 *
 * Returns 1 on a successful, whole-string match.
 * Returns 0 on either a match failure or when the pattern is invalid.
 */
int match(const char *pattern, const char *string);

/*
 * Tests if the pattern is valid
 * Returns 1 if the pattern is valid.
 * Returns 0 if the pattern is invalid, and would match nothing.
 */
int match_isvalid(const char *pattern);
