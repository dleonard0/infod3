#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdarg.h>

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
	struct store_index ix;

	/* an empty store can be searched without error */
	assert(!store_get(store, "noexist"));

	/* there is no first entry in an empty store */
	assert(!store_get_first(store, &ix));

	/* can delete anything from an empty store with no effect */
	assert(store_del(store, "anything") == 0);
}

/* assert that the store contains all of the given infos in the
 * order given */
__attribute__((sentinel))
static void
assert_store_is_(const char *file, int line, const char *expr,
	struct store *store, const struct info *expect0, ...)
{
	const struct info *found;
	const struct info *expect;
	const struct info *i;
	struct store_index ix;
	va_list ap;
	va_list ap2;

	#define FMTi "%s=%.*s"
	#define ARGi(i) (i) ? (i)->keyvalue : "(null)", \
			(i) ? (int)((i)->sz - strlen((i)->keyvalue) - 1) : 0, \
			(i) ? &(i)->keyvalue[strlen((i)->keyvalue) + 1] : 0

	va_start(ap, expect0);
	va_copy(ap2, ap);
	/* We can search for each of the content keys, individually */
	for (expect = expect0;
	     expect;
	     expect = va_arg(ap2, const struct info *))
	{
		char keybuf[1024];

		memset(keybuf, '?', sizeof keybuf);
		strncpy(keybuf, expect->keyvalue, sizeof keybuf - 1);
		keybuf[sizeof keybuf - 1] = '\0';
		found = store_get(store, keybuf);
		if (!found) {
			fprintf(stderr, "%s:%d: failed %s\n"
				"\tmissing key %s\n",
				file, line, expr, keybuf);
			abort();
		}
		if (found->sz != expect->sz ||
		    memcmp(found->keyvalue, expect->keyvalue, found->sz) != 0)
		{
			fprintf(stderr, "%s:%d: failed %s at get '%s'\n"
				"\texpected value " FMTi ", actual " FMTi "\n",
				file, line, expr, keybuf,
				ARGi(expect), ARGi(found));
			abort();
		}
	}
	va_end(ap2);

	/* We can index over the sorted keys exactly */
	for (expect = expect0, i = store_get_first(store, &ix);
	     expect && i;
	     expect = va_arg(ap, const struct info *),
	     i = store_get_next(store, &ix))
	{
		if (expect->sz != i->sz ||
		    memcmp(i->keyvalue, expect->keyvalue, i->sz) != 0)
		{
			fprintf(stderr, "%s:%d: failed %s\n"
				"\titeration expected " FMTi " got " FMTi "\n",
				file, line, expr,
				ARGi(expect), ARGi(i));
			abort();
		}
	}
	va_end(ap);
	if (i) {
		fprintf(stderr, "%s:%d: failed %s\n"
			"\titeration got unexpected key " FMTi "\n",
			file, line, expr, ARGi(i));
		abort();
	}
	if (expect) {
		fprintf(stderr, "%s:%d: failed %s\n"
			"\titeration omitted expected key " FMTi "\n",
			file, line, expr, ARGi(expect));
		abort();
	}
}
#define _S(p) #p
#define assert_store_is(...) \
	assert_store_is_(__FILE__, __LINE__, \
		"test_store" _S((__VA_ARGS__)), \
		__VA_ARGS__)

/* assert that the store contains none of the keys */
__attribute__((sentinel))
static void
assert_store_lacks_(const char *file, int line, const char *expr,
	struct store *store, const char *key0, ...)
{
	va_list ap;
	const char *key;

	va_start(ap, key0);
	for (key = key0;
	     key;
	     key = va_arg(ap, const char *))
	{
		const struct info *i = store_get(store, key);
		if (i) {
			fprintf(stderr, "%s:%d: failed %s\n"
				"\tfound non-key %s = " FMTi "\n",
				file, line, expr, key, ARGi(i));
			abort();
		}
	}
}
#define assert_store_lacks(...) \
	assert_store_lacks_(__FILE__, __LINE__, \
		"assert_store_contains_noneof_" _S((__VA_ARGS__)), \
		__VA_ARGS__)


#define assert_store_put(store, keyvalue) \
	assert(store_put(store, sizeof (keyvalue), keyvalue) != -1)

/* flexible constants! */
#define INFO(kv) \
	(&((const union { \
		struct info info; \
		struct { \
			uint16_t sz; \
			char k[sizeof (kv)]; \
		} info_; \
	}) { .info_.sz = sizeof (kv), .info_.k = kv }).info)

int
main()
{
	struct store *store;
	const char *storefile = NULL;

    /* -- empty store -- */

	/* can allocate and immediately free a store */
	store = store_open(storefile);
	assert(store);
	store_close(store);

	store = store_open(storefile);
	test_empty_store(store);

    /* -- single-entry store tests -- */

	/* can store it */
	assert_store_put(store, "key1\0value1");

	assert_store_is(store,
		INFO("key1\0value1"),
		NULL);
	assert_store_lacks(store, "", "key", "key0", "key2", "zzzzzzzz", NULL);

    /* -- a store with 2 entries -- */

	assert_store_put(store, "key2\0value2");

	assert_store_is(store,
		INFO("key1\0value1"),
		INFO("key2\0value2"),
		NULL);
	assert_store_lacks(store, "", "key", "key0", "key3", "zzzzzzzz", NULL);

    /* -- a store with 3 entries -- */

	assert_store_put(store, "key0\0value0");

	assert_store_is(store,
		INFO("key0\0value0"),
		INFO("key1\0value1"),
		INFO("key2\0value2"),
		NULL);
	assert_store_lacks(store, "", "key", "key3", "zzzzzzzz", NULL);

    /* -- we can't delete a prefix of an existing key */

	assert(!store_del(store, "key"));
	assert(!store_del(store, ""));

    /* -- or an overlong key, or something weird */

	assert(!store_del(store, "key00"));
	assert(!store_del(store, "value0"));

    /* -- deleting null is OK */

	assert(!store_del(store, NULL));

    /* -- delete each key (twice) and add it back again (twice),
          replacing first with a different value, then put the original
          back again -- */

	assert(store_del(store, "key0"));
	assert(!store_del(store, "key0"));
	assert_store_is(store,
		INFO("key1\0value1"),
		INFO("key2\0value2"),
		NULL);
	assert(!store_get(store, "key0"));
	assert_store_put(store, "key0\0value0!");
	assert_store_is(store,
		INFO("key0\0value0!"),
		INFO("key1\0value1"),
		INFO("key2\0value2"),
		NULL);
	assert_store_put(store, "key0\0value0");
	assert_store_is(store,
		INFO("key0\0value0"),
		INFO("key1\0value1"),
		INFO("key2\0value2"),
		NULL);

	assert(store_del(store, "key1"));
	assert(!store_del(store, "key1"));
	assert_store_is(store,
		INFO("key0\0value0"),
		INFO("key2\0value2"),
		NULL);
	assert(!store_get(store, "key1"));
	assert_store_put(store, "key1\0value1!");
	assert_store_is(store,
		INFO("key0\0value0"),
		INFO("key1\0value1!"),
		INFO("key2\0value2"),
		NULL);
	assert_store_put(store, "key1\0value1");
	assert_store_is(store,
		INFO("key0\0value0"),
		INFO("key1\0value1"),
		INFO("key2\0value2"),
		NULL);

	assert(store_del(store, "key2"));
	assert(!store_del(store, "key2"));
	assert_store_is(store,
		INFO("key0\0value0"),
		INFO("key1\0value1"),
		NULL);
	assert(!store_get(store, "key2"));
	assert_store_put(store, "key2\0value2!");
	assert_store_is(store,
		INFO("key0\0value0"),
		INFO("key1\0value1"),
		INFO("key2\0value2!"),
		NULL);
	assert_store_put(store, "key2\0value2");
	assert_store_is(store,
		INFO("key0\0value0"),
		INFO("key1\0value1"),
		INFO("key2\0value2"),
		NULL);

    /* -- cleanup -- */
	store_close(store);
}
