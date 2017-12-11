#pragma once

/*
 * A doubly-linked list element is any struct with
 * 'next' and 'prevp' fields.
 *
 * Every 'linked' element maintains this invariant:
 *          (*p->prevp == p)
 * An 'unlinked' element is any where that invariant
 * does not hold.
 *
 * Example use:
 *
 *	struct foo {
 *		LINK(struct foo);
 *		...
 *	};
 *
 *	struct foo *head = NULL;
 *	struct foo *f = new_foo();    -- f is unlinked
 *	INSERT(f, &head);             -- f is now linked
 *	REMOVE(f);                    -- f is unlinked again
 */

#define LINK(T) T *next, **prevp
#define NEXT(p) (p)->next

/*
 * Insert unlinked p at a head pointer.
 * After insertion, p is 'linked' *head == p.
 */
#define INSERT(p, headp) _INSERT(typeof(*(p)), p, headp)
#define _INSERT(T, p, headp) do { \
	T *_p = (p); \
	T **_headp = (headp); \
	_p->prevp = _headp; \
	_p->next = *_headp; \
	if (_p->next) _p->next->prevp = &_p->next; \
	*_headp = _p; \
    } while (0)

/*
 * Remove a linked p, leaving it unlinked.
 * Does not update a tail pointer. */
#define REMOVE(p) _REMOVE(typeof(*(p)), p)
#define _REMOVE(T, p) do { \
	T *_p = (p); \
	if (_p->next) _p->next->prevp = _p->prevp; \
	*_p->prevp = _p->next; \
    } while (0)

/*
 * Append an unlinked p at tail pointer.
 * Initially the tail should be set to &(head = NULL).
 * Calling INSERT() or REMOVE() will invalidate the tail,
 * pointer.
 */
#define APPEND(p, tailp) _APPEND(typeof(*(p)), p, tailp)
#define _APPEND(T, p, tailp) do { \
	T *_p = (p); \
	T ***_tailp = (tailp); \
	if (**_tailp) abort(); \
	**_tailp = _p; \
	_p->prevp = *_tailp; \
	*_tailp = &_p->next; \
	_p->next = 0; \
    } while (0)


