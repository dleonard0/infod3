#pragma once

/* A doubly-linked list element is any struct with
 * a 'next' and 'prevp' field, where every
 * linked element maintains this invariant:
 *          (*p->prevp == p)
 */

/* Insert unlinked p at a head pointer. After insertion, head = p. */
#define INSERT(p, head) do { \
	(p)->prevp = &(head); \
	(p)->next = (head); \
	if (head) (head)->prevp = &(p)->next; \
	(head) = (p); \
    } while (0)

/* Append unlinked p at tail pointer.
 * The tail pointer is not updated when the tail is REMOVEd! */
#define APPEND(p, tail) do { \
	INSERT(p, *(tail)); \
	(tail) = &(p)->next; \
    } while (0)

/* Remove a linked p, leaving it unlinked. */
#define REMOVE(p) do { \
	if ((p)->next) (p)->next->prevp = (p)->prevp; \
	*(p)->prevp = (p)->next; \
    } while (0)

#define LINK(T) T *next, **prevp

#define NEXT(p) (p)->next
