#pragma once
#include <stdint.h>

/*
 * A file-backed, key/value store.
 */
struct store;

/* A <key,value> element */
struct info {
	uint16_t sz;				/* total size of key\0value */
	char keyvalue[];			/* key\0value */
};

/* On error, returns NULL and sets errno */
struct store *store_open(const char *path);
void store_close(struct store *store);

/* Fetchs the last info put for the key, or NULL if not found.
 * A gotten pointer is very short lived, and is invalidated
 * on the next store_put() or store_del(). */
const struct info *store_get(struct store *store, const char *key);
/* Inserts new or replaces existing key with the given value.
 * Prereq: the keyvalue must contain a NUL byte to separate the key from
 * the value.
 * Returns 1 if the value was changed or created.
 * Returns 0 if the value did not change.
 * Returns -1 if the store failed. */
int store_put(struct store *store, uint16_t sz, const char *keyvalue);
/* Delete the key from the store.
 * Returns 1 if the key existed and was deleted.
 * Returns 0 if key did not exist.  */
int store_del(struct store *store, const char *key);

struct store_index {
	unsigned int i;
};

/*
 * Fetches the first info in the store.
 * Initialises the store_index so that it may be passed to store_get_next().
 * The returned pointer is invalidated if store_put() or store_del() is called.
 * Returns NULL if the store is empty.
 */
const struct info *store_get_first(struct store *store, struct store_index *ix);
/* Fetches the next info in the store.
 * Returns NULL at the end of the store. */
const struct info *store_get_next(struct store *store, struct store_index *ix);

