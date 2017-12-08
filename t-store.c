#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "store.h"

#define DEBUG 0
#if DEBUG
# define dprintf(...) fprintf(stderr, __VA_ARGS__)
#else
# define dprintf(...) /* nothing */
#endif

/* non-destructive test on empty store */
static void
test_empty_store(struct store *store)
{
	struct index *index;

	/* an empty store can be searched without error */
	assert(!store_get(store, "noexist"));

	/* an index will show an empty store to be empty */
	index = index_open(store);
	assert(index);
	assert(!index_next(index));
	index_seek(index, "noexist");
	assert(!index_next(index));
	index_close(index);
}

/* sorted_info: NULL-terminated list of the sorted content infos
 * non_keys: NULL-terminated list of keys that should not exist */
static void
test_store(struct store *store, struct info **sorted_info,
	const char **non_keys)
{
	struct info **i, *found;
	struct index *index;

	/* We can search for each of the content keys */
	for (i = sorted_info; *i; i++) {
		char keybuf[1024];
		unsigned int orig_refcnt = (*i)->refcnt;

		strncpy(keybuf, (*i)->keyvalue, sizeof keybuf);
		keybuf[sizeof keybuf - 1] = '\0';
		found = store_get(store, keybuf);
		if (found != *i) fprintf(stderr, "keybuf = \"%s\"\n", keybuf);
		assert(found);
		assert(found == *i);
		assert(found->refcnt == orig_refcnt + 1);
		info_decref(found);
	}

	/* We can index over the sorted keys exactly */
	index = index_open(store);
	assert(index);
	for (i = sorted_info; *i; i++) {
		found = index_next(index);
		if (found != *i)
			fprintf(stderr, "i = \"%s\"\n", (*i)->keyvalue);
		assert(found);
		assert(found == *i);
	}
	assert(!index_next(index));
	index_close(index);

	/* We cannot find any non-keys in the store */
	if (non_keys)
	for (; *non_keys; non_keys++) {
		found = store_get(store, *non_keys);
		assert(!found);
	}
}

static void
test_index_changing(struct store *store, const char *keyvalue, uint16_t sz)
{
	struct info *insert;
	struct index *index;
	struct info *content[10];
	struct info *info;
	unsigned int n = 0;
	unsigned int i = 0;

	insert = info_new(sz);
	assert(insert);
	memcpy(insert->keyvalue, keyvalue, sz);

	/* can index over the whole store */
	index = index_open(store);
	assert(index);
	while ((info = index_next(index))) {
		content[n++] = info;
		info_incref(info);
	}
	index_close(index);

	/* At each position in the store 0..n insert a new info
	 * index_next() returns consistent values */
	for (i = 0; i <= n; i++) {
		unsigned int j;
		struct info *last;
		unsigned int expected; /* index sequence length expected */

		index = index_open(store);
		assert(index);

		/* Predict the length of the sequence that the index
		 * will yield when inserting 'insert' before the ith
		 * index_next(). The apparent length of the store
		 * will be different because we will sometimes insert before
		 * the moving index pointer and the insert will be
		 * invisible to the index. */
		if (i && strcmp(insert->keyvalue, content[i-1]->keyvalue) < 0)
			expected = n;
		else
			expected = n + 1;

		last = NULL;
		dprintf("+<%s> i=%u e=%u [ ", insert->keyvalue, i, expected);
		for (j = 0; ; j++) {
			if (j == i) {
				dprintf("+ ");
				info_incref(insert); /* for the store */
				assert(store_put(store, insert) == 0);
			}
			info = index_next(index);
			if (!info)
				break;
			dprintf("<%s> ", info->keyvalue);
			if (last) {
				if (strcmp(last->keyvalue, info->keyvalue) >= 0)
					fprintf(stderr, "last=%s info=%s\n",
						last->keyvalue, info->keyvalue);
				assert(strcmp(last->keyvalue, info->keyvalue)<0);
			}
			info_incref(info);
			info_decref(last);
			last = info;
		}
		dprintf("]\n");

		/* The store_put(insert) happened */
		assert(j >= i);
		/* The indexed list was the expected length */
		if (j != expected)
			fprintf(stderr, "i=%u j=%u n=%u expected=%u\n",
				i, j, n, expected);
		assert(j == expected);

		info_decref(last);
		index_close(index);

		/* Repeat the index again, but this time delete the insert
		 * just before the ith index_next(). The delete will be
		 * invisible if insert would occupy the ith or earlier
		 * position. */
		if (i && strcmp(insert->keyvalue, content[i-1]->keyvalue) < 0)
			expected = n + 1;
		else
			expected = n;

		index = index_open(store);
		assert(index);
		last = NULL;
		dprintf("-<%s> i=%u e=%u [ ", insert->keyvalue, i, expected);
		for (j = 0; ; j++) {
			if (j == i) {
				dprintf("- ");
				store_del(store, insert);
			}
			info = index_next(index);
			if (!info)
				break;
			dprintf("<%s> ", info->keyvalue);
			if (last) {
				if (strcmp(last->keyvalue, info->keyvalue) >= 0)
					fprintf(stderr, "last=%s info=%s\n",
						last->keyvalue, info->keyvalue);
				assert(strcmp(last->keyvalue, info->keyvalue)<0);
			}
			info_incref(info);
			info_decref(last);
			last = info;
		}
		dprintf("]\n");

		/* The store_del(insert) happened */
		assert(j >= i);
		/* The indexed list was the expected length */
		if (j != expected)
			fprintf(stderr, "i=%u j=%u n=%u expected=%u\n",
				i, j, n, expected);
		assert(j == expected);

		info_decref(last);
		index_close(index);
	}

	/* Release content[] */
	while (n)
		info_decref(content[--n]);
	info_decref(insert);
}

int
main()
{
	struct store *store;
	struct info *info;
	struct info *info0;
	struct info *info1;
	struct info *info2;
	static const char keyvalue0[] = "key0\0data0";
	static const char keyvalue1[] = "key1\0data1";
	static const char keyvalue2[] = "key2\0data2";
	unsigned int i;

    /* -- empty store -- */

	/* can allocate and immediately free a store */
	store = store_new();
	assert(store);
	store_free(store);

	store = store_new();
	test_empty_store(store);

    /* -- single-entry store tests -- */

	/* can allocate the key-value <"key1","value1"> */
	info1 = info_new(sizeof keyvalue1);
	assert(info1);
	memcpy(info1->keyvalue, keyvalue1, sizeof keyvalue1);
	assert(info1->refcnt == 1);

	/* can store it */
	assert(store_put(store, info1) == 0);
	assert(info1->refcnt == 1);	/* ownership was transfered */

	test_store(store,
		(struct info *[]) { info1, NULL },
		(const char *[]) { "", "key", "key0", "key2", "zzzzzzzz", 0 });

    /* -- a store with 2 entries -- */

	/* can allocate <key2,value2> */
	info2 = info_new(sizeof keyvalue2);
	assert(info2);
	memcpy(info2->keyvalue, keyvalue2, sizeof keyvalue2);
	assert(info2->refcnt == 1);

	/* can store it */
	assert(store_put(store, info2) == 0);
	assert(info2->refcnt == 1);

	test_store(store,
		(struct info *[]) { info1, info2, NULL },
		(const char *[]) { "", "key", "key0", "key3", "zzzzzzzz", 0 });

    /* -- a store with 3 entries -- */

	info0 = info_new(sizeof keyvalue0);
	assert(info0);
	memcpy(info0->keyvalue, keyvalue0, sizeof keyvalue0);

	assert(store_put(store, info0) == 0);
	assert(info0->refcnt == 1);

	test_store(store,
		(struct info *[]) { info0, info1, info2, NULL },
		(const char *[]) { "", "key", "key3", "zzzzzzzz", 0 });

    /* -- delete each key and add it back again -- */
	for (i = 0; i < 3; i++) {
		const char *keys[] = { "key0", "key1", "key2" };
		struct info *infos[] = { info0, info1, info2 };

		const char *keys_removed[2] = { keys[i], NULL };
		struct info *infos_left[3];
		memcpy(infos_left, infos, sizeof infos);
		for (unsigned int j = 0; j < 2; j++)
			infos_left[j] = infos[j < i ? j : j + 1];
		infos_left[2] = NULL;

		/* get and delete the i'th key */
		info = store_get(store, keys[i]);
		assert(info == infos[i]);
		store_del(store, info);
		store_del(store, info);	/* double deletes are OK */
		store_del(store, NULL);	/* deleting NULL is OK */

		/* the store now looks how we expect it to look */
		test_store(store, infos_left, keys_removed);

		/* Put the orginal back */
		assert(store_put(store, info) == 0);
		/* the store is back to normal */
		test_store(store,
			(struct info *[]) { info0, info1, info2, NULL },
			(const char *[]) { "", 0 });
	}

    /* -- replace each key with a different value, then put the original
          back again -- */
	for (i = 0; i < 3; i++) {
		const char *keys[] = { "key0", "key1", "key2" };
		struct info *infos[] = { info0, info1, info2, NULL };
		struct info *repl;

		/* get the original i'th entry */
		const char *key = keys[i];
		info = store_get(store, key);	/* refcnt original */
		assert(info == infos[i]);
		assert(info->refcnt == 2);

		/* construct a replacement, with same key, different data */
		repl = info_new(5); /* enough room for "keyX\0" */
		memcpy(repl->keyvalue, key, 5);

		/* store the replacement, it replaces the original in-store */
		assert(store_put(store, repl) == 0);
		/* the original was decref'd correctly */
		assert(info->refcnt == 1);
		/* and the replacement's refcnt did not change */
		assert(repl->refcnt == 1);

		/* the store now appears with infoX replaced by repl */
		assert(store_get(store, key) == repl);
		assert(repl->refcnt == 2);   /* we hold onto it for later */
		infos[i] = repl;
		test_store(store, infos, NULL);

		/* put back the original infoX */
		assert(store_put(store, info) == 0);
		/* its refcnt will have remained stable */
		assert(info->refcnt == 1);
		/* But the previous replacement will have been decref'd */
		assert(repl->refcnt == 1);
		/* remove our reference (we held onto it prior) */
		info_decref(repl);
	}

    /* -- alter the store while an index is moving -- */

	test_index_changing(store, "key", 4);
	test_index_changing(store, "key0a", 6);
	test_index_changing(store, "key1a", 6);
	test_index_changing(store, "key2a", 6);
	test_index_changing(store, "zzzzz", 6);

    /* -- cleanup -- */
	assert(info0->refcnt == 1);
	assert(info1->refcnt == 1);
	assert(info2->refcnt == 1);

	/* Incref info2 around so we can see it decref'd by store_free() */
	info_incref(info2);
	assert(info2->refcnt == 2);
	store_free(store);
	assert(info2->refcnt == 1);
	info_decref(info2);

}
