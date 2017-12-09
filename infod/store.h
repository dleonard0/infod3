#pragma once
#include <stdint.h>

/*
 * An in-memory, reference-counted key/value store.
 *
 * Reference counting is used because an info (key/value pair) may be in
 * use by I/O even when it has been deleted from the store.
 *
 *  info_new()   - Allocates sz bytes. NULL return sets errno.
 *  store_new()  - Initializes new store. NULL return sets errno.
 *  store_get()  - Finds info with same key; returns NULL or incref'd pointer.
 *  store_put()  - Inserts an info; decref'ing any old info with the same key.
 *  store_del()  - Decrefs and forgets the info obtained from index.
 *  index_open() - Creates an index into the store. NULL return sets errno.
 *  index_seek() - Seeks to the first info with key equal or larger than arg
 *  index_next() - reads and advances the index. Returns NULL at end.
 */
struct store;

/* A <key,value> element */
struct info {
	uint16_t sz;				/* total size of key\0value */
	uint16_t refcnt;
	char keyvalue[];			/* key\0value */
};

struct store *store_new(void);
void store_free(struct store *);

/* Info pointer parameters are either:
 *   GIVEN - ownership is given to the function, or
 *   LOANED - ownership is not taken
 * Function-returned info pointers are either:
 *   OWNED - ownership moves to caller
 *   BORROWED - not given
 *
 * Owned pointers must be protected from leaking.
 * The NULL pointer can be freely given or owned.
 *
 * Borrowed pointers have a short range and their
 * content is only valid immediately. They may be
 * immediately loaned, though never given.
 */

/* info: GIVEN */
void info_decref(struct info *info);
/* info: must be BORROWED, becomes OWNED */
void info_incref(struct info *info);

/* returns OWNED; returns NULL on error */
struct info *info_new(uint16_t sz);
/* info: GIVEN
 * Returns -1 if the store failed. (info is decref'd) */
int store_put(struct store *store, struct info *info);
/* info: LOANED */
void store_del(struct store *store, struct info *info);
/* returns OWNED; returns NULL if not found. */
struct info *store_get(struct store *store, const char *key);

/*
 * An index into the store.
 * The store may be modified while the index is active.
 */
struct index;
struct index *index_open(struct store *store);
void index_close(struct index *index);

/* returns BORROWED; returns next info >= key or NULL */
struct info *index_seek(struct index *index, const char *key);
/* returns BORROWED; returns next info, or NULL at end */
struct info *index_next(struct index *index);
