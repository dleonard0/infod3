
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
 *  index_next() - reads and advances the index. Returned info is incref'd, or
 *                 is NULL to indicate end of store. So the caller must decref.
 */

#pragma once

#include <stdint.h>

struct info {
	uint16_t sz;
	uint16_t refcnt;
	char keydata[];				/* <key>+NUL+<data> */
};
struct info *info_new(uint16_t sz);		/* Initial refcnt is 1 */
void info_decref(struct info *info);
void info_incref(struct info *info);


struct store;
struct store *store_new(void);
void store_free(struct store *);
int store_put(struct store *store, struct info *info);
void store_del(struct store *store, struct info *info);
struct info *store_get(struct store *store, const char *key);

struct index;
struct index *index_open(struct store *store);
struct info *index_seek(struct index *index, const char *key);
struct info *index_next(struct index *index);	/* NULL or incref'd info */
void index_close(struct index *index);
