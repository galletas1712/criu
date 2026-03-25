#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>

#undef LOG_PREFIX
#define LOG_PREFIX "page-coalesce: "

#include "types.h"
#include "cr_options.h"
#include "common/list.h"
#include "image.h"
#include "image-desc.h"
#include "log.h"
#include "pagemap.h"
#include "protobuf.h"
#include "servicefd.h"
#include "xmalloc.h"
#include "images/pagemap.pb-c.h"

#define COALESCE_BATCH_PAGES 16384
#define COALESCE_WORK_PAGES 128
#define COALESCE_HASH_MIN_LOAD 4
#define COALESCE_MAX_THREADS 32
#define COALESCE_INITIAL_SLOTS (1 << 22)

static const unsigned char zero_page[PAGE_SIZE] __attribute__((aligned(64)));

struct page_hash_key {
	u64 w0;
	u64 w1;
	u64 w2;
	u64 w3;
};

struct page_hash_slot {
	struct page_hash_key key;
	u64 offset;
	bool used;
};

struct page_batch_meta {
	struct page_hash_key key;
	bool zero;
};

struct page_target {
	int pagemap_type;
	unsigned long img_id;
};

struct page_store {
	struct page_hash_slot *slots;
	size_t cap;
	size_t grow_at;
	size_t used;
	struct cr_img *blob;
	u64 next_offset;
	u64 grow_us;
	u64 blob_write_us;
};

struct chunk_group_slot {
	struct page_hash_key key;
	unsigned int group_id;
	u32 generation;
};

struct hash_batch {
	const char *pages;
	struct page_batch_meta *meta;
	unsigned int nr_pages;
	unsigned int generation;
	unsigned int done_workers;
	unsigned int nr_workers;
	unsigned int worker_start[COALESCE_MAX_THREADS];
	unsigned int worker_end[COALESCE_MAX_THREADS];
	bool stop;
	pthread_mutex_t lock;
	pthread_cond_t work_ready;
	pthread_cond_t work_done;
	u64 hash_us;
};

struct hash_worker {
	struct hash_batch *batch;
	unsigned int worker_id;
};

struct hash_pool {
	struct hash_batch batch;
	struct hash_worker *workers;
	pthread_t *threads;
	unsigned int nr_threads;
};

struct chunk_schedule {
	u32 *pages;
	size_t nr_chunks;
	size_t cap;
};

struct blob_writer {
	struct cr_img *blob;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t ready;
	pthread_cond_t idle;
	struct iovec *iovecs;
	unsigned int nr_iovecs;
	int status;
	bool stop;
	bool pending;
	bool started;
	u64 write_us;
};

struct coalesce_stats {
	u64 old_bytes;
	u64 blob_bytes;
	u64 index_bytes;
	u64 present_pages;
	u64 zero_pages;
	u64 unique_pages;
	u64 lookup_candidates;
	u64 local_duplicate_pages;
	u64 images;
	u64 total_us;
	u64 read_us;
	u64 hash_us;
	u64 lookup_us;
	u64 blob_write_us;
	u64 index_write_us;
};

struct coalesce_job {
	struct list_head list;
	int pagemap_type;
	unsigned long img_id;
};

struct compact_image {
	struct list_head list;
	u32 pages_id;
};

struct compact_image_set {
	struct list_head images;
};

struct coalesce_worker_state {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t ready;
	struct list_head jobs;
	struct compact_image_set compact;
	struct hash_pool pool;
	struct page_store store;
	struct coalesce_stats stats;
	int dfd;
	bool started;
	bool stop;
	bool failed;
};

/* One background worker consumes completed pagemap/pages pairs while dump continues. */
static struct coalesce_worker_state online_state = {
	.jobs = LIST_HEAD_INIT(online_state.jobs),
	.compact.images = LIST_HEAD_INIT(online_state.compact.images),
};

static u64 now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (u64)tv.tv_sec * 1000000ULL + (u64)tv.tv_usec;
}

static inline u64 rotl64(u64 x, int r)
{
	return (x << r) | (x >> (64 - r));
}

static inline u64 fmix64(u64 k)
{
	k ^= k >> 33;
	k *= 0xff51afd7ed558ccdULL;
	k ^= k >> 33;
	k *= 0xc4ceb9fe1a85ec53ULL;
	k ^= k >> 33;
	return k;
}

static inline u64 page_hash_step(u64 state, u64 word, u64 addend, u64 multiplier)
{
	state ^= word + addend;
	state = rotl64(state, 27);
	state *= multiplier;
	return state;
}

static bool page_is_all_zero(const void *page)
{
	return memcmp(page, zero_page, PAGE_SIZE) == 0;
}

static void compute_page_hash_key(const void *page, struct page_hash_key *out, bool *zero)
{
	const u64 *words = page;
	const u64 *end = words + PAGE_SIZE / sizeof(*words);
	u64 h0 = 0x243f6a8885a308d3ULL;
	u64 h1 = 0x13198a2e03707344ULL;

	if (page_is_all_zero(page)) {
		*zero = true;
		memzero(out, sizeof(*out));
		return;
	}

	while (words < end) {
		u64 word = *words++;

		h0 = page_hash_step(h0, word, 0x9e3779b97f4a7c15ULL, 0xc2b2ae3d27d4eb4fULL);
		h1 = page_hash_step(h1, rotl64(word, 17), 0x94d049bb133111ebULL, 0x165667b19e3779f9ULL);
	}

	*zero = false;
	h0 = fmix64(h0 ^ PAGE_SIZE);
	h1 = fmix64(h1 ^ (PAGE_SIZE << 1));
	out->w0 = h0;
	out->w1 = h1;
	out->w2 = fmix64(h0 ^ rotl64(h1, 17));
	out->w3 = fmix64(h1 ^ rotl64(h0, 31));
}

static bool page_hash_key_equal(const struct page_hash_key *left, const struct page_hash_key *right)
{
	return left->w0 == right->w0 && left->w1 == right->w1 && left->w2 == right->w2 && left->w3 == right->w3;
}

static size_t page_hash_key_slot(const struct page_hash_key *key)
{
	u64 mixed = key->w0 ^ rotl64(key->w1, 13) ^ rotl64(key->w2, 29) ^ rotl64(key->w3, 47);

	return (size_t)fmix64(mixed);
}

static int init_compat_pagemap_entry(PagemapEntry *pe)
{
	if (pe->has_in_parent && pe->in_parent)
		pe->flags |= PE_PARENT;
	else if (!pe->has_flags)
		pe->flags = PE_PRESENT;

	if (!pe->has_nr_pages)
		pe->nr_pages = pe->compat_nr_pages;

	return 0;
}

static size_t next_power_of_two(size_t value)
{
	size_t power = 1;

	while (power < value)
		power <<= 1;

	return power;
}

static int page_store_resize(struct page_store *store, size_t new_cap)
{
	struct page_hash_slot *new_slots;
	size_t i;
	u64 start_us = now_us();

	new_slots = xzalloc(new_cap * sizeof(*new_slots));
	if (!new_slots)
		return -1;

	for (i = 0; i < store->cap; i++) {
		struct page_hash_slot *slot = &store->slots[i];
		size_t idx;

		if (!slot->used)
			continue;

		idx = page_hash_key_slot(&slot->key) & (new_cap - 1);
		while (new_slots[idx].used)
			idx = (idx + 1) & (new_cap - 1);

		new_slots[idx] = *slot;
	}

	xfree(store->slots);
	store->slots = new_slots;
	store->cap = new_cap;
	store->grow_at = (new_cap * 7) / 10;
	store->grow_us += now_us() - start_us;
	return 0;
}

static int page_store_lookup_or_reserve(struct page_store *store, const struct page_hash_key *key, u64 *offset, bool *is_new)
{
	size_t idx;

	if (!store->cap) {
		if (page_store_resize(store, next_power_of_two(COALESCE_INITIAL_SLOTS)))
			return -1;
	} else if (store->used >= store->grow_at) {
		size_t new_cap = store->cap << 1;

		if (new_cap < store->cap)
			return -1;
		if (page_store_resize(store, new_cap))
			return -1;
	}

	idx = page_hash_key_slot(key) & (store->cap - 1);
	while (store->slots[idx].used) {
		if (page_hash_key_equal(&store->slots[idx].key, key)) {
			*offset = store->slots[idx].offset;
			*is_new = false;
			return 0;
		}
		idx = (idx + 1) & (store->cap - 1);
	}

	store->slots[idx].used = true;
	store->slots[idx].key = *key;
	store->slots[idx].offset = store->next_offset;
	store->used++;

	*offset = store->next_offset;
	store->next_offset += PAGE_SIZE;
	*is_new = true;
	return 0;
}

static int chunk_group_lookup_or_reserve(struct chunk_group_slot *slots, size_t cap, u32 generation,
					 const struct page_hash_key *key, unsigned int new_group_id,
					 unsigned int *group_id, bool *is_new)
{
	size_t idx;

	idx = page_hash_key_slot(key) & (cap - 1);
	while (slots[idx].generation == generation) {
		if (page_hash_key_equal(&slots[idx].key, key)) {
			*group_id = slots[idx].group_id;
			*is_new = false;
			return 0;
		}
		idx = (idx + 1) & (cap - 1);
	}

	slots[idx].generation = generation;
	slots[idx].key = *key;
	slots[idx].group_id = new_group_id;
	*group_id = new_group_id;
	*is_new = true;
	return 0;
}

static void *hash_worker_main(void *arg)
{
	struct hash_worker *worker = arg;
	struct hash_batch *batch = worker->batch;
	unsigned int generation = 0;
	unsigned int worker_id = worker->worker_id;

	for (;;) {
		u64 local_hash_us = 0;
		unsigned int start;
		unsigned int end;
		u64 start_us;

		pthread_mutex_lock(&batch->lock);
		while (!batch->stop && batch->generation == generation)
			pthread_cond_wait(&batch->work_ready, &batch->lock);

		if (batch->stop) {
			pthread_mutex_unlock(&batch->lock);
			break;
		}

		generation = batch->generation;
		start = batch->worker_start[worker_id];
		end = batch->worker_end[worker_id];
		pthread_mutex_unlock(&batch->lock);

		start_us = now_us();
		while (start < end) {
			const char *page = batch->pages + start * PAGE_SIZE;

			compute_page_hash_key(page, &batch->meta[start].key, &batch->meta[start].zero);
			start++;
		}
		local_hash_us += now_us() - start_us;

		pthread_mutex_lock(&batch->lock);
		batch->hash_us += local_hash_us;
		batch->done_workers++;
		if (batch->done_workers == batch->nr_workers)
			pthread_cond_signal(&batch->work_done);
		pthread_mutex_unlock(&batch->lock);
	}

	return NULL;
}

static int hash_pool_init(struct hash_pool *pool)
{
	long cpus;
	unsigned int i;

	memzero(pool, sizeof(*pool));
	cpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (cpus < 1)
		cpus = 1;
	pool->nr_threads = (unsigned int)cpus;
	if (pool->nr_threads > COALESCE_MAX_THREADS)
		pool->nr_threads = COALESCE_MAX_THREADS;
	if (pool->nr_threads < 2)
		return 0;

	pool->workers = xmalloc(pool->nr_threads * sizeof(*pool->workers));
	pool->threads = xmalloc(pool->nr_threads * sizeof(*pool->threads));
	if (!pool->workers || !pool->threads)
		return -1;

	pthread_mutex_init(&pool->batch.lock, NULL);
	pthread_cond_init(&pool->batch.work_ready, NULL);
	pthread_cond_init(&pool->batch.work_done, NULL);
	pool->batch.nr_workers = pool->nr_threads;

	for (i = 0; i < pool->nr_threads; i++) {
		pool->workers[i].batch = &pool->batch;
		pool->workers[i].worker_id = i;
		if (pthread_create(&pool->threads[i], NULL, hash_worker_main, &pool->workers[i]) != 0) {
			unsigned int j;

			pool->batch.stop = true;
			pthread_cond_broadcast(&pool->batch.work_ready);
			for (j = 0; j < i; j++)
				pthread_join(pool->threads[j], NULL);
			pthread_cond_destroy(&pool->batch.work_done);
			pthread_cond_destroy(&pool->batch.work_ready);
			pthread_mutex_destroy(&pool->batch.lock);
			xfree(pool->workers);
			xfree(pool->threads);
			memzero(pool, sizeof(*pool));
			pr_err("pthread_create failed for page hash worker\n");
			return -1;
		}
	}

	return 0;
}

static void hash_pool_fini(struct hash_pool *pool)
{
	unsigned int i;

	if (!pool->threads)
		return;

	pthread_mutex_lock(&pool->batch.lock);
	pool->batch.stop = true;
	pthread_cond_broadcast(&pool->batch.work_ready);
	pthread_mutex_unlock(&pool->batch.lock);

	for (i = 0; i < pool->nr_threads; i++)
		pthread_join(pool->threads[i], NULL);

	pthread_cond_destroy(&pool->batch.work_done);
	pthread_cond_destroy(&pool->batch.work_ready);
	pthread_mutex_destroy(&pool->batch.lock);
	xfree(pool->workers);
	xfree(pool->threads);
	memzero(pool, sizeof(*pool));
}

static void hash_pages_serial(const char *pages, struct page_batch_meta *meta, unsigned int nr_pages, u64 *hash_us)
{
	unsigned int i;
	u64 start_us = now_us();

	for (i = 0; i < nr_pages; i++) {
		const char *page = pages + i * PAGE_SIZE;

		compute_page_hash_key(page, &meta[i].key, &meta[i].zero);
	}

	*hash_us += now_us() - start_us;
}

static int hash_pool_run(struct hash_pool *pool, const char *pages, struct page_batch_meta *meta, unsigned int nr_pages,
			 u64 *hash_us)
{
	unsigned int i;

	if (!pool->threads || nr_pages < COALESCE_HASH_MIN_LOAD * COALESCE_WORK_PAGES) {
		hash_pages_serial(pages, meta, nr_pages, hash_us);
		return 0;
	}

	pthread_mutex_lock(&pool->batch.lock);
	pool->batch.pages = pages;
	pool->batch.meta = meta;
	pool->batch.nr_pages = nr_pages;
	pool->batch.done_workers = 0;
	pool->batch.hash_us = 0;
	for (i = 0; i < pool->nr_threads; i++) {
		unsigned int base = nr_pages / pool->nr_threads;
		unsigned int extra = nr_pages % pool->nr_threads;
		unsigned int start = i * base + (i < extra ? i : extra);
		unsigned int span = base + (i < extra ? 1 : 0);

		pool->batch.worker_start[i] = start;
		pool->batch.worker_end[i] = start + span;
	}
	pool->batch.generation++;
	pthread_cond_broadcast(&pool->batch.work_ready);
	while (pool->batch.done_workers != pool->batch.nr_workers)
		pthread_cond_wait(&pool->batch.work_done, &pool->batch.lock);
	*hash_us += pool->batch.hash_us;
	pthread_mutex_unlock(&pool->batch.lock);
	return 0;
}

static int append_chunk_schedule(struct chunk_schedule *schedule, u32 chunk_pages)
{
	u32 *grown;
	size_t new_cap;

	if (schedule->nr_chunks < schedule->cap) {
		schedule->pages[schedule->nr_chunks++] = chunk_pages;
		return 0;
	}

	new_cap = schedule->cap ? schedule->cap * 2 : 64;
	grown = xrealloc(schedule->pages, new_cap * sizeof(*schedule->pages));
	if (!grown)
		return -1;

	schedule->pages = grown;
	schedule->cap = new_cap;
	schedule->pages[schedule->nr_chunks++] = chunk_pages;
	return 0;
}

static int build_chunk_schedule(struct cr_img *pagemap, struct chunk_schedule *schedule)
{
	PagemapEntry *pe = NULL;
	int ret = -1;

	memzero(schedule, sizeof(*schedule));

	while (1) {
		int pb_ret = pb_read_one_eof(pagemap, &pe, PB_PAGEMAP);
		u64 i;

		if (pb_ret < 0)
			goto out;
		if (pb_ret == 0) {
			ret = 0;
			goto out;
		}

		init_compat_pagemap_entry(pe);
		if (!pagemap_present(pe)) {
			pagemap_entry__free_unpacked(pe, NULL);
			pe = NULL;
			continue;
		}

		for (i = 0; i < pe->nr_pages;) {
			u32 chunk_pages = pe->nr_pages - i;

			if (chunk_pages > COALESCE_BATCH_PAGES)
				chunk_pages = COALESCE_BATCH_PAGES;
			if (append_chunk_schedule(schedule, chunk_pages))
				goto out;
			i += chunk_pages;
		}

		pagemap_entry__free_unpacked(pe, NULL);
		pe = NULL;
	}

out:
	if (pe)
		pagemap_entry__free_unpacked(pe, NULL);
	if (ret) {
		xfree(schedule->pages);
		memzero(schedule, sizeof(*schedule));
	}
	return ret;
}

static void *blob_writer_main(void *arg)
{
	struct blob_writer *writer = arg;

	pthread_mutex_lock(&writer->lock);
	for (;;) {
		u64 local_write_us = 0;
		unsigned int i;
		int status = 0;

		while (!writer->stop && !writer->pending)
			pthread_cond_wait(&writer->ready, &writer->lock);

		if (writer->stop) {
			pthread_mutex_unlock(&writer->lock);
			return NULL;
		}

		pthread_mutex_unlock(&writer->lock);
		local_write_us = now_us();
		for (i = 0; i < writer->nr_iovecs;) {
			unsigned int batch_iov = writer->nr_iovecs - i;
			int written;

			if (batch_iov > IOV_MAX)
				batch_iov = IOV_MAX;

			written = bwritev(&writer->blob->_x, &writer->iovecs[i], batch_iov);
			if (written < 0) {
				pr_perror("Can't write coalesced page blob");
				status = -1;
				break;
			}
			if (written != (int)(batch_iov * PAGE_SIZE)) {
				pr_err("Short write to coalesced page blob: %d/%zu bytes\n", written,
				       (size_t)batch_iov * PAGE_SIZE);
				status = -1;
				break;
			}
			i += batch_iov;
		}
		local_write_us = now_us() - local_write_us;

		pthread_mutex_lock(&writer->lock);
		writer->write_us += local_write_us;
		writer->status = status;
		writer->pending = false;
		writer->nr_iovecs = 0;
		pthread_cond_signal(&writer->idle);
	}
}

static int blob_writer_init(struct blob_writer *writer, struct cr_img *blob)
{
	memzero(writer, sizeof(*writer));
	writer->blob = blob;
	writer->iovecs = xmalloc(COALESCE_BATCH_PAGES * sizeof(*writer->iovecs));
	if (!writer->iovecs)
		return -1;
	pthread_mutex_init(&writer->lock, NULL);
	pthread_cond_init(&writer->ready, NULL);
	pthread_cond_init(&writer->idle, NULL);
	if (pthread_create(&writer->thread, NULL, blob_writer_main, writer) != 0) {
		pr_err("pthread_create failed for page blob writer\n");
		pthread_cond_destroy(&writer->idle);
		pthread_cond_destroy(&writer->ready);
		pthread_mutex_destroy(&writer->lock);
		xfree(writer->iovecs);
		memzero(writer, sizeof(*writer));
		return -1;
	}
	writer->started = true;
	return 0;
}

static int blob_writer_wait(struct blob_writer *writer)
{
	int status;

	if (!writer->started)
		return 0;

	pthread_mutex_lock(&writer->lock);
	while (writer->pending)
		pthread_cond_wait(&writer->idle, &writer->lock);
	status = writer->status;
	writer->status = 0;
	pthread_mutex_unlock(&writer->lock);
	return status;
}

static int blob_writer_submit(struct blob_writer *writer, const struct iovec *iovecs, unsigned int nr_iovecs)
{
	if (!writer->started || !nr_iovecs)
		return 0;

	pthread_mutex_lock(&writer->lock);
	while (writer->pending)
		pthread_cond_wait(&writer->idle, &writer->lock);
	memcpy(writer->iovecs, iovecs, nr_iovecs * sizeof(*iovecs));
	writer->nr_iovecs = nr_iovecs;
	writer->status = 0;
	writer->pending = true;
	pthread_cond_signal(&writer->ready);
	pthread_mutex_unlock(&writer->lock);
	return 0;
}

static void blob_writer_fini(struct blob_writer *writer)
{
	if (!writer->started)
		return;

	pthread_mutex_lock(&writer->lock);
	while (writer->pending)
		pthread_cond_wait(&writer->idle, &writer->lock);
	writer->stop = true;
	pthread_cond_signal(&writer->ready);
	pthread_mutex_unlock(&writer->lock);
	pthread_join(writer->thread, NULL);
	pthread_cond_destroy(&writer->idle);
	pthread_cond_destroy(&writer->ready);
	pthread_mutex_destroy(&writer->lock);
	xfree(writer->iovecs);
}

static int collect_page_targets(int dfd, struct page_target **targets, size_t *nr_targets)
{
	DIR *dir;
	struct dirent *ent;
	size_t cap = 0;
	int dupfd;

	*targets = NULL;
	*nr_targets = 0;

	dupfd = dup(dfd);
	if (dupfd < 0) {
		pr_perror("Can't dup image directory fd");
		return -1;
	}

	dir = fdopendir(dupfd);
	if (!dir) {
		pr_perror("Can't open image directory");
		close(dupfd);
		return -1;
	}

	while ((ent = readdir(dir)) != NULL) {
		struct page_target target;
		int matched = sscanf(ent->d_name, "pagemap-%lu.img", &target.img_id);

		target.pagemap_type = CR_FD_PAGEMAP;
		if (matched != 1) {
			matched = sscanf(ent->d_name, "pagemap-shmem-%lu.img", &target.img_id);
			target.pagemap_type = CR_FD_SHMEM_PAGEMAP;
		}
		if (matched != 1)
			continue;

		if (*nr_targets == cap) {
			size_t new_cap = cap ? cap * 2 : 64;
			struct page_target *grown = xrealloc(*targets, new_cap * sizeof(**targets));
			if (!grown) {
				closedir(dir);
				return -1;
			}
			*targets = grown;
			cap = new_cap;
		}

		(*targets)[*nr_targets] = target;
		(*nr_targets)++;
	}

	if (closedir(dir)) {
		pr_perror("Can't close image directory");
		return -1;
	}

	return 0;
}

static void compact_image_set_init(struct compact_image_set *set)
{
	INIT_LIST_HEAD(&set->images);
}

static void compact_image_set_fini(struct compact_image_set *set)
{
	struct compact_image *image, *tmp;

	list_for_each_entry_safe(image, tmp, &set->images, list) {
		list_del(&image->list);
		xfree(image);
	}
}

static int compact_image_set_add(struct compact_image_set *set, u32 pages_id)
{
	struct compact_image *image = xmalloc(sizeof(*image));

	if (!image)
		return -1;

	image->pages_id = pages_id;
	list_add_tail(&image->list, &set->images);
	return 0;
}

static int unlink_image_file(int dfd, int type, u32 pages_id)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), imgset_template[type].fmt, pages_id);
	if (!unlinkat(dfd, path, 0))
		return 0;
	if (errno == ENOENT)
		return 0;

	pr_perror("Can't remove image file %s", path);
	return -1;
}

static void cleanup_compact_sidecars(int dfd, struct compact_image_set *set)
{
	struct compact_image *image;

	(void)clear_compact_pages_commit(dfd);
	(void)unlinkat(dfd, imgset_template[CR_FD_PAGES_BLOB].fmt, 0);
	list_for_each_entry(image, &set->images, list)
		(void)unlink_image_file(dfd, CR_FD_PAGE_INDEX, image->pages_id);
}

static int publish_compact_sidecars(int dfd, struct compact_image_set *set)
{
	struct compact_image *image;

	if (mark_compact_pages_commit(dfd))
		return -1;

	list_for_each_entry(image, &set->images, list) {
		if (unlink_image_file(dfd, CR_FD_PAGES, image->pages_id))
			pr_warn("Keeping raw pages-%u.img after compact commit\n", image->pages_id);
	}

	return 0;
}

static int coalesce_one_pagemap(int dfd, struct hash_pool *pool, struct page_store *store, struct compact_image_set *compact,
				const struct page_target *target, struct coalesce_stats *stats)
{
	char index_path[PATH_MAX] = {};
	struct page_batch_meta *meta = NULL;
	struct chunk_group_slot *chunk_groups = NULL;
	unsigned int *chunk_group_of_page = NULL;
	unsigned int *chunk_rep_page = NULL;
	u64 *chunk_group_offsets = NULL;
	u64 *offset_buffers[2] = {};
	struct iovec *blob_iov = NULL;
	struct cr_img *index = NULL;
	struct cr_img *old_pages = NULL;
	struct cr_img *pagemap = NULL;
	struct chunk_schedule schedule = {};
	struct blob_writer writer = {};
	u32 pages_id = 0;
	off_t old_pages_size;
	off_t source_off = 0;
	int old_pages_fd;
	const char *old_pages_map = NULL;
	bool old_pages_mapped = false;
	size_t chunk_groups_cap;
	unsigned int current_offsets = 0;
	unsigned int pending_offsets = 0;
	unsigned int pending_chunk_pages = 0;
	bool have_pending_index = false;
	u64 step_start_us = 0;
	u64 image_start_us = 0;
	u64 image_present_pages = 0;
	u64 image_zero_pages = 0;
	u64 image_unique_pages = 0;
	u64 image_lookup_candidates = 0;
	u64 image_local_duplicate_pages = 0;
	u64 image_total_us = 0;
	u64 image_read_us = 0;
	u64 image_hash_us = 0;
	u64 image_lookup_us = 0;
	u64 image_blob_write_us = 0;
	u64 image_index_write_us = 0;
	u32 chunk_group_generation = 1;
	int ret = -1;

	meta = xzalloc(COALESCE_BATCH_PAGES * sizeof(*meta));
	offset_buffers[0] = xzalloc(COALESCE_BATCH_PAGES * sizeof(*offset_buffers[0]));
	offset_buffers[1] = xzalloc(COALESCE_BATCH_PAGES * sizeof(*offset_buffers[1]));
	blob_iov = xzalloc(COALESCE_BATCH_PAGES * sizeof(*blob_iov));
	chunk_group_of_page = xzalloc(COALESCE_BATCH_PAGES * sizeof(*chunk_group_of_page));
	chunk_rep_page = xzalloc(COALESCE_BATCH_PAGES * sizeof(*chunk_rep_page));
	chunk_group_offsets = xzalloc(COALESCE_BATCH_PAGES * sizeof(*chunk_group_offsets));
	chunk_groups_cap = next_power_of_two(COALESCE_BATCH_PAGES * 2);
	chunk_groups = xzalloc(chunk_groups_cap * sizeof(*chunk_groups));
	if (!meta || !offset_buffers[0] || !offset_buffers[1] || !blob_iov || !chunk_group_of_page || !chunk_rep_page ||
	    !chunk_group_offsets || !chunk_groups)
		goto out;

	pagemap = open_image_at(dfd, target->pagemap_type, O_RSTR, target->img_id);
	if (!pagemap || empty_image(pagemap)) {
		pr_err("Missing pagemap image for id %lu\n", target->img_id);
		goto out;
	}

	old_pages = open_raw_pages_image_at(dfd, O_RSTR, pagemap, &pages_id);
	if (!old_pages || empty_image(old_pages)) {
		if (compact_pages_committed(dfd, pages_id)) {
			ret = 0;
			goto out;
		}
		pr_err("Missing pages image for id %u\n", pages_id);
		goto out;
	}

	index = open_image_at(dfd, CR_FD_PAGE_INDEX, O_DUMP, pages_id);
	if (!index)
		goto out;

	old_pages_fd = img_raw_fd(old_pages);
	old_pages_size = img_raw_size(old_pages);
	if (old_pages_fd < 0 || old_pages_size < 0)
		goto out;

	if (build_chunk_schedule(pagemap, &schedule))
		goto out;

	step_start_us = now_us();
	old_pages_map = mmap(NULL, (size_t)old_pages_size, PROT_READ, MAP_PRIVATE, old_pages_fd, 0);
	if (old_pages_map == MAP_FAILED) {
		pr_perror("Can't mmap pages image for id %u", pages_id);
		old_pages_map = NULL;
		goto out;
	}
	old_pages_mapped = true;
	(void)posix_fadvise(old_pages_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
	(void)madvise((void *)old_pages_map, (size_t)old_pages_size, MADV_SEQUENTIAL);
	image_read_us += now_us() - step_start_us;

	if (blob_writer_init(&writer, store->blob))
		goto out;

	image_start_us = now_us();
	for (size_t chunk_idx = 0; chunk_idx < schedule.nr_chunks; chunk_idx++) {
		unsigned int chunk_pages = schedule.pages[chunk_idx];
		size_t chunk_bytes = (size_t)chunk_pages * PAGE_SIZE;
		const char *chunk_pages_ptr = old_pages_map + source_off;
		u64 *offsets = offset_buffers[current_offsets];
		u64 lookup_start_us;
		unsigned int j;
		unsigned int nr_blob_pages = 0;
		unsigned int group_count = 0;
		unsigned int chunk_nonzero_pages = 0;

		if (hash_pool_run(pool, chunk_pages_ptr, meta, chunk_pages, &image_hash_us))
			goto out;

		if (!chunk_group_generation) {
			memzero(chunk_groups, chunk_groups_cap * sizeof(*chunk_groups));
			chunk_group_generation = 1;
		}

		for (j = 0; j < chunk_pages; j++) {
			unsigned int group_id;
			bool is_new;

			if (meta[j].zero) {
				offsets[j] = PAGE_INDEX_ZERO;
				image_zero_pages++;
				continue;
			}

			chunk_nonzero_pages++;
			if (chunk_group_lookup_or_reserve(chunk_groups, chunk_groups_cap, chunk_group_generation, &meta[j].key,
							  group_count, &group_id, &is_new))
				goto out;
			chunk_group_of_page[j] = group_id;
			if (is_new)
				chunk_rep_page[group_count++] = j;
		}

		lookup_start_us = now_us();
		for (j = 0; j < group_count; j++) {
			unsigned int rep = chunk_rep_page[j];
			bool is_new;

			if (page_store_lookup_or_reserve(store, &meta[rep].key, &chunk_group_offsets[j], &is_new))
				goto out;
			if (is_new) {
				blob_iov[nr_blob_pages].iov_base = (void *)(chunk_pages_ptr + rep * PAGE_SIZE);
				blob_iov[nr_blob_pages].iov_len = PAGE_SIZE;
				nr_blob_pages++;
				image_unique_pages++;
			}
		}
		image_lookup_us += now_us() - lookup_start_us;
		image_lookup_candidates += group_count;
		image_local_duplicate_pages += chunk_nonzero_pages - group_count;

		for (j = 0; j < chunk_pages; j++) {
			if (!meta[j].zero)
				offsets[j] = chunk_group_offsets[chunk_group_of_page[j]];
		}
		for (j = 0; j < chunk_pages; j++) {
			if (offsets[j] != PAGE_INDEX_ZERO && (offsets[j] & (PAGE_SIZE - 1))) {
				pr_err("Non page-aligned compact page offset for image %u at page %u: %llu\n",
				       pages_id, j, (unsigned long long)offsets[j]);
				goto out;
			}
		}

		if (nr_blob_pages > 0) {
			if (blob_writer_wait(&writer))
				goto out;
		} else if (blob_writer_wait(&writer)) {
			goto out;
		}

		if (have_pending_index) {
			step_start_us = now_us();
			if (write_img_buf(index, offset_buffers[pending_offsets], pending_chunk_pages * sizeof(*offsets)))
				goto out;
			image_index_write_us += now_us() - step_start_us;
			have_pending_index = false;
		}

		if (nr_blob_pages > 0 && blob_writer_submit(&writer, blob_iov, nr_blob_pages))
			goto out;

		pending_offsets = current_offsets;
		pending_chunk_pages = chunk_pages;
		have_pending_index = true;
		current_offsets ^= 1U;

		image_present_pages += chunk_pages;
		source_off += chunk_bytes;
		chunk_group_generation++;
	}

	if (source_off != old_pages_size) {
		pr_err("Pages image size mismatch for id %u: consumed %jd bytes, file has %jd bytes\n", pages_id,
		       (intmax_t)source_off, (intmax_t)old_pages_size);
		goto out;
	}

	if (blob_writer_wait(&writer))
		goto out;
	if (have_pending_index) {
		step_start_us = now_us();
		if (write_img_buf(index, offset_buffers[pending_offsets], pending_chunk_pages * sizeof(*offset_buffers[pending_offsets])))
			goto out;
		image_index_write_us += now_us() - step_start_us;
	}

	snprintf(index_path, sizeof(index_path), imgset_template[CR_FD_PAGE_INDEX].fmt, pages_id);

	store->blob_write_us += writer.write_us;
	image_blob_write_us = writer.write_us;
	image_total_us = now_us() - image_start_us;

	stats->old_bytes += old_pages_size;
	stats->index_bytes += image_present_pages * sizeof(*offset_buffers[0]);
	stats->present_pages += image_present_pages;
	stats->zero_pages += image_zero_pages;
	stats->unique_pages += image_unique_pages;
	stats->lookup_candidates += image_lookup_candidates;
	stats->local_duplicate_pages += image_local_duplicate_pages;
	stats->images++;
	stats->total_us += image_total_us;
	stats->read_us += image_read_us;
	stats->hash_us += image_hash_us;
	stats->lookup_us += image_lookup_us;
	stats->blob_write_us = store->blob_write_us;
	stats->index_write_us += image_index_write_us;

	pr_info("Image %u coalesced: present=%llu unique=%llu local_unique=%llu local_dup=%llu zero=%llu total=%llu ms read=%llu ms hash=%llu ms lookup=%llu ms blob=%llu ms index=%llu ms\n",
		pages_id, (unsigned long long)image_present_pages, (unsigned long long)image_unique_pages,
		(unsigned long long)image_lookup_candidates, (unsigned long long)image_local_duplicate_pages,
		(unsigned long long)image_zero_pages, (unsigned long long)(image_total_us / 1000ULL),
		(unsigned long long)(image_read_us / 1000ULL), (unsigned long long)(image_hash_us / 1000ULL),
		(unsigned long long)(image_lookup_us / 1000ULL), (unsigned long long)(image_blob_write_us / 1000ULL),
		(unsigned long long)(image_index_write_us / 1000ULL));

	if (compact_image_set_add(compact, pages_id))
		goto out;

	ret = 0;
out:
	blob_writer_fini(&writer);
	if (index)
		close_image(index);
	if (old_pages)
		close_image(old_pages);
	if (pagemap)
		close_image(pagemap);
	if (old_pages_mapped)
		munmap((void *)old_pages_map, old_pages_size);
	xfree(schedule.pages);
	xfree(offset_buffers[0]);
	xfree(offset_buffers[1]);
	xfree(meta);
	xfree(blob_iov);
	xfree(chunk_group_of_page);
	xfree(chunk_rep_page);
	xfree(chunk_group_offsets);
	xfree(chunk_groups);

	if (ret != 0 && index_path[0] && unlinkat(dfd, index_path, 0) && errno != ENOENT)
		pr_perror("Can't remove partial compact page index %s", index_path);

	return ret;
}

static int should_run_page_coalesce(void)
{
	if (!opts.auto_dedup)
		return 0;

	if (opts.lazy_pages || opts.use_page_server || opts.stream) {
		pr_warn("Skipping page coalescing because lazy-pages, page-server, or image streaming is enabled\n");
		return 0;
	}

	return 1;
}

static int coalesce_worker_init(struct coalesce_worker_state *state)
{
	memzero(state, sizeof(*state));
	INIT_LIST_HEAD(&state->jobs);
	compact_image_set_init(&state->compact);

	state->dfd = get_service_fd(IMG_FD_OFF);
	if (state->dfd < 0) {
		pr_err("Image directory is not open\n");
		return -1;
	}

	if (hash_pool_init(&state->pool))
		return -1;

	if (clear_compact_pages_commit(state->dfd)) {
		hash_pool_fini(&state->pool);
		return -1;
	}

	state->store.blob = open_image_at(state->dfd, CR_FD_PAGES_BLOB, O_RDWR | O_CREAT | O_TRUNC);
	if (!state->store.blob) {
		hash_pool_fini(&state->pool);
		return -1;
	}

	pthread_mutex_init(&state->lock, NULL);
	pthread_cond_init(&state->ready, NULL);
	state->started = true;
	return 0;
}

static void coalesce_worker_destroy(struct coalesce_worker_state *state)
{
	struct coalesce_job *job, *tmp;

	if (!state->started)
		return;

	list_for_each_entry_safe(job, tmp, &state->jobs, list) {
		list_del(&job->list);
		xfree(job);
	}

	close_image(state->store.blob);
	hash_pool_fini(&state->pool);
	xfree(state->store.slots);
	compact_image_set_fini(&state->compact);
	pthread_cond_destroy(&state->ready);
	pthread_mutex_destroy(&state->lock);
	memzero(state, sizeof(*state));
	INIT_LIST_HEAD(&state->jobs);
	compact_image_set_init(&state->compact);
}

static void *coalesce_worker_main(void *arg)
{
	struct coalesce_worker_state *state = arg;

	for (;;) {
		struct coalesce_job *job;
		struct page_target target;

		pthread_mutex_lock(&state->lock);
		while (list_empty(&state->jobs) && !state->stop)
			pthread_cond_wait(&state->ready, &state->lock);

		if (list_empty(&state->jobs) && state->stop) {
			pthread_mutex_unlock(&state->lock);
			return NULL;
		}

		job = list_first_entry(&state->jobs, struct coalesce_job, list);
		list_del(&job->list);
		pthread_mutex_unlock(&state->lock);

		target.pagemap_type = job->pagemap_type;
		target.img_id = job->img_id;
		if (coalesce_one_pagemap(state->dfd, &state->pool, &state->store, &state->compact, &target, &state->stats)) {
			pthread_mutex_lock(&state->lock);
			state->failed = true;
			state->stop = true;
			pthread_cond_broadcast(&state->ready);
			pthread_mutex_unlock(&state->lock);
			xfree(job);
			return NULL;
		}

		xfree(job);
	}
}

int coalesce_checkpoint_pages_start(void)
{
	if (!should_run_page_coalesce())
		return 0;

	if (online_state.failed)
		return -1;

	if (online_state.started)
		return 0;

	if (coalesce_worker_init(&online_state))
		return -1;

	if (pthread_create(&online_state.thread, NULL, coalesce_worker_main, &online_state) != 0) {
		pr_err("pthread_create failed for online page coalescer\n");
		coalesce_worker_destroy(&online_state);
		return -1;
	}

	return 0;
}

int coalesce_checkpoint_pages_enqueue(int pagemap_type, unsigned long img_id)
{
	struct coalesce_job *job;

	if (!should_run_page_coalesce())
		return 0;
	if (!online_state.started)
		return 0;

	job = xmalloc(sizeof(*job));
	if (!job) {
		pthread_mutex_lock(&online_state.lock);
		online_state.failed = true;
		pthread_mutex_unlock(&online_state.lock);
		return -1;
	}

	job->pagemap_type = pagemap_type;
	job->img_id = img_id;

	pthread_mutex_lock(&online_state.lock);
	if (online_state.failed) {
		pthread_mutex_unlock(&online_state.lock);
		xfree(job);
		return -1;
	}
	list_add_tail(&job->list, &online_state.jobs);
	pthread_cond_signal(&online_state.ready);
	pthread_mutex_unlock(&online_state.lock);
	return 0;
}

void coalesce_checkpoint_pages_abort(void)
{
	struct coalesce_job *job, *tmp;

	if (!online_state.started)
		return;

	pthread_mutex_lock(&online_state.lock);
	online_state.stop = true;
	online_state.failed = true;
	list_for_each_entry_safe(job, tmp, &online_state.jobs, list) {
		list_del(&job->list);
		xfree(job);
	}
	pthread_cond_broadcast(&online_state.ready);
	pthread_mutex_unlock(&online_state.lock);

	pthread_join(online_state.thread, NULL);
	cleanup_compact_sidecars(online_state.dfd, &online_state.compact);
	coalesce_worker_destroy(&online_state);
}

static int coalesce_checkpoint_pages_finish_online(void)
{
	int ret = 0;

	pthread_mutex_lock(&online_state.lock);
	online_state.stop = true;
	pthread_cond_broadcast(&online_state.ready);
	pthread_mutex_unlock(&online_state.lock);

	pthread_join(online_state.thread, NULL);
	if (online_state.failed)
		ret = -1;
	else if (publish_compact_sidecars(online_state.dfd, &online_state.compact))
		ret = -1;

	if (!ret) {
		online_state.stats.total_us = online_state.stats.read_us + online_state.stats.hash_us + online_state.stats.lookup_us +
					      online_state.store.blob_write_us + online_state.stats.index_write_us;
		online_state.stats.blob_write_us = online_state.store.blob_write_us;
		online_state.stats.blob_bytes = online_state.store.next_offset;

		pr_info("Coalesced %llu present pages across %llu pagemap images into %llu unique non-zero pages, eliding %llu zero pages\n",
			(unsigned long long)online_state.stats.present_pages, (unsigned long long)online_state.stats.images,
			(unsigned long long)online_state.stats.unique_pages, (unsigned long long)online_state.stats.zero_pages);
		pr_info("Chunk-local dedupe candidates: lookup=%llu local_duplicate=%llu\n",
			(unsigned long long)online_state.stats.lookup_candidates,
			(unsigned long long)online_state.stats.local_duplicate_pages);
		pr_info("Page payload bytes: old=%llu blob=%llu index=%llu combined=%llu saved=%lld\n",
			(unsigned long long)online_state.stats.old_bytes, (unsigned long long)online_state.stats.blob_bytes,
			(unsigned long long)online_state.stats.index_bytes,
			(unsigned long long)(online_state.stats.blob_bytes + online_state.stats.index_bytes),
			(long long)(online_state.stats.old_bytes -
				   (online_state.stats.blob_bytes + online_state.stats.index_bytes)));
		pr_info("Coalesce timings: total=%llu ms read=%llu ms hash=%llu ms lookup=%llu ms blob=%llu ms index=%llu ms grow=%llu ms table_used=%zu table_cap=%zu table_load=%llu%% workers=%u batch_pages=%u mode=online\n",
			(unsigned long long)(online_state.stats.total_us / 1000ULL),
			(unsigned long long)(online_state.stats.read_us / 1000ULL),
			(unsigned long long)(online_state.stats.hash_us / 1000ULL),
			(unsigned long long)(online_state.stats.lookup_us / 1000ULL),
			(unsigned long long)(online_state.stats.blob_write_us / 1000ULL),
			(unsigned long long)(online_state.stats.index_write_us / 1000ULL),
			(unsigned long long)(online_state.store.grow_us / 1000ULL), online_state.store.used,
			online_state.store.cap,
			(unsigned long long)(online_state.store.cap ? (online_state.store.used * 100ULL / online_state.store.cap) : 0),
			online_state.pool.nr_threads, COALESCE_BATCH_PAGES);
	}
	if (ret)
		cleanup_compact_sidecars(online_state.dfd, &online_state.compact);

	coalesce_worker_destroy(&online_state);
	return ret;
}

static int coalesce_checkpoint_pages_postpass(void)
{
	struct hash_pool pool;
	struct page_store store = {};
	struct coalesce_stats stats = {};
	struct compact_image_set compact;
	struct page_target *targets = NULL;
	size_t i;
	size_t nr_targets = 0;
	int dfd;
	int ret = 0;
	u64 start_us;

	if (!should_run_page_coalesce())
		return 0;

	dfd = get_service_fd(IMG_FD_OFF);
	if (dfd < 0) {
		pr_err("Image directory is not open\n");
		return -1;
	}

	if (collect_page_targets(dfd, &targets, &nr_targets))
		return -1;
	if (!nr_targets) {
		xfree(targets);
		return 0;
	}
	compact_image_set_init(&compact);

	if (hash_pool_init(&pool)) {
		xfree(targets);
		compact_image_set_fini(&compact);
		return -1;
	}

	if (clear_compact_pages_commit(dfd)) {
		hash_pool_fini(&pool);
		xfree(targets);
		compact_image_set_fini(&compact);
		return -1;
	}

	store.blob = open_image_at(dfd, CR_FD_PAGES_BLOB, O_RDWR | O_CREAT | O_TRUNC);
	if (!store.blob) {
		hash_pool_fini(&pool);
		xfree(targets);
		compact_image_set_fini(&compact);
		return -1;
	}

	start_us = now_us();
	for (i = 0; i < nr_targets; i++) {
		if (coalesce_one_pagemap(dfd, &pool, &store, &compact, &targets[i], &stats)) {
			ret = -1;
			break;
		}
	}
	stats.total_us = now_us() - start_us;
	stats.blob_write_us = store.blob_write_us;
	stats.blob_bytes = store.next_offset;

	close_image(store.blob);
	hash_pool_fini(&pool);
	xfree(store.slots);
	xfree(targets);

	if (!ret && publish_compact_sidecars(dfd, &compact))
		ret = -1;
	if (ret)
		cleanup_compact_sidecars(dfd, &compact);
	compact_image_set_fini(&compact);
	if (ret)
		return ret;

	pr_info("Coalesced %llu present pages across %llu pagemap images into %llu unique non-zero pages, eliding %llu zero pages\n",
		(unsigned long long)stats.present_pages, (unsigned long long)stats.images,
		(unsigned long long)stats.unique_pages, (unsigned long long)stats.zero_pages);
	pr_info("Chunk-local dedupe candidates: lookup=%llu local_duplicate=%llu\n",
		(unsigned long long)stats.lookup_candidates, (unsigned long long)stats.local_duplicate_pages);
	pr_info("Page payload bytes: old=%llu blob=%llu index=%llu combined=%llu saved=%lld\n",
		(unsigned long long)stats.old_bytes, (unsigned long long)stats.blob_bytes,
		(unsigned long long)stats.index_bytes,
		(unsigned long long)(stats.blob_bytes + stats.index_bytes),
		(long long)(stats.old_bytes - (stats.blob_bytes + stats.index_bytes)));
	pr_info("Coalesce timings: total=%llu ms read=%llu ms hash=%llu ms lookup=%llu ms blob=%llu ms index=%llu ms grow=%llu ms table_used=%zu table_cap=%zu table_load=%llu%% workers=%u batch_pages=%u\n",
		(unsigned long long)(stats.total_us / 1000ULL), (unsigned long long)(stats.read_us / 1000ULL),
		(unsigned long long)(stats.hash_us / 1000ULL), (unsigned long long)(stats.lookup_us / 1000ULL),
		(unsigned long long)(stats.blob_write_us / 1000ULL), (unsigned long long)(stats.index_write_us / 1000ULL),
		(unsigned long long)(store.grow_us / 1000ULL), store.used, store.cap,
		(unsigned long long)(store.cap ? (store.used * 100ULL / store.cap) : 0), pool.nr_threads, COALESCE_BATCH_PAGES);
	return 0;
}

int coalesce_checkpoint_pages(void)
{
	if (online_state.started)
		return coalesce_checkpoint_pages_finish_online();

	return coalesce_checkpoint_pages_postpass();
}
