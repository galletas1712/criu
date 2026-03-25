#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <linux/falloc.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <limits.h>

#include "types.h"
#include "image.h"
#include "cr_options.h"
#include "servicefd.h"
#include "pagemap.h"
#include "restorer.h"
#include "rst-malloc.h"
#include "page-xfer.h"

#include "fault-injection.h"
#include "xmalloc.h"
#include "protobuf.h"
#include "images/pagemap.pb-c.h"

#ifndef SEEK_DATA
#define SEEK_DATA 3
#define SEEK_HOLE 4
#endif

#define MAX_BUNCH_SIZE 256

/*
 * One "job" for the preadv() syscall in pagemap.c
 */
struct page_read_iov {
	off_t from;	  /* offset in pi file where to start reading from */
	off_t end;	  /* the end of the read == sum to.iov_len -s */
	struct iovec *to; /* destination iovs */
	struct iovec *to_base;
	unsigned int nr;  /* their number */
	struct restore_vma_copy *copies;
	unsigned int nr_copies;

	struct list_head l;
};

struct compact_page_ref {
	u64 off;
	void *dst;
};

struct compact_page_group {
	u64 off;
	void **dsts;
	unsigned int nr_dsts;
};

static inline bool compact_io_is_zero(off_t off);
static inline bool compact_io_is_zero_skip(off_t off);
static inline bool compact_io_is_run(off_t off);
static inline off_t compact_io_decode(off_t off);

static bool direct_io_aligned(const void *buf, size_t len, off_t off)
{
	return (((unsigned long)buf & (PAGE_SIZE - 1)) == 0) && ((len & (PAGE_SIZE - 1)) == 0) &&
	       ((off & (PAGE_SIZE - 1)) == 0);
}

static bool direct_iovs_aligned(const struct iovec *iov, unsigned int nr, off_t off)
{
	unsigned int i;

	if (off & (PAGE_SIZE - 1))
		return false;

	for (i = 0; i < nr; i++) {
		if (!direct_io_aligned(iov[i].iov_base, iov[i].iov_len, off))
			return false;
	}

	return true;
}

static int pread_full(int fd, void *buf, size_t len, off_t off, unsigned int *short_reads)
{
	size_t curr = 0;

	while (curr < len) {
		ssize_t ret = pread(fd, (char *)buf + curr, len - curr, off + curr);

		if (ret < 1) {
			if (ret < 0)
				pr_perror("Can't read mapping page %zd", ret);
			else
				pr_err("Unexpected EOF while reading %zu bytes at off %lld\n", len - curr,
				       (long long)(off + curr));
			return -1;
		}

		curr += ret;
		if (curr < len && short_reads)
			(*short_reads)++;
	}

	return 0;
}

static void copy_linear_to_iovecs(const void *src, const struct iovec *iov, unsigned int nr)
{
	const char *curr = src;
	unsigned int i;

	for (i = 0; i < nr; i++) {
		memcpy(iov[i].iov_base, curr, iov[i].iov_len);
		curr += iov[i].iov_len;
	}
}

static void advance_iovecs(struct iovec **iov, unsigned int *nr, ssize_t len)
{
	while (len > 0) {
		if (len >= (ssize_t)(*iov)->iov_len) {
			len -= (*iov)->iov_len;
			(*iov)++;
			(*nr)--;
			continue;
		}

		(*iov)->iov_base += len;
		(*iov)->iov_len -= len;
		break;
	}
}

static int direct_read_iovecs(struct page_read *pr, int fd, off_t off, struct iovec *iov, unsigned int nr,
			      size_t len, unsigned int *short_reads)
{
	void *aligned_buf = NULL;
	struct iovec *direct_iov = NULL;
	struct iovec *direct_curr;
	unsigned int direct_nr;
	bool aligned = direct_iovs_aligned(iov, nr, off);

	pr->direct_pages += len / PAGE_SIZE;
	if (!aligned) {
		int err;

		pr->direct_misaligned_pages += len / PAGE_SIZE;
		pr->direct_misaligned_reads++;

		err = posix_memalign(&aligned_buf, PAGE_SIZE, len);
		if (err) {
			pr_err("Can't allocate aligned buffer for direct read (%zu bytes, err=%d)\n", len, err);
			return -1;
		}

		if (pread_full(fd, aligned_buf, len, off, short_reads)) {
			xfree(aligned_buf);
			return -1;
		}
		copy_linear_to_iovecs(aligned_buf, iov, nr);
		xfree(aligned_buf);
		return 0;
	}

	direct_iov = xmalloc(nr * sizeof(*direct_iov));
	if (!direct_iov) {
		pr_err("Can't allocate direct iovec scratch (%u entries)\n", nr);
		return -1;
	}

	memcpy(direct_iov, iov, nr * sizeof(*direct_iov));
	direct_curr = direct_iov;
	direct_nr = nr;
	while (1) {
		ssize_t bytes = preadv(fd, direct_curr, direct_nr, off);

		if (fault_injected(FI_PARTIAL_PAGES)) {
			if (bytes > 0 && direct_nr >= 2) {
				pr_debug("`- trim preadv %zu\n", bytes);
				bytes /= 2;
			}
		}

		if (bytes < 0) {
			pr_err("Can't read async pr bytes (%zd / %zu read, %lld off, %u iovs)\n", bytes, len,
			       (long long)off, direct_nr);
			xfree(direct_iov);
			return -1;
		}
		if (bytes == 0) {
			pr_err("Unexpected EOF in async page read (%zu bytes remaining at off %lld, %u iovs)\n", len,
			       (long long)off, direct_nr);
			xfree(direct_iov);
			return -1;
		}
		if ((size_t)bytes == len) {
			xfree(direct_iov);
			return 0;
		}

		if (short_reads)
			(*short_reads)++;
		advance_iovecs(&direct_curr, &direct_nr, bytes);
		off += bytes;
		len -= bytes;
	}
}

static int compact_page_ref_cmp(const void *a, const void *b)
{
	const struct compact_page_ref *ra = a;
	const struct compact_page_ref *rb = b;

	if (ra->off < rb->off)
		return -1;
	if (ra->off > rb->off)
		return 1;
	if ((unsigned long)ra->dst < (unsigned long)rb->dst)
		return -1;
	if ((unsigned long)ra->dst > (unsigned long)rb->dst)
		return 1;
	return 0;
}

static void free_page_read_iov(struct page_read_iov *piov)
{
	xfree(piov->copies);
	xfree(piov->to_base);
	xfree(piov);
}

static int append_page_read_iov(struct page_read_iov *dst, struct page_read_iov *src)
{
	struct iovec *to = NULL;
	struct restore_vma_copy *copies = NULL;
	unsigned int old_nr = dst->nr;
	unsigned int old_copies = dst->nr_copies;
	unsigned int nr = dst->nr + src->nr;
	unsigned int nr_copies = dst->nr_copies + src->nr_copies;

	to = xmalloc(nr * sizeof(*to));
	if (!to)
		return -1;

	memcpy(to, dst->to_base, dst->nr * sizeof(*to));
	memcpy(&to[old_nr], src->to, src->nr * sizeof(*src->to));

	if (nr_copies > 0) {
		copies = xmalloc(nr_copies * sizeof(*copies));
		if (!copies) {
			xfree(to);
			return -1;
		}

		if (old_copies > 0)
			memcpy(copies, dst->copies, old_copies * sizeof(*copies));
		memcpy(&copies[old_copies], src->copies, src->nr_copies * sizeof(*src->copies));
	}

	xfree(dst->to_base);
	xfree(dst->copies);
	dst->to_base = to;
	dst->to = to;
	dst->copies = copies;
	dst->nr = nr;
	dst->nr_copies = nr_copies;
	dst->end += src->end - src->from;

	free_page_read_iov(src);
	return 0;
}

static int append_compact_page_read_iov(struct list_head *to, struct page_read_iov *src)
{
	struct page_read_iov *dst;
	bool same_zero;
	bool same_run;

	if (list_empty(to)) {
		list_add_tail(&src->l, to);
		return 0;
	}

	dst = list_entry(to->prev, struct page_read_iov, l);
	same_zero = compact_io_is_zero(dst->from) && compact_io_is_zero(src->from) && dst->from == src->from;
	same_run = compact_io_is_run(dst->from) && compact_io_is_run(src->from) &&
		   compact_io_decode(src->from) == compact_io_decode(dst->end);
	if (!same_zero && !same_run) {
		list_add_tail(&src->l, to);
		return 0;
	}

	if (dst->nr + src->nr > IOV_MAX) {
		list_add_tail(&src->l, to);
		return 0;
	}

	return append_page_read_iov(dst, src);
}

static int compact_async_piov_cmp(const void *a, const void *b)
{
	const struct page_read_iov *const *pa = a;
	const struct page_read_iov *const *pb = b;
	bool a_zero = compact_io_is_zero((*pa)->from);
	bool b_zero = compact_io_is_zero((*pb)->from);
	unsigned long a_dst = (unsigned long)(*pa)->to[0].iov_base;
	unsigned long b_dst = (unsigned long)(*pb)->to[0].iov_base;

	if (a_zero != b_zero)
		return a_zero ? 1 : -1;
	if (!a_zero) {
		off_t a_from = compact_io_decode((*pa)->from);
		off_t b_from = compact_io_decode((*pb)->from);

		if (a_from < b_from)
			return -1;
		if (a_from > b_from)
			return 1;
	}
	if (a_dst < b_dst)
		return -1;
	if (a_dst > b_dst)
		return 1;
	return 0;
}

static void prepare_compact_async_queue(struct page_read *pr, u64 *sort_us, u64 *merge_us,
					unsigned int *queue_jobs, unsigned int *merged_jobs)
{
	struct timeval tv0, tv1, tv2, tv3;
	struct page_read_iov **piovs;
	struct page_read_iov *piov;
	struct list_head sorted;
	unsigned int nr = 0;
	unsigned int i = 0;

	if (!pr->pidx)
		return;

	list_for_each_entry(piov, &pr->async, l)
		nr++;
	*queue_jobs = nr;
	*merged_jobs = nr;
	if (nr < 2)
		return;

	piovs = xmalloc(nr * sizeof(*piovs));
	if (!piovs) {
		pr_warn("Can't allocate compact async sort array, keeping logical order\n");
		return;
	}

	list_for_each_entry(piov, &pr->async, l)
		piovs[i++] = piov;

	gettimeofday(&tv0, NULL);
	qsort(piovs, nr, sizeof(*piovs), compact_async_piov_cmp);
	gettimeofday(&tv1, NULL);

	INIT_LIST_HEAD(&sorted);
	gettimeofday(&tv2, NULL);
	for (i = 0; i < nr; i++) {
		list_del_init(&piovs[i]->l);
		if (append_compact_page_read_iov(&sorted, piovs[i])) {
			pr_warn("Can't merge compact async queue entry, keeping split job\n");
			list_add_tail(&piovs[i]->l, &sorted);
		}
	}
	list_splice_init(&sorted, &pr->async);
	*sort_us = (u64)(tv1.tv_sec - tv0.tv_sec) * 1000000ULL +
		   (u64)(tv1.tv_usec - tv0.tv_usec);
	gettimeofday(&tv3, NULL);
	*merge_us = (u64)(tv3.tv_sec - tv2.tv_sec) * 1000000ULL +
		    (u64)(tv3.tv_usec - tv2.tv_usec);

	nr = 0;
	list_for_each_entry(piov, &pr->async, l)
		nr++;
	*merged_jobs = nr;

	xfree(piovs);
}

static inline bool can_extend_bunch(struct iovec *bunch, unsigned long off, unsigned long len)
{
	return /* The next region is the continuation of the existing */
		((unsigned long)bunch->iov_base + bunch->iov_len == off) &&
		/* The resulting region is non empty and is small enough */
		(bunch->iov_len == 0 || bunch->iov_len + len < MAX_BUNCH_SIZE * PAGE_SIZE);
}

static int punch_hole(struct page_read *pr, unsigned long off, unsigned long len, bool cleanup)
{
	int ret;
	struct iovec *bunch = &pr->bunch;

	if (!cleanup && can_extend_bunch(bunch, off, len)) {
		pr_debug("pr%lu-%u:Extend bunch len from %zu to %lu\n", pr->img_id, pr->id, bunch->iov_len,
			 bunch->iov_len + len);
		bunch->iov_len += len;
	} else {
		if (bunch->iov_len > 0) {
			pr_debug("Punch!/%p/%zu/\n", bunch->iov_base, bunch->iov_len);
			ret = fallocate(img_raw_fd(pr->pi), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
					(unsigned long)bunch->iov_base, bunch->iov_len);
			if (ret != 0) {
				pr_perror("Error punching hole");
				return -1;
			}
		}
		bunch->iov_base = (void *)off;
		bunch->iov_len = len;
		pr_debug("pr%lu-%u:New bunch/%p/%zu/\n", pr->img_id, pr->id, bunch->iov_base, bunch->iov_len);
	}
	return 0;
}

int dedup_one_iovec(struct page_read *pr, unsigned long off, unsigned long len)
{
	unsigned long iov_end;

	iov_end = off + len;
	while (1) {
		int ret;
		unsigned long piov_end;
		struct page_read *prp;

		ret = pr->seek_pagemap(pr, off);
		if (ret == 0) {
			if (off < pr->cvaddr && pr->cvaddr < iov_end) {
				pr_debug("pr%lu-%u:No range %lx-%lx in pagemap\n", pr->img_id, pr->id, off, pr->cvaddr);
				off = pr->cvaddr;
			} else {
				pr_debug("pr%lu-%u:No range %lx-%lx in pagemap\n", pr->img_id, pr->id, off, iov_end);
				return 0;
			}
		}

		if (!pr->pe)
			return -1;
		piov_end = pr->pe->vaddr + pagemap_len(pr->pe);
		if (!pagemap_in_parent(pr->pe)) {
			ret = punch_hole(pr, pr->pi_off, min(piov_end, iov_end) - off, false);
			if (ret == -1)
				return ret;
		}

		prp = pr->parent;
		if (prp) {
			/* recursively */
			pr_debug("pr%lu-%u:Go to next parent level\n", pr->img_id, pr->id);
			len = min(piov_end, iov_end) - off;
			ret = dedup_one_iovec(prp, off, len);
			if (ret != 0)
				return -1;
		}

		if (piov_end < iov_end) {
			off = piov_end;
			continue;
		} else
			return 0;
	}
	return 0;
}

static int advance(struct page_read *pr)
{
	pr->curr_pme++;
	if (pr->curr_pme >= pr->nr_pmes)
		return 0;

	pr->pe = pr->pmes[pr->curr_pme];
	pr->cvaddr = pr->pe->vaddr;

	return 1;
}

static void skip_pagemap_pages(struct page_read *pr, unsigned long len)
{
	if (!len)
		return;

	if (pagemap_present(pr->pe))
		pr->pi_off += len;
	pr->cvaddr += len;
}

static int seek_pagemap(struct page_read *pr, unsigned long vaddr)
{
	if (!pr->pe)
		goto adv;

	do {
		unsigned long start = pr->pe->vaddr;
		unsigned long end = start + pagemap_len(pr->pe);

		if (vaddr < pr->cvaddr)
			break;

		if (vaddr >= start && vaddr < end) {
			skip_pagemap_pages(pr, vaddr - pr->cvaddr);
			return 1;
		}

		if (end <= vaddr)
			skip_pagemap_pages(pr, end - pr->cvaddr);
	adv:; /* otherwise "label at end of compound stmt" gcc error */
	} while (advance(pr));

	return 0;
}

static inline void pagemap_bound_check(PagemapEntry *pe, unsigned long vaddr, unsigned long int nr)
{
	if (vaddr < pe->vaddr || (vaddr - pe->vaddr) / PAGE_SIZE + nr > pe->nr_pages) {
		pr_err("Page read err %" PRIx64 ":%" PRIx64 " vs %lx:%lx\n", pe->vaddr, pe->nr_pages, vaddr, nr);
		BUG();
	}
}

static int read_parent_page(struct page_read *pr, unsigned long vaddr, unsigned long int nr, void *buf, unsigned flags)
{
	struct page_read *ppr = pr->parent;
	int ret;

	if (!ppr) {
		pr_err("No parent for snapshot pagemap\n");
		return -1;
	}

	/*
	 * Parent pagemap at this point entry may be shorter
	 * than the current vaddr:nr needs, so we have to
	 * carefully 'split' the vaddr:nr into pieces and go
	 * to parent page-read with the longest requests it
	 * can handle.
	 */

	do {
		unsigned long int p_nr;

		pr_debug("\tpr%lu-%u Read from parent\n", pr->img_id, pr->id);
		ret = ppr->seek_pagemap(ppr, vaddr);
		if (ret <= 0) {
			pr_err("Missing %lx in parent pagemap\n", vaddr);
			return -1;
		}

		/*
		 * This is how many pages we have in the parent
		 * page_read starting from vaddr. Go ahead and
		 * read as much as we can.
		 */
		p_nr = ppr->pe->nr_pages - (vaddr - ppr->pe->vaddr) / PAGE_SIZE;
		pr_info("\tparent has %lu pages in\n", p_nr);
		if (p_nr > nr)
			p_nr = nr;

		ret = ppr->read_pages(ppr, vaddr, p_nr, buf, flags);
		if (ret == -1)
			return ret;

		/*
		 * OK, let's see how much data we have left and go
		 * to parent page-read again for the next pagemap
		 * entry.
		 */
		nr -= p_nr;
		vaddr += p_nr * PAGE_SIZE;
		buf += p_nr * PAGE_SIZE;
	} while (nr);

	return 0;
}

static int read_indexed_pages(struct page_read *pr, off_t logical_off, unsigned long len, void *buf)
{
	int fd;
	unsigned long nr_pages = len / PAGE_SIZE;
	unsigned long nr_refs = 0;
	unsigned long nr_groups = 0;
	unsigned long nr_runs = 0;
	unsigned long zero_pages = 0;
	unsigned long duplicate_pages = 0;
	struct timeval tv0, tv_index_done, tv2, tv3, tv4, tv_end;
	u64 offsets_on_stack[64];
	u64 *offsets = offsets_on_stack;
	struct compact_page_ref *refs = NULL;
	struct compact_page_group *groups = NULL;
	unsigned int *group_fill = NULL;
	unsigned long i;
	int index_fd = img_raw_fd(pr->pidx);
	off_t index_off = (logical_off / PAGE_SIZE) * sizeof(u64);
	int ret = 0;

	fd = img_raw_fd(pr->pi);
	if (fd < 0 || index_fd < 0) {
		pr_err("Failed getting raw image fd for compacted page read\n");
		return -1;
	}

	if (nr_pages > ARRAY_SIZE(offsets_on_stack)) {
		offsets = xmalloc(nr_pages * sizeof(*offsets));
		if (!offsets)
			return -1;
	}

	gettimeofday(&tv0, NULL);
	{
		size_t curr = 0;

		while (curr < nr_pages * sizeof(*offsets)) {
			ssize_t read_ret = pread(index_fd, (char *)offsets + curr, nr_pages * sizeof(*offsets) - curr, index_off + curr);

			if (read_ret < 1) {
				pr_perror("Can't read compacted page index");
				ret = -1;
				goto out;
			}
			curr += read_ret;
		}
	}
	gettimeofday(&tv_index_done, NULL);

	refs = xmalloc(nr_pages * sizeof(*refs));
	if (!refs) {
		ret = -1;
		goto out;
	}

	for (i = 0; i < nr_pages; i++) {
		if (offsets[i] == PAGE_INDEX_ZERO) {
			memset((char *)buf + i * PAGE_SIZE, 0, PAGE_SIZE);
			zero_pages++;
			continue;
		}

		refs[nr_refs].off = offsets[i];
		refs[nr_refs].dst = (char *)buf + i * PAGE_SIZE;
		nr_refs++;
	}

	if (nr_refs == 0) {
		tv2 = tv_index_done;
		tv3 = tv_index_done;
		tv4 = tv_index_done;
		tv_end = tv_index_done;
		goto out_log;
	}

	gettimeofday(&tv2, NULL);
	qsort(refs, nr_refs, sizeof(*refs), compact_page_ref_cmp);
	gettimeofday(&tv3, NULL);

	groups = xzalloc(nr_refs * sizeof(*groups));
	group_fill = xzalloc(nr_refs * sizeof(*group_fill));
	if (!groups || !group_fill) {
		ret = -1;
		goto out;
	}

	for (i = 0; i < nr_refs; i++) {
		if (!nr_groups || refs[i].off != groups[nr_groups - 1].off) {
			groups[nr_groups].off = refs[i].off;
			groups[nr_groups].nr_dsts = 1;
			groups[nr_groups].dsts = NULL;
			nr_groups++;
		} else {
			groups[nr_groups - 1].nr_dsts++;
			duplicate_pages++;
		}
	}

	for (i = 0; i < nr_groups; i++) {
		groups[i].dsts = xmalloc(groups[i].nr_dsts * sizeof(*groups[i].dsts));
		if (!groups[i].dsts) {
			ret = -1;
			goto out;
		}
	}

	{
		unsigned long group = 0;

		for (i = 0; i < nr_refs; i++) {
			while (refs[i].off != groups[group].off)
				group++;

			groups[group].dsts[group_fill[group]++] = refs[i].dst;
		}
	}

	if (nr_groups > 0)
		posix_fadvise(fd, (off_t)groups[0].off, (off_t)(groups[nr_groups - 1].off - groups[0].off + PAGE_SIZE),
			      POSIX_FADV_WILLNEED);

	gettimeofday(&tv4, NULL);
	for (i = 0; i < nr_groups; i++) {
		unsigned long run_start = i;
		unsigned long run_pages = 1;

		while (run_start + run_pages < nr_groups &&
		       groups[run_start + run_pages].off == groups[run_start + run_pages - 1].off + PAGE_SIZE)
			run_pages++;

		nr_runs++;

		for (unsigned long chunk_start = 0; chunk_start < run_pages; chunk_start += (unsigned long)IOV_MAX) {
			struct iovec run_iov_stack[64];
			struct iovec bounce_iov_stack[64];
			struct iovec *run_iov = run_iov_stack;
			struct iovec *bounce_iov = NULL;
			struct iovec *read_iov = run_iov;
			unsigned int run_iov_n = run_pages - chunk_start;
			unsigned int j;
			ssize_t read_ret;
			unsigned long chunk_off = (off_t)groups[run_start + chunk_start].off;
			void *aligned_buf = NULL;

			if (run_iov_n > IOV_MAX)
				run_iov_n = IOV_MAX;
			if (run_iov_n > ARRAY_SIZE(run_iov_stack)) {
				run_iov = xmalloc(run_iov_n * sizeof(*run_iov));
				if (!run_iov) {
					ret = -1;
					goto out;
				}
			}

			for (j = 0; j < run_iov_n; j++) {
				run_iov[j].iov_base = groups[run_start + chunk_start + j].dsts[0];
				run_iov[j].iov_len = PAGE_SIZE;
			}

			if (pr->use_direct) {
				bool aligned = (chunk_off & (PAGE_SIZE - 1)) == 0;

				pr->direct_pages += run_iov_n;
				for (j = 0; aligned && j < run_iov_n; j++) {
					if (((unsigned long)run_iov[j].iov_base & (PAGE_SIZE - 1)) ||
					    (run_iov[j].iov_len & (PAGE_SIZE - 1)))
						aligned = false;
				}

				if (!aligned) {
					int err;

					pr->direct_misaligned_pages += run_iov_n;
					pr->direct_misaligned_reads++;
					err = posix_memalign(&aligned_buf, PAGE_SIZE, (size_t)run_iov_n * PAGE_SIZE);
					if (err) {
						pr_err("Can't allocate aligned compact read buffer (%u pages, err=%d)\n",
						       run_iov_n, err);
						if (run_iov != run_iov_stack)
							xfree(run_iov);
						ret = -1;
						goto out;
					}

					bounce_iov = bounce_iov_stack;
					if (run_iov_n > ARRAY_SIZE(bounce_iov_stack)) {
						bounce_iov = xmalloc(run_iov_n * sizeof(*bounce_iov));
						if (!bounce_iov) {
							xfree(aligned_buf);
							if (run_iov != run_iov_stack)
								xfree(run_iov);
							ret = -1;
							goto out;
						}
					}

					for (j = 0; j < run_iov_n; j++) {
						bounce_iov[j].iov_base = (char *)aligned_buf + j * PAGE_SIZE;
						bounce_iov[j].iov_len = PAGE_SIZE;
					}
					read_iov = bounce_iov;
				}
			}

			read_ret = preadv(fd, read_iov, run_iov_n, chunk_off);
			if (read_ret < 0 || (unsigned long)read_ret != run_iov_n * PAGE_SIZE) {
				if (read_ret < 0)
					pr_perror("Can't read compacted page run");
				else
					pr_err("Short read from compacted page run: %zd/%u pages at off %llu\n",
					       read_ret, run_iov_n, (unsigned long long)chunk_off);
				if (bounce_iov && bounce_iov != bounce_iov_stack)
					xfree(bounce_iov);
				xfree(aligned_buf);
				if (run_iov != run_iov_stack)
					xfree(run_iov);
				ret = -1;
				goto out;
			}

			if (aligned_buf) {
				for (j = 0; j < run_iov_n; j++)
					memcpy(run_iov[j].iov_base, bounce_iov[j].iov_base, PAGE_SIZE);
			}

			if (bounce_iov && bounce_iov != bounce_iov_stack)
				xfree(bounce_iov);
			xfree(aligned_buf);
			if (run_iov != run_iov_stack)
				xfree(run_iov);
		}

		for (unsigned long j = 0; j < run_pages; j++) {
			for (unsigned int k = 1; k < groups[run_start + j].nr_dsts; k++)
				memcpy(groups[run_start + j].dsts[k], groups[run_start + j].dsts[0], PAGE_SIZE);
		}

		i = run_start + run_pages - 1;
	}
	gettimeofday(&tv_end, NULL);

out_log:
	if (!ret) {
		unsigned long total_ms = (unsigned long)((tv_end.tv_sec - tv0.tv_sec) * 1000 + (tv_end.tv_usec - tv0.tv_usec) / 1000);
		unsigned long index_ms = (unsigned long)((tv_index_done.tv_sec - tv0.tv_sec) * 1000 +
							 (tv_index_done.tv_usec - tv0.tv_usec) / 1000);
		unsigned long sort_ms = (unsigned long)((tv3.tv_sec - tv2.tv_sec) * 1000 + (tv3.tv_usec - tv2.tv_usec) / 1000);
		unsigned long replay_ms = (unsigned long)((tv_end.tv_sec - tv4.tv_sec) * 1000 + (tv_end.tv_usec - tv4.tv_usec) / 1000);

		pr_info("compact page replay %lu ms (index %lu ms sort %lu ms replay %lu ms, %lu unique pages, %lu duplicate pages, %lu zero pages, %lu runs)\n",
			total_ms,
			index_ms, sort_ms, replay_ms, nr_groups, duplicate_pages, zero_pages, nr_runs);
	}

out:
	if (groups) {
		for (i = 0; i < nr_groups; i++)
			xfree(groups[i].dsts);
		xfree(groups);
	}
	xfree(group_fill);
	xfree(refs);
	if (offsets != offsets_on_stack)
		xfree(offsets);

	return ret;
}

#define COMPACT_IO_RUN_FLAG       ((u64)1 << 62)
#define COMPACT_IO_ZERO_FLAG      ((u64)1 << 61)
#define COMPACT_IO_ZERO_SKIP_FLAG ((u64)1 << 60)
#define COMPACT_IO_FLAG_MASK      (COMPACT_IO_RUN_FLAG | COMPACT_IO_ZERO_FLAG | COMPACT_IO_ZERO_SKIP_FLAG)

static inline bool compact_io_is_zero(off_t off)
{
	return (((u64)off) & COMPACT_IO_ZERO_FLAG) != 0;
}

static inline bool compact_io_is_zero_skip(off_t off)
{
	return ((((u64)off) & (COMPACT_IO_ZERO_FLAG | COMPACT_IO_ZERO_SKIP_FLAG)) ==
		(COMPACT_IO_ZERO_FLAG | COMPACT_IO_ZERO_SKIP_FLAG));
}

static inline bool compact_io_is_run(off_t off)
{
	return (((u64)off) & COMPACT_IO_RUN_FLAG) != 0;
}

static inline bool compact_io_is_tagged(off_t off)
{
	return (((u64)off) & COMPACT_IO_FLAG_MASK) != 0;
}

static inline off_t compact_io_encode_run(u64 off)
{
	return (off_t)(COMPACT_IO_RUN_FLAG | off);
}

static inline off_t compact_io_encode_zero(void)
{
	return (off_t)COMPACT_IO_ZERO_FLAG;
}

static inline off_t compact_io_encode_zero_skip(void)
{
	return (off_t)(COMPACT_IO_ZERO_FLAG | COMPACT_IO_ZERO_SKIP_FLAG);
}

static inline off_t compact_io_decode(off_t off)
{
	return (off_t)(((u64)off) & ~COMPACT_IO_FLAG_MASK);
}

static int read_page_index_offsets(struct page_read *pr, off_t logical_off, unsigned long nr_pages, u64 *offsets)
{
	ssize_t ret;
	size_t curr = 0;
	int index_fd = img_raw_fd(pr->pidx);
	off_t index_off = (logical_off / PAGE_SIZE) * sizeof(u64);

	if (index_fd < 0) {
		pr_err("Failed getting compacted page index fd\n");
		return -1;
	}

	while (curr < nr_pages * sizeof(*offsets)) {
		ret = pread(index_fd, (char *)offsets + curr, nr_pages * sizeof(*offsets) - curr, index_off + curr);
		if (ret < 1) {
			pr_perror("Can't read compacted page index");
			return -1;
		}
		curr += ret;
	}

	return 0;
}

static int read_local_page(struct page_read *pr, unsigned long vaddr, unsigned long len, void *buf)
{
	int fd;
	ssize_t ret;
	size_t curr = 0;
	void *aligned_buf = NULL;
	void *read_buf = buf;
	unsigned long nr_pages = len / PAGE_SIZE;

	if (pr->pidx) {
		return read_indexed_pages(pr, pr->pi_off, len, buf);
	}

	fd = img_raw_fd(pr->pi);
	if (fd < 0) {
		pr_err("Failed getting raw image fd\n");
		return -1;
	}
	/*
	 * Flush any pending async requests if any not to break the
	 * linear reading from the pages.img file.
	 */
	if (pr->sync(pr))
		return -1;

	if (pr->use_direct) {
		pr->direct_pages += nr_pages;
		if (((unsigned long)buf & (PAGE_SIZE - 1)) || (len & (PAGE_SIZE - 1)) || (pr->pi_off & (PAGE_SIZE - 1))) {
			int err;

			pr->direct_misaligned_pages += nr_pages;
			pr->direct_misaligned_reads++;

			err = posix_memalign(&aligned_buf, PAGE_SIZE, len);
			if (err) {
				pr_err("Can't allocate aligned buffer for direct read (%lu bytes, err=%d)\n", len, err);
				return -1;
			}
			read_buf = aligned_buf;
		}
	}

	pr_debug("\tpr%lu-%u Read page from self %lx/%" PRIx64 "\n", pr->img_id, pr->id, pr->cvaddr, pr->pi_off);
	while (1) {
		ret = pread(fd, read_buf + curr, len - curr, pr->pi_off + curr);
		if (ret < 1) {
			pr_perror("Can't read mapping page %zd", ret);
			xfree(aligned_buf);
			return -1;
		}
		curr += ret;
		if (curr == len)
			break;
	}

	if (aligned_buf) {
		memcpy(buf, aligned_buf, len);
		xfree(aligned_buf);
	}

	if (opts.auto_dedup && !pr->disable_dedup) {
		ret = punch_hole(pr, pr->pi_off, len, false);
		if (ret == -1)
			return -1;
	}

	return 0;
}

static int enqueue_iov_range(struct list_head *to, off_t from, void *buf, unsigned long len)
{
	struct page_read_iov *cur_async = NULL;
	struct page_read_iov *pr_iov;
	struct iovec *iov;
	off_t cur_end;
	bool same_zero;
	bool same_phys_run;

	if (!list_empty(to))
		cur_async = list_entry(to->prev, struct page_read_iov, l);

	if (cur_async) {
		cur_end = cur_async->end;
		same_zero = compact_io_is_zero(cur_async->from) && compact_io_is_zero(from);
		same_phys_run = false;
		if (!compact_io_is_tagged(cur_async->from) && !compact_io_is_tagged(from))
			same_phys_run = from == cur_async->end;
		else if (compact_io_is_run(cur_async->from) && compact_io_is_run(from))
			same_phys_run = compact_io_decode(from) == compact_io_decode(cur_end);
	}

	if (cur_async && (same_zero || same_phys_run)) {
		iov = &cur_async->to[cur_async->nr - 1];
		if (iov->iov_base + iov->iov_len == buf) {
			iov->iov_len += len;
		} else {
			unsigned int n_iovs = cur_async->nr + 1;

			if (n_iovs >= IOV_MAX)
				cur_async = NULL;
			else {
				iov = xrealloc(cur_async->to, n_iovs * sizeof(*iov));
				if (!iov)
					return -1;

				cur_async->to = iov;
				cur_async->to_base = iov;
				iov += cur_async->nr;
				iov->iov_base = buf;
				iov->iov_len = len;
				cur_async->nr = n_iovs;
			}
		}

		if (cur_async) {
			cur_async->end += len;
			return 0;
		}
	}

	pr_iov = xzalloc(sizeof(*pr_iov));
	if (!pr_iov)
		return -1;

	pr_iov->from = from;
	pr_iov->end = from + len;

	iov = xzalloc(sizeof(*iov));
	if (!iov) {
		xfree(pr_iov);
		return -1;
	}

	iov->iov_base = buf;
	iov->iov_len = len;

	pr_iov->to = iov;
	pr_iov->to_base = iov;
	pr_iov->nr = 1;

	list_add_tail(&pr_iov->l, to);

	return 0;
}

static int enqueue_async_iov(struct page_read *pr, void *buf, unsigned long len, struct list_head *to)
{
	return enqueue_iov_range(to, pr->pi_off, buf, len);
}

static int enqueue_compact_iovecs(struct page_read *pr, void *buf, unsigned long len, struct list_head *to)
{
	unsigned long nr_pages = len / PAGE_SIZE;
	u64 offsets_on_stack[64];
	u64 *offsets = offsets_on_stack;
	struct compact_page_ref *refs = NULL;
	struct compact_page_group *groups = NULL;
	unsigned int *group_fill = NULL;
	unsigned long nr_refs = 0;
	unsigned long nr_groups = 0;
	unsigned long i;
	unsigned long zero_run_pages = 0;
	unsigned long zero_run_start = 0;
	int ret = 0;

	if (nr_pages > ARRAY_SIZE(offsets_on_stack)) {
		offsets = xmalloc(nr_pages * sizeof(*offsets));
		if (!offsets)
			return -1;
	}

	ret = read_page_index_offsets(pr, pr->pi_off, nr_pages, offsets);
	if (ret)
		goto out;

	for (i = 0; i < nr_pages; i++) {
		bool is_zero = offsets[i] == PAGE_INDEX_ZERO;

		if (is_zero) {
			if (zero_run_pages == 0)
				zero_run_start = i;
			zero_run_pages++;
			continue;
		}

		if (zero_run_pages > 0) {
			ret = enqueue_iov_range(to, pr->zero_skip ? compact_io_encode_zero_skip() : compact_io_encode_zero(),
					      (char *)buf + zero_run_start * PAGE_SIZE,
					      zero_run_pages * PAGE_SIZE);
			if (ret)
				goto out;
			zero_run_pages = 0;
		}

		nr_refs++;
	}

	if (zero_run_pages > 0) {
		ret = enqueue_iov_range(to, pr->zero_skip ? compact_io_encode_zero_skip() : compact_io_encode_zero(),
				      (char *)buf + zero_run_start * PAGE_SIZE,
				      zero_run_pages * PAGE_SIZE);
		if (ret)
			goto out;
	}

	if (nr_refs == 0)
		goto out;

	refs = xmalloc(nr_refs * sizeof(*refs));
	if (!refs) {
		ret = -1;
		goto out;
	}

	for (i = 0, nr_refs = 0; i < nr_pages; i++) {
		if (offsets[i] == PAGE_INDEX_ZERO)
			continue;

		refs[nr_refs].off = offsets[i];
		refs[nr_refs].dst = (char *)buf + i * PAGE_SIZE;
		nr_refs++;
	}

	qsort(refs, nr_refs, sizeof(*refs), compact_page_ref_cmp);

	groups = xzalloc(nr_refs * sizeof(*groups));
	group_fill = xzalloc(nr_refs * sizeof(*group_fill));
	if (!groups || !group_fill) {
		ret = -1;
		goto out;
	}

	for (i = 0; i < nr_refs; i++) {
		if (!nr_groups || refs[i].off != groups[nr_groups - 1].off) {
			groups[nr_groups].off = refs[i].off;
			groups[nr_groups].nr_dsts = 1;
			nr_groups++;
		} else {
			groups[nr_groups - 1].nr_dsts++;
		}
	}

	for (i = 0; i < nr_groups; i++) {
		groups[i].dsts = xmalloc(groups[i].nr_dsts * sizeof(*groups[i].dsts));
		if (!groups[i].dsts) {
			ret = -1;
			goto out;
		}
	}

	{
		unsigned long group = 0;

		for (i = 0; i < nr_refs; i++) {
			while (refs[i].off != groups[group].off)
				group++;

			groups[group].dsts[group_fill[group]++] = refs[i].dst;
		}
	}

	for (i = 0; i < nr_groups; ) {
		unsigned long run_start = i;
		unsigned long run_pages = 1;
		unsigned int nr_copies = 0;
		unsigned long j;
		struct page_read_iov *piov;

		while (run_start + run_pages < nr_groups && run_pages < IOV_MAX &&
		       groups[run_start + run_pages].off == groups[run_start + run_pages - 1].off + PAGE_SIZE)
			run_pages++;

		for (j = 0; j < run_pages; j++)
			nr_copies += groups[run_start + j].nr_dsts - 1;

		piov = xzalloc(sizeof(*piov));
		if (!piov) {
			ret = -1;
			goto out;
		}

		piov->from = compact_io_encode_run(groups[run_start].off);
		piov->end = piov->from + run_pages * PAGE_SIZE;
		piov->nr = run_pages;
		piov->nr_copies = nr_copies;
		piov->to = xmalloc(run_pages * sizeof(*piov->to));
		if (!piov->to) {
			free_page_read_iov(piov);
			ret = -1;
			goto out;
		}
		piov->to_base = piov->to;

		if (nr_copies > 0) {
			piov->copies = xmalloc(nr_copies * sizeof(*piov->copies));
			if (!piov->copies) {
				free_page_read_iov(piov);
				ret = -1;
				goto out;
			}
		}

		nr_copies = 0;
		for (j = 0; j < run_pages; j++) {
			void *src = groups[run_start + j].dsts[0];
			unsigned int k;

			piov->to[j].iov_base = src;
			piov->to[j].iov_len = PAGE_SIZE;

			for (k = 1; k < groups[run_start + j].nr_dsts; k++) {
				piov->copies[nr_copies].src = src;
				piov->copies[nr_copies].dst = groups[run_start + j].dsts[k];
				nr_copies++;
			}
		}

		ret = append_compact_page_read_iov(to, piov);
		if (ret)
			goto out;

		i = run_start + run_pages;
	}

out:
	if (groups) {
		for (i = 0; i < nr_groups; i++)
			xfree(groups[i].dsts);
	}
	xfree(group_fill);
	xfree(groups);
	xfree(refs);
	if (offsets != offsets_on_stack)
		xfree(offsets);

	return ret;
}

int pagemap_render_iovec(struct list_head *from, struct task_restore_args *ta)
{
	struct page_read_iov *piov;
	unsigned int compact_jobs = 0;
	unsigned int zero_jobs = 0;
	unsigned int max_iovs = 0;
	unsigned int max_copies = 0;
	unsigned long compact_pages = 0;
	unsigned long zero_pages = 0;
	unsigned long copy_pages = 0;

	ta->vma_ios = (struct restore_vma_io *)rst_mem_align_cpos(RM_PRIVATE);
	ta->vma_ios_n = 0;

	list_for_each_entry(piov, from, l) {
		struct restore_vma_io *rio;
		struct restore_vma_copy *copies;

		pr_info("`- render %d iovs + %u copies (%p:%zd...)\n",
			piov->nr, piov->nr_copies, piov->to[0].iov_base, piov->to[0].iov_len);
		rio = rst_mem_alloc(RIO_SIZE(piov->nr, piov->nr_copies), RM_PRIVATE);
		if (!rio)
			return -1;

		rio->nr_iovs = piov->nr;
		rio->nr_copies = piov->nr_copies;
		rio->off = piov->from;
		memcpy(rio->iovs, piov->to, piov->nr * sizeof(struct iovec));
		if (piov->nr_copies > 0) {
			copies = restore_vma_io_copies(rio);
			memcpy(copies, piov->copies, piov->nr_copies * sizeof(*copies));
		}

		if (compact_io_is_zero(piov->from)) {
			zero_jobs++;
			zero_pages += (unsigned long)((piov->end - piov->from) / PAGE_SIZE);
		} else if (compact_io_is_run(piov->from)) {
			compact_jobs++;
			compact_pages += piov->nr;
			copy_pages += piov->nr_copies;
		}

		if (piov->nr > max_iovs)
			max_iovs = piov->nr;
		if (piov->nr_copies > max_copies)
			max_copies = piov->nr_copies;

		ta->vma_ios_n++;
	}

	pr_info("pagemap render profile rios %u compact %u (%lu unique pages, %lu copy pages) zero %u (%lu zero pages) max %u iovs max %u copies\n",
		ta->vma_ios_n, compact_jobs, compact_pages, copy_pages, zero_jobs, zero_pages, max_iovs, max_copies);

	return 0;
}

int pagemap_enqueue_iovec(struct page_read *pr, void *buf, unsigned long len, struct list_head *to)
{
	struct page_read_iov *cur_async = NULL;
	struct iovec *iov;

	if (pr->pidx)
		return enqueue_compact_iovecs(pr, buf, len, to);

	if (!list_empty(to))
		cur_async = list_entry(to->prev, struct page_read_iov, l);

	/*
	 * We don't have any async requests or we have new read
	 * request that should happen at pos _after_ some hole from
	 * the previous one.
	 * Start the new preadv request here.
	 */
	if (!cur_async || pr->pi_off != cur_async->end)
		return enqueue_async_iov(pr, buf, len, to);

	/*
	 * This read is pure continuation of the previous one. Let's
	 * just add another IOV (or extend one of the existing).
	 */
	iov = &cur_async->to[cur_async->nr - 1];
	if (iov->iov_base + iov->iov_len == buf) {
		/* Extendable */
		iov->iov_len += len;
	} else {
		/* Need one more target iovec */
		unsigned int n_iovs = cur_async->nr + 1;

		if (n_iovs >= IOV_MAX)
			return enqueue_async_iov(pr, buf, len, to);

		iov = xrealloc(cur_async->to, n_iovs * sizeof(*iov));
		if (!iov)
			return -1;

		cur_async->to = iov;
		cur_async->to_base = iov;

		iov += cur_async->nr;
		iov->iov_base = buf;
		iov->iov_len = len;

		cur_async->nr = n_iovs;
	}

	cur_async->end += len;

	return 0;
}

static int maybe_read_page_local(struct page_read *pr, unsigned long vaddr, unsigned long nr, void *buf, unsigned flags)
{
	int ret;
	unsigned long len = nr * PAGE_SIZE;

	/*
	 * There's no API in the kernel to start asynchronous
	 * cached read (or write), so in case someone is asking
	 * for us for urgent async read, just do the regular
	 * cached read.
	 */
	if ((flags & (PR_ASYNC | PR_ASAP)) != PR_ASYNC) {
		ret = read_local_page(pr, vaddr, len, buf);
		if (ret == 0 && pr->io_complete)
			ret = pr->io_complete(pr, vaddr, nr);
	} else
		ret = pagemap_enqueue_iovec(pr, buf, len, &pr->async);

	pr->pi_off += len;

	return ret;
}

/*
 * We cannot use maybe_read_page_local() for streaming images as it uses
 * pread(), seeking in the file. Instead, we use this custom page reader.
 */
static int maybe_read_page_img_streamer(struct page_read *pr, unsigned long vaddr, unsigned long nr, void *buf, unsigned flags)
{
	unsigned long len = nr * PAGE_SIZE;
	int fd;
	int ret;
	size_t curr = 0;

	fd = img_raw_fd(pr->pi);
	if (fd < 0) {
		pr_err("Getting raw FD failed\n");
		return -1;
	}

	if (pr->pidx) {
		pr_err("Compacted page images do not support streamed restore\n");
		return -1;
	}

	pr_debug("\tpr%lu-%u Read page from self %lx/%" PRIx64 "\n", pr->img_id, pr->id, pr->cvaddr, pr->pi_off);

	/* We can't seek. The requested address better match */
	BUG_ON(pr->cvaddr != vaddr);

	while (1) {
		ret = read(fd, buf + curr, len - curr);
		if (ret == 0) {
			pr_err("Reached EOF unexpectedly while reading page from image\n");
			return -1;
		} else if (ret < 0) {
			pr_perror("Can't read mapping page %d", ret);
			return -1;
		}
		curr += ret;
		if (curr == len)
			break;
	}

	if (opts.auto_dedup)
		pr_warn_once("Can't dedup when streaming images\n");

	if (pr->io_complete)
		ret = pr->io_complete(pr, vaddr, nr);

	pr->pi_off += len;

	return ret;
}

static int read_page_complete(unsigned long img_id, unsigned long vaddr, unsigned long int nr_pages, void *priv)
{
	int ret = 0;
	struct page_read *pr = priv;

	if (pr->img_id != img_id) {
		pr_err("Out of order read completed (want %lu have %lu)\n", pr->img_id, img_id);
		return -1;
	}

	if (pr->io_complete)
		ret = pr->io_complete(pr, vaddr, nr_pages);
	else
		pr_warn_once("Remote page read w/o io_complete!\n");

	return ret;
}

static int maybe_read_page_remote(struct page_read *pr, unsigned long vaddr, unsigned long nr, void *buf, unsigned flags)
{
	int ret;

	/* We always do PR_ASAP mode here (FIXME?) */
	ret = request_remote_pages(pr->img_id, vaddr, nr);
	if (!ret)
		ret = page_server_start_read(buf, nr, read_page_complete, pr, flags);
	return ret;
}

static int read_pagemap_page(struct page_read *pr, unsigned long vaddr, unsigned long nr, void *buf, unsigned flags)
{
	pr_info("pr%lu-%u Read %lx %lu pages\n", pr->img_id, pr->id, vaddr, nr);
	pagemap_bound_check(pr->pe, vaddr, nr);

	if (pagemap_in_parent(pr->pe)) {
		if (read_parent_page(pr, vaddr, nr, buf, flags) < 0)
			return -1;
	} else {
		if (pr->maybe_read_page(pr, vaddr, nr, buf, flags) < 0)
			return -1;
	}

	pr->cvaddr += nr * PAGE_SIZE;

	return 1;
}

static void free_pagemaps(struct page_read *pr)
{
	int i;

	for (i = 0; i < pr->nr_pmes; i++)
		pagemap_entry__free_unpacked(pr->pmes[i], NULL);

	xfree(pr->pmes);
	pr->pmes = NULL;
}

static void advance_piov(struct page_read_iov *piov, ssize_t len)
{
	ssize_t olen = len;
	int onr = piov->nr;
	piov->from += len;

	while (len) {
		struct iovec *cur = piov->to;

		if (cur->iov_len <= len) {
			piov->to++;
			piov->nr--;
			len -= cur->iov_len;
			continue;
		}

		cur->iov_base += len;
		cur->iov_len -= len;
		break;
	}

	pr_debug("Advanced iov %zu bytes, %d->%d iovs, %zu tail\n", olen, onr, piov->nr, len);
}

/*
 * Drain (free without reading) all async entries in pr and its parent chain.
 * Called on error paths to satisfy BUG_ON(!list_empty(&pr->async)) in
 * close_page_read().
 */
static void drain_async_queue(struct page_read *pr)
{
	struct page_read_iov *piov, *n;

	list_for_each_entry_safe(piov, n, &pr->async, l) {
		list_del(&piov->l);
		free_page_read_iov(piov);
	}
	if (pr->parent)
		drain_async_queue(pr->parent);
}

static int process_async_reads(struct page_read *pr)
{
	int fd, ret = 0;
	struct page_read_iov *piov, *n;
	off_t first_off = 0, last_end = 0;
	bool have_range = false;
	struct timeval tv0, tv1, tv2, copy0, copy1;
	size_t zero_bytes = 0;
	size_t zero_skip_bytes = 0;
	size_t copy_bytes = 0;
	size_t read_bytes = 0;
	unsigned int zero_ios = 0;
	unsigned int zero_skip_ios = 0;
	unsigned int copy_ios = 0;
	unsigned int read_ios = 0;
	unsigned int short_reads = 0;
	unsigned int max_iovs = 0;
	unsigned int max_copies = 0;
	unsigned int queue_jobs = 0;
	unsigned int merged_jobs = 0;
	u64 copy_us = 0;
	u64 sort_us = 0;
	u64 merge_us = 0;

	fd = img_raw_fd(pr->pi);
	prepare_compact_async_queue(pr, &sort_us, &merge_us, &queue_jobs, &merged_jobs);
	/* Pre-warm page cache for all async read ranges (fadvise + preadv strategy) */
	gettimeofday(&tv0, NULL);
	list_for_each_entry(piov, &pr->async, l) {
		off_t file_from;
		off_t file_end;

		if (compact_io_is_zero(piov->from))
			continue;
		file_from = compact_io_decode(piov->from);
		file_end = file_from + (piov->end - piov->from);
		if (!have_range) {
			first_off = file_from;
			last_end = file_end;
			have_range = true;
		} else {
			if (file_from < first_off)
				first_off = file_from;
			if (file_end > last_end)
				last_end = file_end;
		}
	}
	if (have_range && last_end > first_off) {
		if (posix_fadvise(fd, first_off, (off_t)(last_end - first_off), POSIX_FADV_WILLNEED) != 0)
			pr_debug("posix_fadvise(WILLNEED) failed for async range\n");
	}
	gettimeofday(&tv1, NULL);
	list_for_each_entry_safe(piov, n, &pr->async, l) {
		ssize_t bytes;
		bool io_failed = false;
		size_t remaining = (size_t)(piov->end - piov->from);
		off_t file_from = compact_io_decode(piov->from);

		if (compact_io_is_zero(piov->from)) {
			unsigned int i;

			if (compact_io_is_zero_skip(piov->from)) {
				zero_skip_bytes += remaining;
				zero_skip_ios++;
			} else {
				for (i = 0; i < piov->nr; i++)
					memset(piov->to[i].iov_base, 0, piov->to[i].iov_len);
				zero_bytes += remaining;
				zero_ios++;
			}
			list_del(&piov->l);
			free_page_read_iov(piov);
			continue;
		}

		pr_debug("Read piov iovs %d, from %lld, len %zu, first %p:%zu\n", piov->nr,
			 (long long)file_from, remaining, piov->to->iov_base, piov->to->iov_len);
		read_ios++;
		read_bytes += remaining;
		if (piov->nr > max_iovs)
			max_iovs = piov->nr;
		if (piov->nr_copies > max_copies)
			max_copies = piov->nr_copies;
		if (pr->use_direct) {
			if (direct_read_iovecs(pr, fd, file_from, piov->to, piov->nr, remaining, &short_reads)) {
				io_failed = true;
			} else if (opts.auto_dedup && !pr->disable_dedup && punch_hole(pr, file_from, remaining, false)) {
				io_failed = true;
			}
			goto read_done;
		}
	more:
		bytes = preadv(fd, piov->to, piov->nr, file_from);
		if (fault_injected(FI_PARTIAL_PAGES)) {
			/*
			 * We might have read everything, but for debug
			 * purposes let's try to force the advance_piov()
			 * and re-read tail.
			 */
			if (bytes > 0 && piov->nr >= 2) {
				pr_debug("`- trim preadv %zu\n", bytes);
				bytes /= 2;
			}
		}

		if (bytes < 0) {
			pr_err("Can't read async pr bytes (%zd / %zu read, %lld off, %d iovs)\n", bytes, remaining,
			       (long long)file_from, piov->nr);
			io_failed = true;
		} else if (bytes == 0) {
			pr_err("Unexpected EOF in async page read (%zu bytes remaining at off %lld, %d iovs)\n", remaining,
			       (long long)file_from, piov->nr);
			io_failed = true;
		} else if (opts.auto_dedup && !pr->disable_dedup && punch_hole(pr, file_from, bytes, false)) {
			io_failed = true;
		}

		if (!io_failed && (size_t)bytes != remaining) {
			/*
			 * The preadv() can return less than requested. It's
			 * valid and doesn't mean error or EOF. We should advance
			 * the iovecs and continue
			 *
			 * Modify the piov in-place, we're going to drop this one
			 * anyway.
			 */
			short_reads++;
			advance_piov(piov, bytes);
			file_from += bytes;
			remaining -= bytes;
			goto more;
		}
read_done:

		/*
		 * On I/O failure drain all remaining async entries (current pr
		 * and parent chain) so that BUG_ON(!list_empty(&pr->async)) in
		 * close_page_read() is satisfied.
		 */
		if (io_failed) {
			list_del(&piov->l);
			free_page_read_iov(piov);
			drain_async_queue(pr);
			return -1;
		}

		if (piov->nr_copies > 0) {
			gettimeofday(&copy0, NULL);
			for (unsigned int i = 0; i < piov->nr_copies; i++)
				memcpy(piov->copies[i].dst, piov->copies[i].src, PAGE_SIZE);
			gettimeofday(&copy1, NULL);
			copy_us += (u64)(copy1.tv_sec - copy0.tv_sec) * 1000000ULL +
				   (u64)(copy1.tv_usec - copy0.tv_usec);
			copy_bytes += (size_t)piov->nr_copies * PAGE_SIZE;
			copy_ios += piov->nr_copies;
		}

		BUG_ON(pr->io_complete); /* FIXME -- implement once needed */

		list_del(&piov->l);
		free_page_read_iov(piov);
	}
	gettimeofday(&tv2, NULL);
	if (have_range && last_end > first_off) {
		unsigned long fadv_ms = (unsigned long)((tv1.tv_sec - tv0.tv_sec) * 1000 + (tv1.tv_usec - tv0.tv_usec) / 1000);
		unsigned long preadv_ms = (unsigned long)((tv2.tv_sec - tv1.tv_sec) * 1000 + (tv2.tv_usec - tv1.tv_usec) / 1000);
		pr_info("pagemap async sort %lu ms merge %lu ms jobs %u->%u fadvise %lu ms preadv %lu ms zero-skip %zu MiB (%u ios) zero-fill %zu MiB (%u ios) copy %lu ms (%zu MiB, %u ios) read %zu MiB (%u ios, %u short, max %u iovs, max %u copies) (range %zu MiB)\n",
			(unsigned long)(sort_us / 1000ULL), (unsigned long)(merge_us / 1000ULL), queue_jobs, merged_jobs,
			fadv_ms, preadv_ms, zero_skip_bytes / (1024 * 1024), zero_skip_ios,
			zero_bytes / (1024 * 1024), zero_ios,
			(unsigned long)(copy_us / 1000ULL), copy_bytes / (1024 * 1024), copy_ios,
			read_bytes / (1024 * 1024), read_ios, short_reads, max_iovs, max_copies,
			(size_t)((last_end - first_off) / (1024 * 1024)));
	} else if (zero_skip_bytes > 0 || zero_bytes > 0 || copy_bytes > 0) {
		unsigned long zero_ms = (unsigned long)((tv2.tv_sec - tv1.tv_sec) * 1000 + (tv2.tv_usec - tv1.tv_usec) / 1000);
		pr_info("pagemap async sort %lu ms merge %lu ms jobs %u->%u zero-skip %zu MiB (%u ios) zero-fill %lu ms (%zu MiB, %u ios) copy %lu ms (%zu MiB, %u ios) read %zu MiB (%u ios, %u short, max %u iovs, max %u copies)\n",
			(unsigned long)(sort_us / 1000ULL), (unsigned long)(merge_us / 1000ULL), queue_jobs, merged_jobs,
			zero_skip_bytes / (1024 * 1024), zero_skip_ios,
			zero_ms, zero_bytes / (1024 * 1024), zero_ios,
			(unsigned long)(copy_us / 1000ULL), copy_bytes / (1024 * 1024), copy_ios,
			read_bytes / (1024 * 1024), read_ios, short_reads, max_iovs, max_copies);
	}

	if (pr->parent)
		ret = process_async_reads(pr->parent);

	return ret;
}

static void close_page_read(struct page_read *pr)
{
	int ret;

	BUG_ON(!list_empty(&pr->async));

	if (pr->bunch.iov_len > 0) {
		ret = punch_hole(pr, 0, 0, true);
		if (ret == -1)
			return;

		pr->bunch.iov_len = 0;
	}

	if (pr->parent) {
		close_page_read(pr->parent);
		xfree(pr->parent);
	}

	if (pr->pmi)
		close_image(pr->pmi);
	if (pr->pi)
		close_image(pr->pi);
	if (pr->pidx)
		close_image(pr->pidx);

	if (pr->direct_pages > 0) {
		double pct = (100.0 * pr->direct_misaligned_pages) / pr->direct_pages;

		pr_info("pr%lu-%u direct-io alignment: %lu/%lu pages non-aligned (%.2f%%) across %lu reads\n",
			pr->img_id, pr->id, pr->direct_misaligned_pages, pr->direct_pages, pct,
			pr->direct_misaligned_reads);
	}

	if (pr->pmes)
		free_pagemaps(pr);
}

static void reset_pagemap(struct page_read *pr)
{
	pr->cvaddr = 0;
	pr->pi_off = 0;
	pr->curr_pme = -1;
	pr->pe = NULL;

	/* FIXME: take care of bunch */

	if (pr->parent)
		reset_pagemap(pr->parent);
}

static int try_open_parent(int dfd, unsigned long id, struct page_read *pr, int pr_flags)
{
	int pfd, ret;
	struct page_read *parent = NULL;

	/* Image streaming lacks support for incremental images */
	if (opts.stream)
		goto out;

	if (open_parent(dfd, &pfd))
		goto err;
	if (pfd < 0)
		goto out;

	parent = xmalloc(sizeof(*parent));
	if (!parent)
		goto err_cl;

	ret = open_page_read_at(pfd, id, parent, pr_flags);
	if (ret < 0)
		goto err_free;

	if (!ret) {
		xfree(parent);
		parent = NULL;
	}

	close(pfd);
out:
	pr->parent = parent;
	return 0;

err_free:
	xfree(parent);
err_cl:
	close(pfd);
err:
	return -1;
}

static void init_compat_pagemap_entry(PagemapEntry *pe)
{
	/*
	 * pagemap image generated with older version will either
	 * contain a hole because the pages are in the parent
	 * snapshot or a pagemap that should be marked with
	 * PE_PRESENT
	 */
	if (pe->has_in_parent && pe->in_parent)
		pe->flags |= PE_PARENT;
	else if (!pe->has_flags)
		pe->flags = PE_PRESENT;

	if (!pe->has_nr_pages)
		pe->nr_pages = pe->compat_nr_pages;
}

/*
 * The pagemap entry size is at least 8 bytes for small mappings with
 * low address and may get to 18 bytes or even more for large mappings
 * with high address and in_parent flag set. 16 seems to be nice round
 * number to minimize {over,under}-allocations
 */
#define PAGEMAP_ENTRY_SIZE_ESTIMATE 16

static int init_pagemaps(struct page_read *pr)
{
	off_t fsize;
	int nr_pmes, nr_realloc;

	if (opts.stream) {
		/*
		 * TODO - There is no easy way to estimate the size of the
		 * pagemap that is still to be read from the pipe. Possible
		 * solution is to ask the image streamer for the size of the
		 * image. 1024 is a wild guess (more space is allocated if
		 * needed).
		 */
		fsize = 1024;
	} else {
		fsize = img_raw_size(pr->pmi);
	}

	if (fsize < 0)
		return -1;

	nr_pmes = fsize / PAGEMAP_ENTRY_SIZE_ESTIMATE + 1;
	nr_realloc = nr_pmes / 2;

	pr->pmes = xzalloc(nr_pmes * sizeof(*pr->pmes));
	if (!pr->pmes)
		return -1;

	pr->nr_pmes = 0;
	pr->curr_pme = -1;

	while (1) {
		int ret = pb_read_one_eof(pr->pmi, &pr->pmes[pr->nr_pmes], PB_PAGEMAP);
		if (ret < 0)
			goto free_pagemaps;
		if (ret == 0)
			break;

		init_compat_pagemap_entry(pr->pmes[pr->nr_pmes]);

		pr->nr_pmes++;
		if (pr->nr_pmes >= nr_pmes) {
			PagemapEntry **new;
			nr_pmes += nr_realloc;
			new = xrealloc(pr->pmes, nr_pmes * sizeof(*pr->pmes));
			if (!new)
				goto free_pagemaps;
			pr->pmes = new;
		}
	}

	close_image(pr->pmi);
	pr->pmi = NULL;

	return 0;

free_pagemaps:
	free_pagemaps(pr);
	return -1;
}

int open_page_read_at(int dfd, unsigned long img_id, struct page_read *pr, int pr_flags)
{
	int flags, i_typ;
	static unsigned ids = 1;
	bool remote = pr_flags & PR_REMOTE;

	/*
	 * Only the top-most page-read can be remote, all the
	 * others are always local.
	 */
	pr_flags &= ~PR_REMOTE;
	if (opts.auto_dedup)
		pr_flags |= PR_MOD;
	if (pr_flags & PR_MOD)
		flags = O_RDWR;
	else
		flags = O_RSTR;

	switch (pr_flags & PR_TYPE_MASK) {
	case PR_TASK:
		i_typ = CR_FD_PAGEMAP;
		break;
	case PR_SHMEM:
		i_typ = CR_FD_SHMEM_PAGEMAP;
		break;
	default:
		BUG();
		return -1;
	}

	INIT_LIST_HEAD(&pr->async);
	pr->pe = NULL;
	pr->parent = NULL;
	pr->cvaddr = 0;
	pr->pi_off = 0;
	pr->bunch.iov_len = 0;
	pr->bunch.iov_base = NULL;
	pr->pmes = NULL;
	pr->pidx = NULL;
	pr->pieok = false;
	pr->disable_dedup = false;
	pr->zero_skip = false;
	pr->use_direct = false;
	pr->direct_pages = 0;
	pr->direct_misaligned_pages = 0;
	pr->direct_misaligned_reads = 0;

	pr->pmi = open_image_at(dfd, i_typ, O_RSTR, img_id);
	if (!pr->pmi)
		return -1;

	if (empty_image(pr->pmi)) {
		close_image(pr->pmi);
		return 0;
	}

	if (try_open_parent(dfd, img_id, pr, pr_flags)) {
		close_image(pr->pmi);
		return -1;
	}

	pr->pi = open_pages_image_at(dfd, flags | O_TRY_DIRECT_OPEN, pr->pmi, &pr->pages_img_id);
	if (!pr->pi || empty_image(pr->pi)) {
		close_page_read(pr);
		return -1;
	}

	if (compact_pages_ready(dfd, pr->pages_img_id)) {
		pr->pidx = open_image_at(dfd, CR_FD_PAGE_INDEX, O_RSTR, pr->pages_img_id);
		if (!pr->pidx) {
			close_page_read(pr);
			return -1;
		}
		if (empty_image(pr->pidx)) {
			close_image(pr->pidx);
			pr->pidx = NULL;
		}
	}

	{
		int pfd = img_raw_fd(pr->pi);

		if (pfd >= 0) {
			int fl = fcntl(pfd, F_GETFL);

			if (fl >= 0 && (fl & O_DIRECT)) {
				pr_debug("O_DIRECT enabled on pages fd %d at open time\n", pfd);
				pr->use_direct = true;
			} else if (!pr->pidx) {
				posix_fadvise(pfd, 0, 0, POSIX_FADV_SEQUENTIAL);
			}
		}
	}

	if (pr->pidx) {
		page_read_disable_dedup(pr);
	}

	if (init_pagemaps(pr)) {
		close_page_read(pr);
		return -1;
	}

	pr->read_pages = read_pagemap_page;
	pr->advance = advance;
	pr->close = close_page_read;
	pr->skip_pages = skip_pagemap_pages;
	pr->sync = process_async_reads;
	pr->seek_pagemap = seek_pagemap;
	pr->reset = reset_pagemap;
	pr->io_complete = NULL; /* set up by the client if needed */
	pr->id = __sync_fetch_and_add(&ids, 1);
	pr->img_id = img_id;

	if (remote)
		pr->maybe_read_page = maybe_read_page_remote;
	else if (opts.stream)
		pr->maybe_read_page = maybe_read_page_img_streamer;
	else {
		bool can_pie = !pr->parent && !opts.lazy_pages;

		pr->maybe_read_page = maybe_read_page_local;
		pr->pieok = can_pie;
		if (pr->pidx)
			pr_info("Compact page read pages_id %u PIE %s (parent %d lazy %d)\n",
				pr->pages_img_id, can_pie ? "enabled" : "disabled", !!pr->parent, !!opts.lazy_pages);
	}

	pr_debug("Opened %s page read %u (parent %u)\n", remote ? "remote" : "local", pr->id,
		 pr->parent ? pr->parent->id : 0);

	return 1;
}

int open_page_read(unsigned long img_id, struct page_read *pr, int pr_flags)
{
	return open_page_read_at(get_service_fd(IMG_FD_OFF), img_id, pr, pr_flags);
}

#define DUP_IDS_BASE 1000

void page_read_disable_dedup(struct page_read *pr)
{
	pr_debug("disable dedup, id: %d\n", pr->id);
	pr->disable_dedup = true;
	if (pr->parent)
		page_read_disable_dedup(pr->parent);
}

void dup_page_read(struct page_read *src, struct page_read *dst)
{
	static int dup_ids = 1;

	memcpy(dst, src, sizeof(*dst));
	INIT_LIST_HEAD(&dst->async);
	dst->id = src->id + DUP_IDS_BASE * dup_ids++;
	dst->direct_pages = 0;
	dst->direct_misaligned_pages = 0;
	dst->direct_misaligned_reads = 0;
	dst->reset(dst);
}
