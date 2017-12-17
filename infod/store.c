#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "store.h"

/* #define DEBUG */
#ifdef DEBUG
# include <stdio.h>
# define dprintf(...) fprintf(stderr, __VA_ARGS__)
#else
# define dprintf(...) /* nothing */
#endif

/*
 * The store has two parts:
 *  - a file-backed page array containing the fragmented/compacted
 *    key-value infos, and
 *  - a heap-allocated pointer array kept in sorted order, that
 *    points into the file-backed info area.
 *
 * The page array is loaded from disk as crash recovery.
 * On insertions, the page array is incrementally compacted by
 * bubbling up 'holes'.
 */

#define INFO_ALIGN		2
#define MIN(a,b)  ((a) < (b) ? (a) : (b))

#define STORE_INCREMENT		64	/* store.max's growth rate */
struct store {
	/* backing file */
	int fd;				/* fd to backing file */
	char *filebase;			/* mapped backing file */
	uint32_t filesz;		/* mapped extent */
	uint32_t pagesize;		/* file increment size */

	uint32_t hole;			/* first hole's offset */
	uint32_t holesz;		/* first hole's size in bytes */
	uint32_t space;			/* space at end of file */

	/* Sorted array of pointers into the filestore */
	unsigned int n;
	unsigned int max;
	struct info **info;		/* TODO use an AVL tree */
};

static unsigned int store_find(const struct store *store, const char *key);

/* Round up n to an alignment boundary, if it isn't on one already. */
static size_t
roundup(size_t n, size_t align)
{
	size_t remainder = n % align;
	return remainder ? n + (align - remainder) : n;
}

/* Return a pointer to the aligned position after a packed,
 * flexible-length info record. */
static char *
info_after(struct info *info, uint16_t sz)
{
	char *start = (char *)info;
	char *end = &info->keyvalue[sz];
	return start + roundup(end - start, INFO_ALIGN);
}

/* Returns the offset after an info */
static uint32_t
info_offset_after(struct store *store, struct info *info, uint16_t sz)
{
	return info_after(info, sz) - store->filebase;
}

/* Returns the offset after an info */
static uint32_t
info_offset(struct store *store, struct info *info)
{
	return (char *)info - store->filebase;
}

static uint32_t
info_allocsz(uint16_t sz)
{
	return info_after(NULL, sz) - (char *)NULL;
}

/* Return true if the hole is at the end of the mapped file */
static int
hole_is_at_end(struct store *store)
{
	return store->hole + store->holesz == store->filesz;
}

/* Ensure that slot store->info[i] exists,
 * expanding store->max as needed.
 * Return -1 on allocation error. */
static int
store_ensure(struct store *store, unsigned int i)
{
	struct info **new_info;
	unsigned int new_max;

	if (i < store->max)
		return 0;
	new_max = roundup(i + 1, STORE_INCREMENT);
	new_info = realloc(store->info, new_max * sizeof *new_info);
	if (!new_info)
		return -1;
	store->max = new_max;
	store->info = new_info;
	return 0;
}

/* Inserts a new, uninitialized cell at store->info[i],
 * shifting up the higher cells to keep them in sorted order.
 * Return -1 on allocation error */
static int
store_insert(struct store *store, unsigned int i)
{
	unsigned int n = store->n;

	if (store_ensure(store, n) == -1)
		return -1;
	if (i != n)
		memmove(&store->info[i + 1], &store->info[i],
			sizeof store->info[0] * (store->n - i));
	++store->n;
	store->info[i] = NULL; /* temporary */
	return i;
}

/* Removes the cell at store->info[i], shifting higher cells down. */
void
store_uninsert(struct store *store, unsigned int i)
{
	--store->n;
	memmove(&store->info[i], &store->info[i + 1],
		sizeof store->info[0] * (store->n - i));
}

static void
store_hole_scan(struct store *store, char *from)
{
	/* Seek to the next hole */
	struct info *file_end;
	struct info *next;
	char *hole_begin;
	char *hole_end;

	file_end = (struct info *)(store->filebase + store->filesz);
	next = (struct info *)from;

	while (next < file_end && next->sz)
		next = (struct info *)info_after(next, next->sz);
	hole_begin = (char*)next;

	while (next < file_end && !next->sz)
		next = (struct info *)info_after(next, 0);
	hole_end = (char *)next;

	store->hole = hole_begin - store->filebase;
	store->holesz = hole_end - hole_begin;

	dprintf(" store_hole_scan: found hole %u:%u\n", store->hole,
		store->hole + store->holesz);
}

/* Update store->hole and store->space */
static void
add_hole(struct store *store, uint32_t from, uint32_t to)
{
	if (from == to)
		return;
	dprintf(" add_hole: %u:%u\n", from, to);
	if (to + store->space == store->filesz)
		store->space = store->filesz - from;
	if (store->holesz == 0 || to < store->hole) {
		store->hole = from;
		store->holesz = to - from;
	} else if (to == store->hole) {
		store->holesz += to - from;
		store->hole = from;
	} else if (from == store->hole + store->holesz) {
		store->holesz = to - store->hole;
	}
}

/* Zero out a range within store->filebase */
static void
make_hole(struct store *store, void *fromp, void *top)
{
	uint32_t from_offset = (char *)fromp - store->filebase;
	uint32_t to_offset = (char *)top - store->filebase;

	add_hole(store, from_offset, to_offset);
	dprintf(" clear %u:%u\n", from_offset, to_offset);
	memset(fromp, 0, to_offset - from_offset);
}

static void
remove_hole(struct store *store, void *fromp, void *top)
{
	uint32_t from_offset = (char *)fromp - store->filebase;
	uint32_t to_offset = (char *)top - store->filebase;
	uint32_t sz = to_offset - from_offset;

	dprintf(" remove_hole %u:%u\n", from_offset, to_offset);
	if (store->filesz - to_offset < store->space)
		store->space = store->filesz - to_offset;
	if (store->hole == from_offset) {
		store->hole += sz;
		store->holesz -= sz;
		if (!store->holesz)
			store_hole_scan(store, top);
	} else if (store->hole < from_offset &&
		   store->hole + store->holesz >= to_offset)
	{
		store->holesz = from_offset - store->hole;
	}
}

static int
info_compar(const void *av, const void *bv)
{
	const struct info * const *a = av;
	const struct info * const *b = bv;

	return strcmp((*a)->keyvalue, (*b)->keyvalue);
}

/* Load or initialize a backing file */
static int
store_file_open(struct store *store, int fd)
{
	struct stat st;
	uint32_t filesz;
	char *filebase;
	uint32_t offset;
	int i;

	/* Open the file and find its physical size */
	if (fstat(fd, &st) == -1)
		return -1;
	if (st.st_size > UINT32_MAX) {
		errno = ENOSPC;
		return -1;
	}
	store->pagesize = getpagesize();

	/* Expand the file up to a page boundary if needed */
	if (st.st_size < store->pagesize)
		filesz = store->pagesize;
	else
		filesz = roundup(st.st_size, store->pagesize);
	if (filesz > st.st_size) {
		if (ftruncate(fd, filesz) == -1)
			return -1;
	}

	/* Map in file */
	filebase = mmap(NULL, filesz, PROT_READ | PROT_WRITE, MAP_SHARED,
		fd, 0);
	if ((void *)filebase == MAP_FAILED)
		return -1;

	/* After this point we are committed and can only return 0 */
	store->fd = fd;
	store->filebase = filebase;
	store->filesz = filesz;

	/* If we'd increased the file's size, zero out that part right now */
	if (filesz > st.st_size)
		memset(filebase + st.st_size, 0, filesz - st.st_size);

	/* Scan the file looking for (unordered) info chunks. */
	store->n = 0;
	store->hole = filesz;
	store->holesz = 0;
	store->space = filesz;
	offset = 0;
	while (offset <= filesz) {
		struct info *info = (struct info *)(filebase + offset);
		uint32_t next_offset = info_after(info, info->sz) - filebase;

		if (!info->sz) {
			/* Discovered hole */
			while (next_offset < filesz) {
				if (((struct info *)(filebase +
					    next_offset))->sz)
					break;
				next_offset += INFO_ALIGN;
			}
			add_hole(store, offset, next_offset);
			offset = next_offset;
			continue;
		}
		if (next_offset > filesz)
			break; /* size exceeds file: bad info */
		if (!memchr(info->keyvalue, 0, info->sz))
			break; /* keyvalue didn't contain a NUL: bad info */
		/* Discovered good info */
		i = store_insert(store, store->n);
		if (i == -1)
			break; /* no space left in list */
		/* remove_hole(store, info, next_offset + filebase); */
		store->info[i] = info;
		offset = next_offset;
		store->space = filesz - offset;
	}
	if (offset < filesz) {
		/* Hole at end of file */
		make_hole(store, filebase + offset, filebase + filesz);
	}

	dprintf("store_file_open: n=%u filesz=%u space=%u hole=%u:%u\n",
		store->n, store->filesz, store->space,
		store->hole, store->hole + store->holesz);

	/* Sort the found infods (there may be duplicates) */
	qsort(store->info, store->n, sizeof store->info[0], info_compar);

	/* De-duplicate the infos */
	i = 1;
	while (i < store->n) {
		struct info *info = store->info[i];
		if (strcmp(store->info[i-1]->keyvalue, info->keyvalue) != 0) {
			i++;
			continue;
		}
		dprintf("store_file_open: removed duplicate %s\n",
			info->keyvalue);
		make_hole(store, info, info_after(info, info->sz));
		store_uninsert(store, i);
	}

	return 0;
}

static void
store_file_close(struct store *store)
{
	if (store->filebase)
		munmap(store->filebase, store->filesz);
	store->filebase = NULL;
}

/* Change the size of the backing file.
 * Adjusts the hole if needed */
static int
store_set_filesize(struct store *store, uint32_t new_filesz)
{
	char *new_base;
	char *old_base = store->filebase;
	uint32_t old_filesz = store->filesz;
	unsigned int i;

	if (new_filesz > old_filesz) {
		/* Grow the file first */
		if (posix_fallocate(store->fd, old_filesz,
		    new_filesz - old_filesz) == -1)
			return -1;
	}

	/* Create a second mapping before releasing the current one. */
	new_base = mmap(NULL, new_filesz, PROT_READ | PROT_WRITE, MAP_SHARED,
		store->fd, 0);
	if ((void *)new_base == MAP_FAILED)
		return -1;

	/* Switch over to the new mapping */
	store->filebase = new_base;
	store->filesz = new_filesz;
	(void) munmap(old_base, old_filesz);
	/* Adjust the sorted pointers to use the new mapping */
	for (i = 0; i < store->n; i++)
		if (store->info[i])
			store->info[i] = (struct info *)(new_base +
				((char *)store->info[i] - old_base));

	if (new_filesz < old_filesz) {
		/* Shrink the file */
		if (ftruncate(store->fd, new_filesz) == -1)
			new_filesz = old_filesz; /* failed to shrink? */
		if (store->hole > new_filesz) {
			store->hole = new_filesz;
			store->holesz = 0;
		} else if (store->hole + store->holesz > new_filesz)
			store->holesz = new_filesz - store->hole;
	} else if (new_filesz > old_filesz) {
		/* Zero the end of the file */
		memset(new_base + old_filesz, 0, new_filesz - old_filesz);
		add_hole(store, old_filesz, new_filesz);
	}
	store->filesz = new_filesz;
	return 0;
}

/* Move the early hole up, towards the end of the file, by
 * moving an info record down into its place.
 * Never alters filebase mapping, but may alter some store->info[] */
static void
store_file_compact1(struct store *store)
{
	char *dst;
	char *src;
	char *src_end;
	struct info *info;
	unsigned int i;
	uint32_t infosz;
	char *hole_end;
	char *file_end;
	uint16_t dirtysz;

	if (hole_is_at_end(store))
		return;

	dst = store->filebase + store->hole;
	src = store->filebase + store->hole + store->holesz;
	info = (struct info *)src;
	src_end = info_after(info, info->sz);
	infosz = src_end - src;

	/* Find the sorted pointer we'll need to update */
	i = store_find(store, info->keyvalue);
	if (i >= store->n || store->info[i] != info)
		abort();

	/* Move it down */
	memmove(dst, src, src_end - src);

	/* Zero out the dirty leftover */
	dirtysz = MIN(store->holesz, infosz);
	memset(src_end - dirtysz, 0, dirtysz);

	/* adjust store->info[i] and the store->hole pointer */
	store->info[i] = (struct info *)dst;
	store->hole += infosz;

	/* Discover and coallesce next hole */
	hole_end = store->filebase + (store->hole + store->holesz);
	file_end = store->filebase + store->filesz;
	while (hole_end < file_end) {
		info = (struct info *)hole_end;
		if (info->sz)
			break;
		hole_end = info_after(info, 0);
	}
	store->holesz = hole_end - (store->filebase + store->hole);
}

/* Trim off excess pages from the mapped file. */
static void
store_file_trim_excess(struct store *store)
{
	if (hole_is_at_end(store)) {
		/* The excess is the set of empty whole pages at the
		 * end of the file. If we have more than two pages
		 * of excess, trim all but one (hysteresis). */
		uint32_t trimmedsz = roundup(store->hole, store->pagesize) +
			store->pagesize;
		if (store->filesz > trimmedsz)
			store_set_filesize(store, trimmedsz);
	}
}

static void
store_file_compact(struct store *store)
{
	store_file_compact1(store);
	store_file_trim_excess(store);
}

/* Allocate an info from the hole.
 * Returns NULL if there is no space.
 * Returns pointer to a new info with info->sz set to sz*/
static struct info *
store_hole_alloc(struct store *store, uint16_t sz)
{
	struct info *info;
	uint32_t allocsz = info_allocsz(sz);

	if (!sz) {
		errno = EINVAL;
		return NULL;
	}

	while (store->holesz < allocsz) {
		if (hole_is_at_end(store)) {
			uint32_t new_size = roundup(store->hole +
				allocsz, store->pagesize);
			if (store_set_filesize(store, new_size) == -1)
				return NULL;
		} else
			store_file_compact1(store);
	}

	info = (struct info *)(store->filebase + store->hole);
	info->sz = sz;
	remove_hole(store, info, info_after(info, sz));
	if (!store->holesz)
		store_file_compact(store);
	return info;

}

/*
 * Resize an existing store->info[i], being careful to not lose any data
 * should an allocation fail.
 * The content of the resized info will be undefined.
 * Returns NULL if we can't grow the file mapping.
 */
static struct info *
store_info_realloc(struct store *store, unsigned int i, uint16_t new_sz)
{
	struct info *new_info;
	struct info *info = store->info[i];
	uint16_t old_sz = info->sz;
	uint32_t new_alloc = info_allocsz(new_sz);
	uint32_t old_alloc = info_allocsz(old_sz);
	uint32_t grow;
	uint32_t old_end;
	char *p;
	char *new_after;
	char *old_after;

	if (new_alloc == old_alloc) {
		/* The allocation can stay the same size */
		info->sz = new_sz;
		return info;
	}
	if (new_alloc < old_alloc) {
		/* Shrinking allocation: new_sz < old_sz */
		make_hole(store, info_after(info, new_sz),
			info_after(info, old_sz));
		info->sz = new_sz;
		store_file_trim_excess(store);
		return store->info[i];
	}

	/* Growing allocation; see if we happen to be followed
	 * by enogh empty space to grow into. */
	grow = new_alloc - old_alloc;
	old_after = info_after(info, old_sz);
	new_after = old_after + grow;
	for (p = old_after; p < new_after; p += sizeof (struct info))
		if (((struct info *)p)->sz)
			break;
	if (p == new_after) {
		remove_hole(store, old_after, new_after);
		info->sz = new_sz;
		return info;
	}

	/* Compact this and earlier info records. */
	if (store->hole < info_offset(store, info)) {
		do {
			store_file_compact1(store);
		} while (store->hole < info_offset(store, store->info[i]));
		/* At this point info has moved backward,
		 * and a hole has opened up in front of us */
		info = store->info[i];
		old_after = info_after(info, old_sz);
		new_after = old_after + grow;
	}
	old_end = info_offset_after(store, info, old_sz);

	if (old_end == store->hole) {
		/* There is now a hole immediately following us, as big as
		 * we could compact. */
		uint32_t new_end = old_end + grow;
		if (new_end > store->filesz && hole_is_at_end(store)) {
			/* The hole is at the end of the file, and
			 * it is still not big enough. Because there is
			 * nothing left to compacted, there is no other
			 * option but to grow the backing file. */
			uint32_t new_filesz = roundup(new_end,store->pagesize);
			if (store_set_filesize(store, new_filesz) == -1)
				return NULL;
			/* Re-mapping the file may have changed pointers: */
			info = store->info[i];
			old_end = info_offset_after(store, info, old_sz);
			/* Fallthrough to one of the next two branches */
			assert(store->holesz >= grow);
		}
		if (store->holesz >= grow) {
			/* The hole is big enough to grow into */
			info->sz = new_sz;
			remove_hole(store, info, info_after(info, new_sz));
			return info;
		}
	}

	/* At this point we abandon being able to re-use the
	 * existing info. The strategy now changes to allocating a
	 * new info. We take this opportunity to compact the data;
	 * even though we will leave a gap later. */
	while (!hole_is_at_end(store)) {
		store_file_compact1(store);
		/* No need to recompute info, because we guaranteed
		 * earlier that the hole > info */
	}

	if (new_alloc < store->holesz) {
		/* The hole at the end is not big enough to allocate from,
		 * so, make the file bigger */
		uint32_t new_end = store->hole + new_alloc;
		uint32_t new_filesz = roundup(new_end,store->pagesize);
		if (store_set_filesize(store, new_filesz) == -1)
			return NULL;
		/* Re-mapping the file may have changed pointers: */
		info = store->info[i];
	}

	new_info = (struct info *)(store->filebase + store->hole);
	new_info->sz = new_sz;
	remove_hole(store, new_info, info_after(new_info, new_sz));
	make_hole(store, info, info_after(info, old_sz));
	store->info[i] = new_info;
	return new_info;
}

static void
store_info_free(struct store *store, unsigned int i)
{
	struct info *info = store->info[i];

	dprintf("del #%u \"%s\" @ %u:%u\n", i, info->keyvalue,
		info_offset(store, info),
		info_offset_after(store, info, info->sz));

	make_hole(store, info, info_after(info, info->sz));
	store->info[i] = NULL;
}

struct store *
store_open(const char *filename)
{
	struct store *store;
	int fd;

	if (!filename) {
		errno = EINVAL;
		return NULL;
	}
	store = malloc(sizeof *store);
	if (!store)
		return NULL;
	store->n = 0;
	store->max = 0;
	store->info = NULL;
	store->filebase = NULL;
	store->fd = -1;

	fd = open(filename, O_RDWR | O_CREAT, 0666);
	if (fd == -1)
		goto fail;
	if (flock(fd, LOCK_EX | LOCK_NB) == -1)
		goto fail;
	if (store_file_open(store, fd) == -1)
		goto fail;
	fd = -1;

	return store;
fail:
	if (fd != -1)
		close(fd);
	store_close(store);
	return NULL;
}

void
store_close(struct store *store)
{
#if 0
	for (unsigned int i = 0; i < store->n; i++)
		free(store->info[i]);
#endif
	free(store->info);
	store_file_close(store);
	if (store->fd != -1)
		close(store->fd);
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

	/* See if we are replacing an existing key */
	i = store_find(store, keyvalue);
	if (store_eq(store, i, keyvalue)) {
		/* Resize the existing info (it may move) */
		info = store_info_realloc(store, i, sz);
		if (!info)
			return -1;
	} else {
		info = store_hole_alloc(store, sz);
		if (!info)
			return -1;
		if (store_insert(store, i) == -1) {
			make_hole(store, info, info_after(info, sz));
			return -1;
		}
		store->info[i] = info;
	}

	dprintf("put \"%s\" @ %u:%u\n", keyvalue,
		info_offset(store, info), info_offset_after(store, info, sz));
	memcpy(info->keyvalue, keyvalue, sz);
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
	store_info_free(store, i);
	store_uninsert(store, i);
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

