#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "store.h"

/* #define DEBUG 1 */
#ifdef DEBUG
# include <stdio.h>
# define dprintf(...) fprintf(stderr, __VA_ARGS__)
#else
# define dprintf(...) /* nothing */
#endif

#define offsetof(T, f) ((size_t)&((T *)0)->f)

/*
 * Implements fast, compact storage for <sz,key\0data> elements
 * (sz<64kB) called 'infos'.
 *
 *
 * Objectives
 *
 * The primary requirement is holding a stable set of keys with
 * slowly changing/growing values (e.g. counters or state strings).
 * Most of the operations on this store will be searching & reading.
 *
 * The secondary requirement is surving process kills and restarts.
 * We want this storage to be robust enough to restart and recover
 * best effort without assistance. If the case of corrupted keys,
 * some loss is tolerable. The store need not survive system restarts.
 *
 *
 * Design
 *
 * We keep the sorted file index on the heap and keep the info
 * data within the file-mapped pages. The file should be stored in
 * a ramdisk and is a multiple of pages. The sorted index is
 * reconstructed on load/recovery.
 *
 * The file contains a sequence of 8-byte-aligned records.
 * The first two bytes of a record determine the record's type:
 * either a data record, or a gap record.
 *
 *   Data:     uint16 sz            number of bytes following
 *             char   keyvalue[sz]
 *             char   empty[*]      pad to next 8 byte boundary
 *
 *   Gap:      uint16 sz            0
 *             uint16 count         number of gap records that follow
 *             char   empty[4]      pad to next 8 byte boundary
 *
 * An expected common case is to reallocate the element at the
 * end of the file.
 *
 *
 * Allocation strategy
 *
 *   Let `s` be the space at the end of the file.
 *   Let `dd` be the new allocation size required.
 *
 * 1. (dd<=s) Easy allocate from the space at the end of the file.
 *
 *      +-------.-------.-------+
 *      | page  : page  : page  |        file use in pages
 *      +-------'-------'-+-----+
 *      | data            |   s |        before allocation
 *      +-----------------+--+--+
 *      | data            :dd|s'|        after allocation 'dd'
 *      +-----------------+--+--+
 *
 * 2. (dd>s) Repack the data, then retry.
 *
 *    NB: If an entry is being reallocated (growing), ensure
 *    that it moves to the end of the (packed) data first, to
 *    anticipate failure due to ENOSPC.
 *
 *    Let `s'` be the space available after repack.
 *
 * 2a. (dd>s'), flie is undersized: allocate new file pages
 *
 *      +-------.-------.-------+
 *      | page  : page  : page  |          file use in pages
 *      +-------'-------'---+---+
 *      | packed data       | s |          data previously packed
 *      +-------------------+---+--+
 *      | packed data       :  dd  |       allocation dd>s won't fit
 *      +-------.-----+-.---+---.--+----+
 *      | page  : page  : page  : page  |  extend file
 *      +-------'-------'---+---+-------+
 *      | packed data       | s | free  |
 *      +-------------------+---+-------+
 *      | packed data       |    s'     |
 *      +-------------------+------+----+
 *      | packed data       :  dd  | s" |  allocation fits
 *      +-------------------+------+----+
 *
 * 2b. (dd+2p<=s') sufficient space avail after repack; becomes
 *                 same as case 1.
 *
 *      +-------.-------.-------+
 *      | page  : page  : page  |        file use in pages
 *      +-------'-------'---+---+
 *      | data              | s |        unpacked data and space
 *      +-------------------+---+--+
 *      | data              :  dd  |     allocation dd>s won't fit
 *      +-------------+-----+---+--+
 *      | packed data |    s'   |        space grows after packing
 *      +-------------+------+--+
 *      | packed data :  dd  |s"|        allocation now fits
 *      +-------------+------+--+
 *
 * 2c. If, after packing and allocating there would be
 *    more than 2*pagesize bytes free, release the excess pages.
 *    The 2 pages is a hysteresis gap.
 *
 *      +-------.-------.-------.-------+
 *      | page  : page  : page  : page  |
 *      +-------'-------'-------'-----+-+
 *      | data                        |s|
 *      +---------+-------------------+-+
 *      | packed  |          s'         |
 *      +---------+--+------------------+
 *      | packed  :dd|        s"        |  after alloc, s > 2pagesz
 *      +---------+--+----------.-------+
 *      | packed  :dd|     s"'  :  free |
 *      +---------+--+----------.-------+
 *      | page  : page  : page  :          truncate file
 *      +-------.-------.-------+
 */

#define MIN(a,b)  ((a) < (b) ? (a) : (b))
#define MAX(a,b)  ((a) > (b) ? (a) : (b))

#define STORE_INCREMENT		64	/* store.max's growth rate */
struct store {
	/* Backing file */
	int fd;				/* fd to backing file */
	char *filebase;			/* mapped backing file */
	uint32_t filesz;		/* mapped extent */
	uint32_t pagesize;		/* file increment size */
	uint32_t space;			/* offset to space at end of file */

	/* Sorted index of pointers into the filestore */
	unsigned int n;
	unsigned int max;
	struct info **index;		/* TODO use an AVL tree */
};

/* Minimum size of an element (info or gap) */
#define INFO_ALIGN	8

/* Memory layout of an 8-byte gap record */
struct gap {
	uint16_t zero1;
	uint16_t zero2;
	uint32_t size;		/* Size of this gap in bytes */
};

union record {
	struct info info;
	struct gap gap;
};

static unsigned int store_find(const struct store *store, const char *key);

/* Rounds n up to an alignment boundary, if it isn't on one already.
 * align must be a power of 2. */
static uint32_t
roundup(uint32_t n, uint32_t align)
{
	return (n + (align - 1)) & ~(align - 1);
}

/* Ensure that slot store->index[i] exists,
 * expanding store->max as needed.
 * Return -1 on allocation error. */
static int
store_index_ensure(struct store *store, unsigned int i)
{
	struct info **new_info;
	unsigned int new_max;

	if (i < store->max)
		return 0;
	new_max = roundup(i + 1, STORE_INCREMENT);
	new_info = realloc(store->index, new_max * sizeof *new_info);
	if (!new_info)
		return -1;
	store->max = new_max;
	store->index = new_info;
	return 0;
}

/* Inserts a new, uninitialized cell at store->index[i],
 * shifting up the higher cells to keep them in sorted order.
 * Return -1 on allocation error */
static int
store_index_insert(struct store *store, unsigned int i)
{
	unsigned int n = store->n;

	if (store_index_ensure(store, n) == -1)
		return -1;
	if (i != n)
		memmove(&store->index[i + 1], &store->index[i],
			sizeof store->index[0] * (store->n - i));
	++store->n;
	store->index[i] = NULL; /* unnecessary, but reveals bugs */
	return i;
}

/* Removes the cell at store->index[i], shifting higher cells down. */
void
store_index_delete(struct store *store, unsigned int i)
{
	--store->n;
	memmove(&store->index[i], &store->index[i + 1],
		sizeof store->index[0] * (store->n - i));
}

/* Compares two index entries. Used to sort the index by key */
static int
info_compar(const void *av, const void *bv)
{
	const struct info * const *a = av;
	const struct info * const *b = bv;

	return strcmp((*a)->keyvalue, (*b)->keyvalue);
}



/* Size of an info given it's .sz field */
static uint32_t
info_size(uint16_t sz)
{
	return roundup(offsetof(struct info, keyvalue[sz]), INFO_ALIGN);
}

static int
record_is_gap(const union record *record)
{
	return record->info.sz == 0;
}

static void
record_init_gap(union record *record, uint32_t nbytes)
{
	assert(nbytes >= INFO_ALIGN);
	record->gap.zero1 = 0;
	record->gap.zero2 = 0;
	record->gap.size = nbytes - INFO_ALIGN;
}

static uint32_t
record_get_size(const union record *record)
{
	return record_is_gap(record)
		? roundup(record->gap.size, INFO_ALIGN) + INFO_ALIGN
		: info_size(record->info.sz);
}

/* Sets the space pointer, and writes a sentinel gap */
static void
store_set_space(struct store *store, uint32_t space)
{
	store->space = space;
	if (space != store->filesz) {
		union record *sentinel =
			(union record *)(store->filebase + space);
		record_init_gap(sentinel, store->filesz - space);
	}
}

/* Convert an info into a gap. Might rewind store->space. */
static void
info_make_gap(struct store *store, struct info *info)
{
	char *filebase = store->filebase;
	uint32_t filesz = store->filesz;
	uint32_t offset = (char *)info - filebase;
	uint32_t next_offset = offset + info_size(info->sz);
	union record *record = (union record *)info;

	while (next_offset < filesz) {
		union record *next_record =
			(union record *)(filebase + next_offset);
		uint32_t record_sz = record_get_size(next_record);
		if (!record_is_gap(next_record))
			break;
		/* Be careful with integer overflow */
		if (next_offset > filesz - record_sz)
			next_offset = filesz;
		else
			next_offset += record_sz;
	}
	if (next_offset > filesz)
		next_offset = filesz;
	record_init_gap(record, next_offset - offset);
	if (next_offset == filesz)
		store_set_space(store, offset);
}

/* Repack the file, and rebuild the sorted index. */
static void
store_repack(struct store *store)
{
	uint32_t offset;
	uint32_t w_offset;
	uint32_t space = store->space;
	uint32_t filesz = store->filesz;
	char *filebase = store->filebase;
	unsigned int i;

	dprintf("repacking: n=%u space=0x%08" PRIx32
		" filesz=0x%" PRIx32 "\n",
		store->n, store->space, store->filesz);

	/* Scan 0..space copying down data */
	offset = 0;
	w_offset = 0;
	i = 0;
	while (offset < space) {
		const union record *record =
			(const union record *)(filebase + offset);
		uint32_t recordsz = record_get_size(record);
		if (!record_is_gap(record)) {
			struct info *w_info =
				(struct info *)(filebase + w_offset);
			dprintf(" 0x%08" PRIx32 "<-0x%08" PRIx32
			        " sz=0x%" PRIx32 " key=\"%.30s\"\n",
				w_offset, offset, recordsz, w_info->keyvalue);
			if (w_offset != offset)
				memmove(w_info, record, recordsz);
			store_index_ensure(store, i);
			store->index[i++] = w_info;
			w_offset += recordsz;
		}
		offset += recordsz;
	}
	space = w_offset;
	assert(space <= store->space);
	if (space < store->filesz)
		record_init_gap((union record *)(filebase + space),
			filesz - space);
	store_set_space(store, space);
	store->n = i;
	qsort(store->index, store->n, sizeof store->index[0], info_compar);

	dprintf("repacked:  n=%u space=0x%08" PRIx32 " filesz=0x%" PRIx32 "\n",
		store->n, store->space, store->filesz);
}

/* Load or initialize a backing file */
static int
store_file_open(struct store *store, int fd)
{
	struct stat st;
	uint32_t filesz;
	char *filebase;
	uint32_t offset;
	unsigned int n;
	int i;

	/* Open the file and find its physical size */
	if (fstat(fd, &st) == -1)
		return -1;
	if (st.st_size >= UINT32_MAX) {
		errno = ENOSPC;
		return -1;
	}
	store->pagesize = getpagesize();

	/* Expand the file to meet a page boundary */
	filesz = MAX(store->pagesize, roundup(st.st_size, store->pagesize));
	if (filesz > st.st_size) {
		if (ftruncate(fd, filesz) == -1)
			return -1;
	}

	dprintf("stat sz=0x%lx filesz=0x%" PRIx32 "\n", st.st_size, filesz);

	/* Map the file into the address space */
	filebase = mmap(NULL, filesz, PROT_READ | PROT_WRITE, MAP_SHARED,
		fd, 0);
	if ((void *)filebase == MAP_FAILED)
		return -1;

	/* If we'd increased the file's size, zero out that part right now */
	if (filesz > st.st_size)
		memset(filebase + st.st_size, 0, filesz - st.st_size);

	/* Scan the number of data records in the file.
	 * If corrupted entries are found, just truncate. */
	offset = 0;
	n = 0;
	while (offset < filesz) {
		union record *record = (union record *)(filebase + offset);
		uint32_t record_sz = record_get_size(record);

#if 0
		dprintf("  +0x%08" PRIx32 ": %4s sz=0x%" PRIx32 "\n",
			offset, record_is_gap(record) ? "gap" : "info",
			record_sz);
#endif

		if (offset > filesz - record_sz)
			break; /* Too big */
		if (!record_is_gap(record))
			n++;
		offset += record_sz;
	}
	if (n && store_index_ensure(store, n - 1) == -1) {
		(void )munmap(filebase, filesz);
		return -1;
	}

	/* After this point we are committed and can only return 0 */

	store->fd = fd;
	store->filebase = filebase;
	store->filesz = filesz;
	store->space = offset;

	/* Repack the store, creating the index */
	store_repack(store);

	/* De-duplicate */
	i = 1;
	while (i < store->n) {
		struct info *info = store->index[i];
		if (strcmp(store->index[i-1]->keyvalue, info->keyvalue) != 0) {
			i++;
			continue;
		}
		dprintf("store_file_open: removed duplicate %.100s\n",
			info->keyvalue);
		info_make_gap(store, info);
		store_index_delete(store, i);
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

/* Change the size of the backing file. */
static int
store_file_setsize(struct store *store, uint32_t new_filesz)
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
		if (store->index[i])
			store->index[i] = (struct info *)(new_base +
				((char *)store->index[i] - old_base));

	if (new_filesz < old_filesz) {
		/* Shrink the file */
		if (ftruncate(store->fd, new_filesz) == -1)
			new_filesz = old_filesz; /* failed to shrink? */
	} else if (new_filesz > old_filesz) {
#if 0
		/* Zero the end of the file */
		memset(new_base + old_filesz, 0, new_filesz - old_filesz);
#endif
	}
	store->filesz = new_filesz;
	return 0;
}

/* Trim off excess pages from the mapped file. */
static void
store_file_trim(struct store *store)
{
	uint32_t space = store->space;
	uint32_t filesz = store->filesz;
	uint32_t maxfilesz = roundup(space + 3 * store->pagesize, store->pagesize);

	if (filesz > maxfilesz) {
		union record *rec;
		uint32_t newfilesz = roundup(space + store->pagesize,
			store->pagesize);
		store_file_setsize(store, newfilesz);
		rec = (union record *)(store->filebase + space);
		record_init_gap(rec, store->filesz - space);
	}
}

static struct info *
store_file_alloc(struct store *s, uint16_t sz)
{
	struct info *info;
	uint32_t allocsz = info_size(sz);

	if (allocsz > s->filesz - s->space)
		store_repack(s);
	if (allocsz > s->filesz - s->space) {
		uint32_t newfilesz;

		if (s->filesz >= UINT32_MAX - allocsz) {
			errno = ENOSPC;
			return NULL;
		}
		newfilesz = roundup(s->space + allocsz, s->pagesize);
		if (store_file_setsize(s, newfilesz) == -1)
			return NULL;
	}

	assert(allocsz <= s->filesz - s->space);
	info = (struct info *)(s->filebase + s->space);
	s->space += allocsz;
	if (s->space < s->filesz) {
		union record *rec;
		rec = (union record *)(s->filebase + s->space);
		record_init_gap(rec, s->filesz - s->space);
	}
	info->sz = sz;
	return info;
}

/* Converts an info into a gap */
static void
store_file_dealloc(struct store *store, struct info *info)
{
	uint32_t offset = (char *)info - store->filebase;
	uint32_t gapsz = info_size(info->sz);
	uint32_t after_offset = offset + gapsz;
	union record *after_record;

	if (after_offset == store->space) {
		store_set_space(store, offset);
		store_file_trim(store);
	} else {
		after_record =
			(union record *)(store->filebase + after_offset);
		if (record_is_gap(after_record))
			gapsz += record_get_size(after_record);
		record_init_gap((union record *)info, gapsz);
	}
}

/*
 * Resize an existing store->index[i], being careful to not lose any data
 * should an allocation fail.
 * The content of the resized info will be undefined.
 * Returns NULL if we can't grow the file mapping.
 */
static struct info *
store_info_realloc(struct store *store, unsigned int i, uint16_t new_sz)
{
	struct info *info = store->index[i];
	uint32_t offset = (char *)info - store->filebase;
	uint16_t old_sz = info->sz;
	uint32_t new_alloc = info_size(new_sz);
	uint32_t old_alloc = info_size(old_sz);
	uint32_t grow;
	union record *after_record;

	assert(offset < store->filesz);
	assert((offset % INFO_ALIGN) == 0);

	if (new_alloc == old_alloc) {
		/* The allocation can stay the same size */
		info->sz = new_sz;
		return info;
	}

	/* Find the record following (old) 'info' */
	if (offset + old_alloc == store->space)
		after_record = NULL;
	else
		after_record =
			(union record *)(store->filebase + offset + old_alloc);

	if (new_alloc < old_alloc) {
		/* Shrinking allocation: new_sz < old_sz */
		info->sz = new_sz;
		if (!after_record) {
			/* Space grows backwards */
			store_set_space(store, offset + new_alloc);
			store_file_trim(store);
			return store->index[i]; /* (may have remapped) */
		} else {
			/* Create new gap */
			uint32_t gap_offset = offset + new_alloc;
			union record *new_gap =
				(union record *)(store->filebase + gap_offset);
			uint32_t new_gap_size = old_alloc - new_alloc;
			if (record_is_gap(after_record)) {
				/* Merge with gap afterward */
				new_gap_size += record_get_size(after_record);
			}
			record_init_gap(new_gap, old_alloc - new_alloc);
			return info;
		}
	}

	grow = new_alloc - old_alloc;

	/* Growing allocation; try to use a following gap to grow into */
	if (after_record && record_is_gap(after_record)) {
		uint32_t after_size = record_get_size(after_record);
		if (after_size == grow) {
			info->sz = new_sz;
			return info;
		}
		if (after_size > grow) {
			union record *new_gap =
				(union record *)(store->filebase +
				    offset + new_alloc);
			record_init_gap(new_gap, after_size - grow);
			info->sz = new_sz;
			return info;
		}
	}

	/* At this point it is clear we have to make the old info a gap */
	if (after_record && record_is_gap(after_record))
		record_init_gap((union record *)info,
			old_alloc + record_get_size(after_record));
	else
		record_init_gap((union record *)info, old_alloc);
	/* (store->index[i] is now an invalid pointer) */

	if (new_alloc < store->filesz - store->space) {
		/* A simple allocation in the space will work */
		info = store->index[i] = store_file_alloc(store, new_sz);
		return info;
	}

	/* delete index[i] properly */
	store_index_delete(store, i);
	info = store_file_alloc(store, new_sz);
	if (!info)
		return NULL;

	/* Because the index will remain sorted, and the info will
	 * have the same key, we can simply re-insert index[i].
	 * It cannot return -1 because we'd just deleted an entry */

	(void) store_index_insert(store, i);
	store->index[i] = info;
	return info;
}

static void
store_info_free(struct store *store, unsigned int i)
{
	struct info *info = store->index[i];
	store_file_dealloc(store, info);
	store->index[i] = NULL;
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
	store->index = NULL;
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
		free(store->index[i]);
#endif
	free(store->index);
	store_file_close(store);
	if (store->fd != -1)
		close(store->fd);
	free(store);
}

/* Binary search to one more than the largest index into
 * store->index[] that has a key lexicographically
 * smaller than 'key'. In other words, find the slot
 * having the same key, or where the slot would be
 * if the key were in the info[] list. */
static unsigned int
store_find(const struct store *store, const char *key)
{
	unsigned int a, b, m;
	struct info **list = store->index;

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
	return i < store->n && strcmp(key, store->index[i]->keyvalue) == 0;
}

int
store_put(struct store *store, uint16_t sz, const char *keyvalue)
{
	unsigned int i;
	struct info *info;

	/* See if we are replacing an existing key */
	i = store_find(store, keyvalue);
	if (store_eq(store, i, keyvalue)) {
		if (store->index[i]->sz == sz &&
		    memcmp(store->index[i]->keyvalue, keyvalue, sz) == 0)
			return 0;
		/* Resize the existing info (it may move) */
		info = store_info_realloc(store, i, sz);
		if (!info)
			return -1;
	} else {
		info = store_file_alloc(store, sz);
		if (!info)
			return -1;
		if (store_index_insert(store, i) == -1) {
			store_file_dealloc(store, info);
			return -1;
		}
		store->index[i] = info;
	}

	dprintf("put \"%.100s\" @ 0x%08zx\n", keyvalue,
		(char *)info - store->filebase);
	memcpy(info->keyvalue, keyvalue, sz);
	return 1;
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
	dprintf("del \"%.100s\" @ 0x%08zx\n", key,
		(char *)store->index[i] - store->filebase);
	store_info_free(store, i);
	store_index_delete(store, i);
	return 1;
}

const struct info *
store_get(struct store *store, const char *key)
{
	unsigned int i = store_find(store, key);
	if (!store_eq(store, i, key))
		return NULL;
	return store->index[i];
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
	return store->index[ix->i++];
}

