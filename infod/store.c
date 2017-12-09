#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "store.h"

#define STORE_INCREMENT		64		/* store table growth rate */

struct info *
info_new(uint16_t sz)
{
	struct info *info = malloc(sizeof *info + sz);
	if (!info)
		return NULL;
	info->sz = sz;
	info->refcnt = 1;
	return info;
}

void
info_decref(struct info *info)
{
	if (info && !--info->refcnt)
		free(info);
}

void
info_incref(struct info *info)
{
	if (info)
		++info->refcnt;
}

struct store {
	unsigned int n;
	unsigned int max;
	struct info **info;		/* TODO use an AVL tree */
};

struct store *
store_new()
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
store_free(struct store *store)
{
	for (unsigned int i = 0; i < store->n; i++)
		info_decref(store->info[i]);
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

static int
store_eq(const struct store *store, unsigned int i, const char *key)
{
	return i < store->n && strcmp(key, store->info[i]->keyvalue) == 0;
}

int
store_put(struct store *store, struct info *info)
{
	unsigned int i = store_find(store, info->keyvalue);
	if (store_eq(store, i, info->keyvalue)) {
		/* Found existing identical key; replace it */
		info_decref(store->info[i]);
	} else {
		if (store_insert(store, i) == -1) {
			info_decref(info);
			return -1;
		}
	}
	store->info[i] = info;
	return 0;
}

void
store_del(struct store *store, struct info *info)
{
	unsigned int i;
	if (!info)
		return;
	i = store_find(store, info->keyvalue);
	if (i < store->n && store->info[i] == info) {
		info_decref(store->info[i]);
		--store->n;
		memmove(&store->info[i], &store->info[i + 1],
			sizeof store->info[0] * (store->n - i));
	}
}

struct info *
store_get(struct store *store, const char *key)
{
	unsigned int i = store_find(store, key);
	if (!store_eq(store, i, key))
		return NULL;
	info_incref(store->info[i]);
	return store->info[i];
}


struct index {
	struct store *store;
	unsigned int i;		/* Current seek position */
	struct info *prev;	/* Last returned, or NULL for first */
};

/* Only support one index at a time */
static struct index one_index;
static int one_index_opened;

struct index *
index_open(struct store *store)
{
	struct index *index = &one_index;
	if (one_index_opened) {
		errno = ENOMEM;
		return NULL;
	}
	one_index_opened = 1;
	index->store = store;
	index->i = 0;
	index->prev = NULL;
	return index;
}

void
index_close(struct index *index)
{
	//assert(index == &one_index);
	//assert(one_index_opened);
	info_decref(index->prev);
	index->prev = NULL;
	one_index_opened = 0;
}

struct info *
index_seek(struct index *index, const char *key)
{
	struct info *prev = index->prev;
	struct store *store = index->store;
	struct info *info;
	unsigned int n = store->n;
	unsigned int i;

	//assert(index == &one_index);
	//assert(one_index_opened);
	i = store_find(index->store, key);
	info = i < n ? store->info[i] : NULL;
	info_incref(info); /* for the index */
	info_decref(prev);
	index->i = i;
	index->prev = info;
	return info;
}

struct info *
index_next(struct index *index)
{
	struct store *store = index->store;
	struct info *prev = index->prev;
	unsigned int i = index->i;
	unsigned int n = store->n;
	struct info *info;

	//assert(index == &one_index);
	//assert(one_index_opened);

	if (n == 0) {
		/* The list was deleted under us: terminate */
		info = NULL;
		i = ~0;
	} else if (!prev && !i) {
		/* Special case of starting the iterator */
		info = store->info[0];
	} else if (!prev) {
		/* Continuing a terminated iterator: remain terminated */
		return NULL;
	} else if (i < n && store->info[i] == prev) {
		/* Normal case: the list did not change under us */
		i++;
		info = i < n ? store->info[i] : NULL;
	} else {
		/* Something has changed under us.
		 * Seek to one step after the previously returned key */
		i = store_find(index->store, prev->keyvalue);
		if (store_eq(store, i, prev->keyvalue))
			i++;
		info = i < n ? store->info[i] : NULL;
	}

	index->prev = info;
	index->i = i;
	info_incref(info); /* for the index */
	info_decref(prev);
	return info;
}
