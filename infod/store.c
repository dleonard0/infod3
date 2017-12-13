#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "store.h"

#define STORE_INCREMENT		64		/* store table growth rate */

struct store {
	unsigned int n;
	unsigned int max;
	struct info **info;		/* TODO use an AVL tree */
};

/* allocate structure with a flexible array member */
static struct info *
info_new(uint16_t sz, const char *keyvalue)
{
	struct info *info = malloc(sizeof *info + sz);
	if (!info)
		return NULL;
	info->sz = sz;
	memcpy(info->keyvalue, keyvalue, sz);
	return info;
}

struct store *
store_open(const char *filename /* TODO use */)
{
	struct store *store = malloc(sizeof *store);
	if (!store)
		return NULL;
	store->n = 0;
	store->max = 0;
	store->info = NULL;
	return store;
}

void
store_close(struct store *store)
{
	for (unsigned int i = 0; i < store->n; i++)
		free(store->info[i]);
	free(store->info);
	free(store);
}

/* Binary search to one more than the largest index into
 * store->info[] that has a key lexicographically
 * smaller than 'key'. In other words, find the slot
 * having the same key, or where the slot would be
 * if the key were in the info[] list. */
static unsigned int
store_find(const struct store *store, const char *key)
{
	unsigned int a, b, m;
	struct info **list = store->info;

	a = 0;
	b = store->n;
	while ((m = (a + b) / 2) > a) {
		int cmp = strcmp(key, list[m]->keyvalue);
		if (cmp == 0)
			break;
		else if (cmp < 0)
			b = m;
		else
			a = m + 1;
	}
	if (m < b && strcmp(key, list[m]->keyvalue) > 0)
		m++;
	return m;
}

/* Insert a new cell at store->info[i] */
static int
store_insert(struct store *store, unsigned int i)
{
	unsigned int n = store->n;

	if (n + 1 >= store->max) {
		struct info **new_info;
		unsigned int new_max = store->max + STORE_INCREMENT;
		new_info = realloc(store->info, new_max * sizeof *new_info);
		if (!new_info)
			return -1;
		store->max = new_max;
		store->info = new_info;
	}
	memmove(&store->info[i + 1], &store->info[i],
		sizeof store->info[0] * (store->n - i));
	++store->n;
	return i;
}

/* test if the i'th slot has the given key */
static int
store_eq(const struct store *store, unsigned int i, const char *key)
{
	return i < store->n && strcmp(key, store->info[i]->keyvalue) == 0;
}

int
store_put(struct store *store, uint16_t sz, const char *keyvalue)
{
	unsigned int i;
	struct info *info;
	info = info_new(sz, keyvalue);
	if (!info)
		return -1;

	i = store_find(store, keyvalue);
	if (store_eq(store, i, keyvalue)) {
		/* Found existing identical key; free up the slot */
		free(store->info[i]);
	} else {
		/* make space for a new slot */
		if (store_insert(store, i) == -1) {
			free(info);
			return -1;
		}
	}
	store->info[i] = info;
	return 0;
}

int
store_del(struct store *store, const char *key)
{
	unsigned int i;

	if (!key)
		return 0;
	i = store_find(store, key);
	if (!store_eq(store, i, key))
		return 0;
	free(store->info[i]);
	--store->n;
	memmove(&store->info[i], &store->info[i + 1],
		sizeof store->info[0] * (store->n - i));
	return 1;
}

const struct info *
store_get(struct store *store, const char *key)
{
	unsigned int i = store_find(store, key);
	if (!store_eq(store, i, key))
		return NULL;
	return store->info[i];
}

const struct info *
store_get_first(struct store *store, struct store_index *ix)
{
	ix->i = 0;
	return store_get_next(store, ix);
}

const struct info *
store_get_next(struct store *store, struct store_index *ix)
{
	if (ix->i >= store->n)
		return NULL;
	return store->info[ix->i++];
}

