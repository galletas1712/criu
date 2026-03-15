#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <linux/falloc.h>
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
#include "compression.h"

#include "fault-injection.h"
#include "xmalloc.h"
#include "protobuf.h"
#include "images/pagemap.pb-c.h"

#ifndef SEEK_DATA
#define SEEK_DATA 3
#define SEEK_HOLE 4
#endif

#define MAX_BUNCH_SIZE 256

#define OFF_MAX (sizeof(off_t) == sizeof(long long) ? LLONG_MAX : sizeof(off_t) == sizeof(int) ? INT_MAX : -999999)
#define OFF_MIN (sizeof(off_t) == sizeof(long long) ? LLONG_MIN : sizeof(off_t) == sizeof(int) ? INT_MIN : -999999)

/*
 * One "job" for the preadv() syscall in pagemap.c
 */
struct page_read_iov {
	off_t from;	  /* offset in pi file where to start reading from */
	off_t end;	  /* the end of the read == sum to.iov_len -s */
	struct iovec *to; /* destination iovs */
	unsigned int nr;  /* their number */

	size_t n_compressed_size; /* Number of compressed blocks (pages or regions) */
	uint32_t *compressed_size; /* Per-block compressed sizes */
	uint64_t total_compressed_size; /* Sum of all compressed sizes */
	/*
	 * Region size in pages when this iov holds region-compressed data.
	 * 0 means per-page compression (or no compression).
	 * Iovs with different region_pages cannot be coalesced into one
	 * async batch.
	 */
	unsigned int region_pages;
	/*
	 * Per-block page count when region_pages > 0. Most blocks have
	 * exactly region_pages pages; the last block of any pagemap
	 * entry that spans this piov may be shorter if the entry's
	 * nr_pages is not a multiple of region_pages.
	 */
	uint16_t *block_pages;

	/*
	 * RM_PRIVATE offsets of the compressed_size[] and block_pages[]
	 * copies that pagemap_render_iovec() places in restorer memory.
	 * Stored as offsets (not pointers) because struct restore_vma_io
	 * references them via RST_MEM_FIXUP_PPTR, which adds the remapped
	 * arena base to the stored value.
	 */
	unsigned long rio_cs_off;
	unsigned long rio_bp_off;

	struct list_head l;
};

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
			/*
			 * Skip hole-punching for compressed entries.
			 * fallocate(PUNCH_HOLE) zeroes whole filesystem
			 * blocks (typically 4K). Compressed pages are
			 * smaller than a block and packed contiguously,
			 * so punching one would destroy adjacent pages
			 * that share the same block.
			 */
			if (!pr->pe->n_compressed_size) {
				unsigned long punch_len = min(piov_end, iov_end) - off;

				ret = punch_hole(pr, pr->pi_off, punch_len, false);
				if (ret == -1)
					return ret;
			}
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

	pr->compressed_size_index = 0;
	pr->region_block_offset = 0;

	return 1;
}

/*
 * Return the page count of the current compressed block (in region mode).
 * The last block of an entry may be shorter than region_pages when the
 * entry's nr_pages is not a multiple of region_pages.
 */
static unsigned int current_block_pages(struct page_read *pr)
{
	unsigned int region_pages = pr->pe->region_pages;
	uint64_t pages_before =
		(uint64_t)pr->compressed_size_index * region_pages;
	uint64_t pages_left = pr->pe->nr_pages - pages_before;

	return pages_left < region_pages ? (unsigned int)pages_left : region_pages;
}

/*
 * Advance the file offset (pi_off) by @len bytes without reading
 * the page data. Used by seek_pagemap() to skip over pages that
 * are not needed for the current read request.
 */
static void skip_pagemap_pages(struct page_read *pr, unsigned long len)
{
	if (!len)
		return;

	if (pagemap_present(pr->pe)) {
		if (!pr->pe->n_compressed_size) {
			pr->pi_off += len;
		} else if (pr->pe->has_region_pages && pr->pe->region_pages) {
			/*
			 * Region mode: each compressed_size[] entry covers
			 * up to region_pages pages. Walk forward, advancing
			 * the in-block cursor and crossing block boundaries
			 * by adding the block's compressed size to pi_off.
			 */
			unsigned long nr = len / PAGE_SIZE;

			while (nr > 0) {
				unsigned int bp = current_block_pages(pr);
				unsigned int avail = bp - pr->region_block_offset;
				unsigned int take = nr < avail ? (unsigned int)nr :
								 avail;

				if (pr->compressed_size_index >= pr->pe->n_compressed_size) {
					pr_err("skip_pagemap_pages: index out of bounds: "
					       "%zu >= %zu\n",
					       pr->compressed_size_index,
					       pr->pe->n_compressed_size);
					BUG();
				}

				pr->region_block_offset += take;
				nr -= take;

				if (pr->region_block_offset == bp) {
					pr->pi_off += pr->pe->compressed_size[pr->compressed_size_index];
					pr->compressed_size_index++;
					pr->region_block_offset = 0;
				}
			}
		} else {
			/* Per-page compressed: variable size per page. */
			unsigned long nr = len / PAGE_SIZE;
			unsigned long i;

			if (pr->compressed_size_index + nr > pr->pe->n_compressed_size) {
				pr_err("skip_pagemap_pages: index out of bounds: %zu + %lu > %zu\n",
				       pr->compressed_size_index, nr,
				       pr->pe->n_compressed_size);
				BUG();
			}

			for (i = 0; i < nr; i++)
				pr->pi_off += pr->pe->compressed_size[pr->compressed_size_index + i];
			pr->compressed_size_index += nr;
		}
	}
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

static int read_local_page(struct page_read *pr, unsigned long vaddr, unsigned long len, void *buf)
{
	int fd;
	ssize_t ret;
	size_t curr = 0;

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

	pr_debug("\tpr%lu-%u Read page from self %lx/%" PRIx64 "\n", pr->img_id, pr->id, pr->cvaddr, pr->pi_off);
	while (1) {
		ret = pread(fd, buf + curr, len - curr, pr->pi_off + curr);
		if (ret < 1) {
			pr_perror("Can't read mapping page %zd", ret);
			return -1;
		}
		curr += ret;
		if (curr == len)
			break;
	}

	if (opts.auto_dedup && !pr->disable_dedup) {
		ret = punch_hole(pr, pr->pi_off, len, false);
		if (ret == -1)
			return -1;
	}

	return 0;
}

static int enqueue_async_iov(struct page_read *pr, void *buf, unsigned long len, struct list_head *to)
{
	struct page_read_iov *pr_iov;
	struct iovec *iov;

	pr_iov = xzalloc(sizeof(*pr_iov));
	if (!pr_iov)
		return -1;

	pr_iov->from = pr->pi_off;

	iov = xzalloc(sizeof(*iov));
	if (!iov) {
		xfree(pr_iov);
		return -1;
	}

	iov->iov_base = buf;
	iov->iov_len = len;

	pr_iov->to = iov;
	pr_iov->nr = 1;

	/*
	 * For uncompressed entries, the end offset is simply
	 * start + len. For compressed entries, copy the per-block
	 * sizes (one per page in per-page mode, one per region in
	 * region mode) so that process_async_reads() knows how much
	 * compressed data to read from the image.
	 */
	if (!pr->pe || !pr->pe->n_compressed_size) {
		pr_iov->end = pr->pi_off + len;
	} else {
		unsigned long nr_pages = len / PAGE_SIZE;
		unsigned int region_pages = (pr->pe->has_region_pages &&
					     pr->pe->region_pages) ?
						    pr->pe->region_pages : 0;
		unsigned long n_blocks;
		unsigned long i;

		if (region_pages) {
			uint64_t pages_consumed =
				(uint64_t)pr->compressed_size_index * region_pages;
			uint64_t end_page = pages_consumed + nr_pages;

			/*
			 * Region mode allows two alignment cases: nr_pages is
			 * a multiple of region_pages, or the read finishes
			 * exactly at the entry's end (last region may be
			 * shorter than region_pages).
			 */
			if (pages_consumed % region_pages != 0 ||
			    (nr_pages % region_pages != 0 &&
			     end_page != pr->pe->nr_pages)) {
				pr_err("Region-mode async enqueue not aligned: idx=%zu nr=%lu region=%u nr_pages=%" PRIu64 "\n",
				       pr->compressed_size_index, nr_pages,
				       region_pages, pr->pe->nr_pages);
				xfree(iov);
				xfree(pr_iov);
				return -1;
			}
			n_blocks = (nr_pages + region_pages - 1) / region_pages;
		} else {
			n_blocks = nr_pages;
		}

		if (pr->compressed_size_index + n_blocks > pr->pe->n_compressed_size) {
			pr_err("Compressed size index out of bounds: %zu + %lu > %zu\n",
			       pr->compressed_size_index, n_blocks,
			       pr->pe->n_compressed_size);
			xfree(iov);
			xfree(pr_iov);
			return -1;
		}

		pr_iov->compressed_size = xmalloc(n_blocks * sizeof(uint32_t));
		if (!pr_iov->compressed_size) {
			xfree(iov);
			xfree(pr_iov);
			return -1;
		}
		pr_iov->n_compressed_size = n_blocks;
		pr_iov->total_compressed_size = 0;
		pr_iov->region_pages = region_pages;
		for (i = 0; i < n_blocks; i++) {
			size_t idx = pr->compressed_size_index + i;

			pr_iov->compressed_size[i] = pr->pe->compressed_size[idx];
			pr_iov->total_compressed_size += pr_iov->compressed_size[i];
		}
		if (region_pages) {
			uint64_t pages_consumed_at_start =
				(uint64_t)pr->compressed_size_index * region_pages;

			pr_iov->block_pages = xmalloc(n_blocks * sizeof(uint16_t));
			if (!pr_iov->block_pages) {
				xfree(pr_iov->compressed_size);
				xfree(iov);
				xfree(pr_iov);
				return -1;
			}
			for (i = 0; i < n_blocks; i++) {
				uint64_t pages_so_far = pages_consumed_at_start +
							i * region_pages;
				uint64_t pages_left =
					pr->pe->nr_pages - pages_so_far;

				pr_iov->block_pages[i] =
					pages_left < region_pages ?
						(uint16_t)pages_left :
						(uint16_t)region_pages;
			}
		}
		pr_iov->end = pr_iov->from + pr_iov->total_compressed_size;
	}

	list_add_tail(&pr_iov->l, to);

	return 0;
}

int pagemap_render_iovec(struct list_head *from, struct task_restore_args *ta)
{
	struct page_read_iov *piov;

	/*
	 * Pass 1: copy each entry's compressed metadata (compressed_size[]
	 * and, for region mode, block_pages[]) into restorer memory and
	 * remember their RM_PRIVATE offsets.
	 *
	 * These must be allocated before the contiguous rio array (pass 2)
	 * for two reasons: the restorer strides over the rio array with
	 * RIO_SIZE(), so nothing may be interleaved between rios; and
	 * rst_mem_alloc() can move the arena, so a rio pointer must never be
	 * held across a subsequent allocation. Offsets are stored (not
	 * pointers) because struct restore_vma_io references them through
	 * RST_MEM_FIXUP_PPTR, which adds the remapped base to the value.
	 */
	list_for_each_entry(piov, from, l) {
		size_t sz;
		void *p;

		piov->rio_cs_off = 0;
		piov->rio_bp_off = 0;

		if (!piov->n_compressed_size)
			continue;

		sz = piov->n_compressed_size * sizeof(uint32_t);
		piov->rio_cs_off = rst_mem_align_cpos(RM_PRIVATE);
		p = rst_mem_alloc(sz, RM_PRIVATE);
		if (!p)
			return -1;
		memcpy(p, piov->compressed_size, sz);

		if (piov->region_pages) {
			sz = piov->n_compressed_size * sizeof(uint16_t);
			piov->rio_bp_off = rst_mem_align_cpos(RM_PRIVATE);
			p = rst_mem_alloc(sz, RM_PRIVATE);
			if (!p)
				return -1;
			memcpy(p, piov->block_pages, sz);
		}
	}

	/* Pass 2: build the contiguous rio array. */
	ta->vma_ios = (struct restore_vma_io *)rst_mem_align_cpos(RM_PRIVATE);
	ta->vma_ios_n = 0;

	list_for_each_entry(piov, from, l) {
		struct restore_vma_io *rio;

		pr_info("`- render %d iovs (%p:%zd...)\n", piov->nr, piov->to[0].iov_base, piov->to[0].iov_len);
		rio = rst_mem_alloc(RIO_SIZE(piov->nr), RM_PRIVATE);
		if (!rio)
			return -1;

		rio->nr_iovs = piov->nr;
		rio->off = piov->from;

		if (!piov->n_compressed_size) {
			rio->compressed_size = NULL;
			rio->n_compressed_size = 0;
			rio->total_compressed_size = 0;
			rio->region_pages = 0;
			rio->n_pages = 0;
			rio->block_pages = NULL;
		} else {
			rio->compressed_size = (uint32_t *)piov->rio_cs_off;
			rio->n_compressed_size = piov->n_compressed_size;
			rio->total_compressed_size = piov->total_compressed_size;
			rio->region_pages = piov->region_pages;

			if (piov->region_pages) {
				uint64_t pages_total = 0;
				size_t i;

				rio->block_pages = (uint16_t *)piov->rio_bp_off;
				for (i = 0; i < piov->n_compressed_size; i++)
					pages_total += piov->block_pages[i];
				rio->n_pages = (int)pages_total;
			} else {
				rio->block_pages = NULL;
				rio->n_pages = piov->n_compressed_size;
			}
		}

		memcpy(rio->iovs, piov->to, piov->nr * sizeof(struct iovec));

		ta->vma_ios_n++;
	}

	return 0;
}

int pagemap_enqueue_iovec(struct page_read *pr, void *buf, unsigned long len, struct list_head *to)
{
	struct page_read_iov *cur_async = NULL;
	struct iovec *iov;
	unsigned int new_region_pages = (pr->pe && pr->pe->has_region_pages &&
					 pr->pe->region_pages) ?
						pr->pe->region_pages : 0;

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
	 * Don't merge piovs with different region modes: each piov is
	 * decompressed with a single algorithm.
	 */
	if (cur_async->region_pages != new_region_pages)
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

		iov += cur_async->nr;
		iov->iov_base = buf;
		iov->iov_len = len;

		cur_async->nr = n_iovs;
	}

	/* Extend the end offset. For compressed entries, append
	 * per-block sizes and advance by compressed bytes. */
	if (!pr->pe || !pr->pe->n_compressed_size) {
		cur_async->end += len;
	} else {
		unsigned long nr_pages = len / PAGE_SIZE;
		unsigned long n_blocks;
		unsigned long i;
		size_t new_n;
		uint32_t *new_cs;
		uint64_t added = 0;

		if (new_region_pages) {
			uint64_t pages_consumed =
				(uint64_t)pr->compressed_size_index * new_region_pages;
			uint64_t end_page = pages_consumed + nr_pages;

			if (pages_consumed % new_region_pages != 0 ||
			    (nr_pages % new_region_pages != 0 &&
			     end_page != pr->pe->nr_pages)) {
				pr_err("Region-mode async append not aligned: idx=%zu nr=%lu region=%u nr_pages=%" PRIu64 "\n",
				       pr->compressed_size_index, nr_pages,
				       new_region_pages, pr->pe->nr_pages);
				return -1;
			}
			n_blocks = (nr_pages + new_region_pages - 1) / new_region_pages;
		} else {
			n_blocks = nr_pages;
		}
		new_n = cur_async->n_compressed_size + n_blocks;

		if (pr->compressed_size_index + n_blocks > pr->pe->n_compressed_size) {
			pr_err("Compressed size index out of bounds: %zu + %lu > %zu\n",
			       pr->compressed_size_index, n_blocks,
			       pr->pe->n_compressed_size);
			return -1;
		}

		new_cs = xrealloc(cur_async->compressed_size, new_n * sizeof(uint32_t));
		if (!new_cs)
			return -1;
		cur_async->compressed_size = new_cs;

		for (i = 0; i < n_blocks; i++) {
			uint32_t cs = pr->pe->compressed_size[pr->compressed_size_index + i];

			cur_async->compressed_size[cur_async->n_compressed_size + i] = cs;
			added += cs;
		}

		if (new_region_pages) {
			uint64_t pages_consumed_at_start =
				(uint64_t)pr->compressed_size_index *
				new_region_pages;
			uint16_t *new_bp = xrealloc(cur_async->block_pages,
						    new_n * sizeof(uint16_t));

			if (!new_bp)
				return -1;
			cur_async->block_pages = new_bp;

			for (i = 0; i < n_blocks; i++) {
				uint64_t pages_so_far = pages_consumed_at_start +
							i * new_region_pages;
				uint64_t pages_left =
					pr->pe->nr_pages - pages_so_far;

				cur_async->block_pages[cur_async->n_compressed_size + i] =
					pages_left < new_region_pages ?
						(uint16_t)pages_left :
						(uint16_t)new_region_pages;
			}
		}

		cur_async->total_compressed_size += added;
		cur_async->n_compressed_size = new_n;
		cur_async->end += added;
	}

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
	if ((flags & (PR_ASYNC | PR_ASAP)) == PR_ASYNC)
		ret = pagemap_enqueue_iovec(pr, buf, len, &pr->async);
	else {
		ret = read_local_page(pr, vaddr, len, buf);
		if (ret == 0 && pr->io_complete)
			ret = pr->io_complete(pr, vaddr, nr);
	}

	pr->pi_off += len;

	return ret;
}

static int pread_full(int fd, void *buf, size_t count, off_t offset)
{
	size_t rd = 0;

	while (rd < count) {
		ssize_t ret = pread(fd, (char *)buf + rd, count - rd,
				    offset + rd);
		if (ret <= 0) {
			pr_perror("Short pread %zd/%zu at offset %lld",
				  ret, count, (long long)(offset + rd));
			return -1;
		}
		rd += ret;
	}
	return 0;
}

/*
 * Advance compressed offset tracking by @nr pages without
 * reading any data. Used when enqueuing async compressed reads.
 *
 * Caller has already verified that @nr aligns to region boundaries
 * (start at block 0 offset, end on block boundary or entry end) and
 * that pr->region_block_offset is 0 in region mode.
 */
static void skip_compressed_offsets(struct page_read *pr, unsigned long nr)
{
	unsigned long i;
	unsigned int region_pages = (pr->pe && pr->pe->has_region_pages) ?
					    pr->pe->region_pages : 0;
	unsigned long n_blocks;

	if (region_pages) {
		n_blocks = (nr + region_pages - 1) / region_pages;
	} else {
		n_blocks = nr;
	}

	for (i = 0; i < n_blocks; i++)
		pr->pi_off += pr->pe->compressed_size[pr->compressed_size_index + i];
	pr->compressed_size_index += n_blocks;
}

/*
 * Synchronous compressed page read: decompress pages one-by-one
 * from the image file directly into @buf.
 */
static int read_compressed_pages(struct page_read *pr, int fd,
				 unsigned long nr, void *buf)
{
	size_t curr = 0;
	off_t compressed_offset = 0;
	char compressed_buf[PAGE_COMPRESSED_SIZE_BOUND];

	for (int i = 0; i < nr; i++) {
		size_t idx = pr->compressed_size_index + i;
		uint32_t cs = pr->pe->compressed_size[idx];

		if (cs > PAGE_COMPRESSED_SIZE_BOUND) {
			pr_err("Invalid compressed size %u for page %zu\n",
			       cs, idx);
			return -1;
		}

		if (cs == 0) {
			/* Zero page, nothing stored in the image */
			memset(buf + curr, 0, PAGE_SIZE);
		} else if (cs == PAGE_SIZE) {
			/* Stored raw, no decompression needed */
			if (pread_full(fd, buf + curr, PAGE_SIZE,
				       pr->pi_off + compressed_offset))
				return -1;
		} else {
			if (pread_full(fd, compressed_buf, cs,
				       pr->pi_off + compressed_offset))
				return -1;

			if (decompress_data(compressed_buf, cs, PAGE_SIZE,
					    buf + curr)) {
				pr_perror("Decompression failed for i=%zu compressed_size=%u, curr: %zu pi_off: %lld",
					idx, cs, curr, (long long)(pr->pi_off + compressed_offset));
				return -1;
			}
		}

		curr += PAGE_SIZE;
		compressed_offset += cs;
	}

	pr->pi_off += compressed_offset;
	pr->compressed_size_index += nr;

	return 0;
}

/*
 * Region-mode synchronous compressed read. Each compressed_size[]
 * entry covers up to region_pages pages. Reads may start mid-region
 * (when pr->region_block_offset > 0) and may end mid-region.
 *
 * For each block touched, read its compressed bytes, decompress into
 * a heap scratch buffer once, then memcpy the requested page slice
 * out. Crossing a block boundary advances pr->compressed_size_index
 * and pr->pi_off; staying within a block bumps region_block_offset.
 */
static int read_compressed_pages_region(struct page_read *pr, int fd,
					unsigned long nr, void *buf)
{
	unsigned int region_pages = pr->pe->region_pages;
	size_t region_bytes_max = (size_t)region_pages * PAGE_SIZE;
	size_t bound = REGION_COMPRESSED_SIZE_BOUND(region_pages);
	unsigned long pages_done = 0;
	int rc = -1;
	char *scratch;

	scratch = xmalloc(region_bytes_max);
	if (!scratch)
		return -1;

	while (pages_done < nr) {
		size_t idx = pr->compressed_size_index;
		uint32_t cs;
		unsigned int this_region;
		size_t this_bytes;
		unsigned int off = pr->region_block_offset;
		unsigned int avail;
		unsigned int take;
		size_t out_bytes;

		if (idx >= pr->pe->n_compressed_size) {
			pr_err("region read: index %zu >= n_compressed %zu\n",
			       idx, pr->pe->n_compressed_size);
			goto out;
		}

		cs = pr->pe->compressed_size[idx];
		this_region = current_block_pages(pr);
		this_bytes = (size_t)this_region * PAGE_SIZE;

		if (cs > bound) {
			pr_err("Invalid region compressed size %u (region=%u idx=%zu)\n",
			       cs, this_region, idx);
			goto out;
		}

		avail = this_region - off;
		take = nr - pages_done < avail ? (unsigned int)(nr - pages_done) :
						 avail;
		out_bytes = (size_t)take * PAGE_SIZE;

		if (cs == 0) {
			memset((char *)buf + pages_done * PAGE_SIZE, 0, out_bytes);
		} else if ((size_t)cs == this_bytes) {
			/* Stored raw: pread directly the requested slice. */
			if (pread_full(fd,
				       (char *)buf + pages_done * PAGE_SIZE,
				       out_bytes,
				       pr->pi_off + (off_t)off * PAGE_SIZE))
				goto out;
		} else if (off == 0 && take == this_region) {
			/* Whole region in one go: decompress straight into the dest. */
			if (pread_full(fd, scratch, cs, pr->pi_off))
				goto out;
			if (decompress_region(scratch, cs, this_region,
					      (char *)buf + pages_done * PAGE_SIZE)) {
				pr_err("Region decompression failed (idx=%zu cs=%u region=%u)\n",
				       idx, cs, this_region);
				goto out;
			}
		} else {
			/* Partial slice: decompress into scratch, then memcpy. */
			char *region_dec = xmalloc(this_bytes);

			if (!region_dec)
				goto out;
			if (pread_full(fd, scratch, cs, pr->pi_off)) {
				xfree(region_dec);
				goto out;
			}
			if (decompress_region(scratch, cs, this_region, region_dec)) {
				pr_err("Region decompression failed (idx=%zu cs=%u region=%u)\n",
				       idx, cs, this_region);
				xfree(region_dec);
				goto out;
			}
			memcpy((char *)buf + pages_done * PAGE_SIZE,
			       region_dec + (size_t)off * PAGE_SIZE,
			       out_bytes);
			xfree(region_dec);
		}

		pages_done += take;
		pr->region_block_offset = off + take;

		if (pr->region_block_offset == this_region) {
			pr->pi_off += cs;
			pr->compressed_size_index++;
			pr->region_block_offset = 0;
		}
	}

	rc = 0;
out:
	xfree(scratch);
	return rc;
}

static int maybe_read_page_local_compressed(struct page_read *pr, unsigned long vaddr, unsigned long nr, void *buf, unsigned flags)
{
	int fd, ret;
	unsigned long len = nr * PAGE_SIZE;
	unsigned int region_pages = (pr->pe->has_region_pages &&
				     pr->pe->region_pages) ?
					    pr->pe->region_pages : 0;

	/*
	 * If this pagemap entry has no compressed_size array, it
	 * was stored uncompressed (e.g. shared memory pagemaps or
	 * entries from a non-compressed parent). Fall back to the
	 * normal uncompressed reader.
	 */
	if (!pr->pe->n_compressed_size)
		return maybe_read_page_local(pr, vaddr, nr, buf, flags);

	if (!region_pages &&
	    pr->compressed_size_index + nr > pr->pe->n_compressed_size) {
		pr_err("Compressed size index out of bounds: %zu + %lu > %zu\n",
		       pr->compressed_size_index, nr,
		       pr->pe->n_compressed_size);
		return -1;
	}

	/*
	 * For PR_ASYNC (without PR_ASAP), enqueue the request.
	 * The compressed_size metadata is captured by
	 * pagemap_enqueue_iovec(); actual decompression happens
	 * later in process_async_reads(). This batches many
	 * small reads into fewer large pread() calls.
	 *
	 * Region mode async requires the request to align to region
	 * boundaries (start at offset 0 in a block; finish on a block
	 * boundary or at the entry's end). Mid-region async requests
	 * fall through to the sync path.
	 */
	if ((flags & (PR_ASYNC | PR_ASAP)) == PR_ASYNC) {
		bool can_async = true;

		if (region_pages) {
			uint64_t pages_consumed =
				(uint64_t)pr->compressed_size_index *
				region_pages;
			uint64_t end_page = pages_consumed + nr;

			can_async = (pr->region_block_offset == 0) &&
				    (nr % region_pages == 0 ||
				     end_page == pr->pe->nr_pages);
		}

		if (can_async) {
			ret = pagemap_enqueue_iovec(pr, buf, len, &pr->async);
			skip_compressed_offsets(pr, nr);
			return ret;
		}
		/* Mid-region or unaligned: fall back to sync. */
	}

	fd = img_raw_fd(pr->pi);
	if (fd < 0) {
		pr_err("Failed getting raw image fd\n");
		return -1;
	}

	/*
	 * Flush any pending async requests not to break the
	 * linear reading from the pages.img file.
	 */
	if (pr->sync(pr))
		return -1;

	pr_debug("\tpr%lu-%u Read page from self %lx/%" PRIx64 "\n",
		 pr->img_id, pr->id, pr->cvaddr, pr->pi_off);

	if (region_pages)
		ret = read_compressed_pages_region(pr, fd, nr, buf);
	else
		ret = read_compressed_pages(pr, fd, nr, buf);
	if (ret)
		return ret;

	if (pr->io_complete)
		ret = pr->io_complete(pr, vaddr, nr);

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

/*
 * Streaming + compressed reader: reads compressed data sequentially
 * from the pipe (no seeking), decompresses page-by-page.
 */
static int maybe_read_page_img_streamer_compressed(struct page_read *pr, unsigned long vaddr, unsigned long nr, void *buf, unsigned flags)
{
	int fd;
	ssize_t ret = 0;
	size_t curr = 0;
	off_t compressed_offset = 0;
	char compressed_buf[PAGE_COMPRESSED_SIZE_BOUND];

	/*
	 * Fall back to uncompressed streamer for entries without compressed data.
	 * Region mode is not supported in streaming restore (checked in config).
	 */
	if (!pr->pe->n_compressed_size)
		return maybe_read_page_img_streamer(pr, vaddr, nr, buf, flags);

	if (pr->pe->has_region_pages && pr->pe->region_pages) {
		pr_err("Streaming restore does not support region-mode compression\n");
		return -1;
	}

	fd = img_raw_fd(pr->pi);
	if (fd < 0) {
		pr_err("Getting raw FD failed\n");
		return -1;
	}

	pr_debug("\tpr%lu-%u Read page from self %lx/%" PRIx64 "\n",
		 pr->img_id, pr->id, pr->cvaddr, pr->pi_off);

	BUG_ON(pr->cvaddr != vaddr);

	if (pr->compressed_size_index + nr > pr->pe->n_compressed_size) {
		pr_err("Compressed size index out of bounds: %zu + %lu > %zu\n",
		       pr->compressed_size_index, nr,
		       pr->pe->n_compressed_size);
		return -1;
	}

	for (int i = 0; i < nr; i++) {
		size_t idx = pr->compressed_size_index + i;
		uint32_t cs = pr->pe->compressed_size[idx];
		size_t rd = 0;

		if (cs > PAGE_COMPRESSED_SIZE_BOUND) {
			pr_err("Invalid compressed size %u for page %zu\n",
			       cs, idx);
			return -1;
		}

		if (cs == 0) {
			/* Zero page, nothing stored in the image */
			memset(buf + curr, 0, PAGE_SIZE);
			curr += PAGE_SIZE;
			continue;
		}

		if (cs == PAGE_SIZE) {
			/* Stored raw, read directly into output */
			while (rd < PAGE_SIZE) {
				ret = read(fd, buf + curr + rd, PAGE_SIZE - rd);
				if (ret == 0) {
					pr_err("Reached EOF reading raw page\n");
					return -1;
				} else if (ret < 0) {
					pr_perror("Can't read raw page");
					return -1;
				}
				rd += ret;
			}
		} else {
			while (rd < cs) {
				/*
				 * cs is bounded by PAGE_COMPRESSED_SIZE_BOUND
				 * (validated above), so cs - rd fits in the
				 * stack buffer. Re-state the bound explicitly
				 * so the compiler's fortified-read analysis
				 * does not warn about a possible overflow into
				 * compressed_buf.
				 */
				size_t to_read = cs - rd;

				if (to_read > sizeof(compressed_buf) - rd)
					to_read = sizeof(compressed_buf) - rd;

				ret = read(fd, compressed_buf + rd, to_read);
				if (ret == 0) {
					pr_err("Reached EOF reading compressed page\n");
					return -1;
				} else if (ret < 0) {
					pr_perror("Can't read compressed page");
					return -1;
				}
				rd += ret;
			}

			if (decompress_data(compressed_buf, cs, PAGE_SIZE, buf + curr)) {
				pr_perror("Decompression failed for page %zu", idx);
				return -1;
			}
		}

		curr += PAGE_SIZE;
		compressed_offset += cs;
	}

	if (pr->io_complete)
		ret = pr->io_complete(pr, vaddr, nr);

	pr->pi_off += compressed_offset;
	pr->compressed_size_index += nr;

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
		xfree(piov->compressed_size);
		xfree(piov->block_pages);
		xfree(piov->to);
		xfree(piov);
	}
	if (pr->parent)
		drain_async_queue(pr->parent);
}

static int process_async_reads(struct page_read *pr)
{
	int fd, ret = 0;
	struct page_read_iov *piov, *n;
	off_t first_off = OFF_MAX, last_end = OFF_MIN;

	fd = img_raw_fd(pr->pi);
	if (!pr->use_direct) {
		list_for_each_entry(piov, &pr->async, l) {
			first_off = min(piov->from, first_off);
			last_end = max(piov->end, last_end);
		}
		if (last_end > first_off) {
			if (posix_fadvise(fd, first_off, (off_t)(last_end - first_off), POSIX_FADV_WILLNEED) != 0)
				pr_debug("posix_fadvise(WILLNEED) failed for async range\n");
		}
	}

	list_for_each_entry_safe(piov, n, &pr->async, l) {
		ssize_t ret;
		struct iovec *iovs = piov->to;

		pr_debug("Read piov iovs %d, from %ju, len %ju, first %p:%zu\n", piov->nr, piov->from,
			 piov->end - piov->from, piov->to->iov_base, piov->to->iov_len);

		if (piov->n_compressed_size) {
			/*
			 * Compressed async read: read all compressed
			 * data in one pread(), then decompress
			 * block-by-block into the destination iovecs.
			 * In per-page mode each block is one page; in
			 * region mode each block is up to region_pages
			 * pages.
			 */
			char *comp_buf;
			off_t comp_off;
			size_t total = piov->total_compressed_size;
			int iov_idx = 0;
			size_t iov_off = 0;
			char *region_scratch = NULL;
			size_t region_scratch_cap = 0;

			/*
			 * An all-zero batch has total == 0: there is nothing
			 * to read from the image and every block is rebuilt by
			 * memset below, so leave comp_buf NULL. xmalloc(0) may
			 * return NULL, which must not be treated as an error.
			 * Route a genuine allocation failure through the err
			 * path so the async queue is drained.
			 */
			comp_buf = NULL;
			if (total) {
				comp_buf = xmalloc(total);
				if (!comp_buf) {
					ret = -1;
					goto err;
				}

				if (pread_full(fd, comp_buf, total, piov->from)) {
					xfree(comp_buf);
					ret = -1;
					goto err;
				}
			}

			if (piov->region_pages) {
				region_scratch_cap =
					(size_t)piov->region_pages * PAGE_SIZE;
				region_scratch = xmalloc(region_scratch_cap);
				if (!region_scratch) {
					xfree(comp_buf);
					ret = -1;
					goto err;
				}
			}

			comp_off = 0;
			for (int i = 0; i < piov->n_compressed_size; i++) {
				uint32_t cs = piov->compressed_size[i];
				unsigned int block_pages;
				size_t block_bytes;
				size_t bound;
				size_t out_left;
				char *src;

				if (piov->region_pages) {
					block_pages = piov->block_pages[i];
					bound = REGION_COMPRESSED_SIZE_BOUND(block_pages);
				} else {
					block_pages = 1;
					bound = PAGE_COMPRESSED_SIZE_BOUND;
				}
				block_bytes = (size_t)block_pages * PAGE_SIZE;

				if (cs > bound) {
					pr_err("Async: invalid compressed size %u for block %d\n",
					       cs, i);
					xfree(comp_buf);
					xfree(region_scratch);
					ret = -1;
					goto err;
				}

				/*
				 * Decompress this block to a contiguous
				 * source buffer.
				 *
				 * Region mode raw blocks (cs == block_bytes)
				 * skip the staging memcpy and copy directly
				 * out of comp_buf. Zero blocks (cs == 0) are
				 * handled by memset on the destination.
				 *
				 * Compressed blocks normally decompress into
				 * region_scratch and we then memcpy into the
				 * iovecs. As an optimisation, when the whole
				 * block fits inside a single destination iovec
				 * (the common case after a contiguous VMA), we
				 * decompress directly into the iovec and skip
				 * the staging buffer.
				 */
				if (piov->region_pages) {
					if (cs == 0) {
						src = NULL; /* memset on dst */
					} else if (cs == block_bytes) {
						src = comp_buf + comp_off;
					} else {
						/* Locate destination iovec for this block */
						int peek_iov = iov_idx;
						size_t peek_off = iov_off;

						while (peek_iov < piov->nr &&
						       peek_off >= piov->to[peek_iov].iov_len) {
							peek_off -= piov->to[peek_iov].iov_len;
							peek_iov++;
						}

						if (peek_iov < piov->nr &&
						    peek_off + block_bytes <=
						        piov->to[peek_iov].iov_len) {
							/* Block fits — decompress directly into iovec */
							char *direct = (char *)piov->to[peek_iov].iov_base + peek_off;

							if (decompress_region(comp_buf + comp_off,
									      cs, block_pages,
									      direct)) {
								pr_err("Async region decompress failed at block %d\n",
								       i);
								xfree(comp_buf);
								xfree(region_scratch);
								ret = -1;
								goto err;
							}
							/* Advance iovec cursor and skip the copy loop. */
							iov_off += block_bytes;
							comp_off += cs;
							continue;
						}

						if (decompress_region(comp_buf + comp_off,
								      cs, block_pages,
								      region_scratch)) {
							pr_err("Async region decompress failed at block %d\n",
							       i);
							xfree(comp_buf);
							xfree(region_scratch);
							ret = -1;
							goto err;
						}
						src = region_scratch;
					}
				} else {
					src = NULL; /* handled below */
				}

				/*
				 * Walk the destination iovec array,
				 * copying block_bytes worth of decompressed
				 * data, splitting across iovecs as needed.
				 */
				out_left = block_bytes;
				while (out_left > 0) {
					char *dst;
					size_t this_chunk;

					while (iov_idx < piov->nr &&
					       iov_off >= piov->to[iov_idx].iov_len) {
						iov_off -= piov->to[iov_idx].iov_len;
						iov_idx++;
					}
					if (iov_idx >= piov->nr) {
						pr_err("Async: ran out of iovs at block %d\n", i);
						xfree(comp_buf);
						xfree(region_scratch);
						ret = -1;
						goto err;
					}

					dst = (char *)piov->to[iov_idx].iov_base + iov_off;
					this_chunk = piov->to[iov_idx].iov_len - iov_off;
					if (this_chunk > out_left)
						this_chunk = out_left;

					if (piov->region_pages) {
						if (src) {
							memcpy(dst, src, this_chunk);
							src += this_chunk;
						} else {
							/* zero block */
							memset(dst, 0, this_chunk);
						}
					} else {
						/*
						 * Per-page block fits in one
						 * iovec slice (this_chunk
						 * == PAGE_SIZE) since iovecs
						 * are page-aligned.
						 */
						if (cs == 0)
							memset(dst, 0, this_chunk);
						else if (cs == PAGE_SIZE)
							memcpy(dst, comp_buf + comp_off, this_chunk);
						else if (decompress_data(comp_buf + comp_off,
									 cs, PAGE_SIZE, dst)) {
							pr_err("Async decompress failed for page %d\n", i);
							xfree(comp_buf);
							xfree(region_scratch);
							ret = -1;
							goto err;
						}
					}

					iov_off += this_chunk;
					out_left -= this_chunk;
				}

				comp_off += cs;
			}

			xfree(comp_buf);
			xfree(region_scratch);
			goto next;
		}

	more:
		ret = preadv(fd, piov->to, piov->nr, piov->from);
		if (fault_injected(FI_PARTIAL_PAGES)) {
			/*
			 * We might have read everything, but for debug
			 * purposes let's try to force the advance_piov()
			 * and re-read tail.
			 */
			if (ret >= 2 * PAGE_SIZE) {
				pr_debug("`- trim preadv %zu\n", ret);
				ret /= 2;
				ret &= PAGE_MASK;
			}
		}

		if (ret < 0) {
			pr_err("Can't read async pr bytes (%zd / %ju read, %ju off, %d iovs)\n", ret,
			       piov->end - piov->from, piov->from, piov->nr);
			goto err;
		}

		if (ret == 0 && piov->end != piov->from) {
			pr_err("Unexpected EOF reading pages: expected %ju more bytes at offset %ju\n",
			       piov->end - piov->from, piov->from);
			goto err;
		}

		if (opts.auto_dedup && punch_hole(pr, piov->from, ret, false))
			goto err;

		if (ret != piov->end - piov->from) {
			/*
			 * The preadv() can return less than requested. It's
			 * valid and doesn't mean error or EOF. We should advance
			 * the iovecs and continue
			 *
			 * Modify the piov in-place, we're going to drop this one
			 * anyway.
			 */

			advance_piov(piov, ret);
			goto more;
		}

	next:
		BUG_ON(pr->io_complete); /* FIXME -- implement once needed */
		list_del(&piov->l);
		xfree(piov->compressed_size);
		xfree(piov->block_pages);
		xfree(iovs);
		xfree(piov);
	}

	if (pr->parent)
		ret = process_async_reads(pr->parent);

	return ret;
err:
	drain_async_queue(pr);
	return -1;
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

	if (pr->pmes)
		free_pagemaps(pr);
}

static void reset_pagemap(struct page_read *pr)
{
	pr->cvaddr = 0;
	pr->pi_off = 0;
	pr->compressed_size_index = 0;
	pr->region_block_offset = 0;
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

int probe_pages_o_direct(int fd)
{
	int fl, ret, memerr;
	void *probe = NULL;
	ssize_t probe_ret;

	fl = fcntl(fd, F_GETFL);
	if (fl < 0)
		return 0;

	ret = fcntl(fd, F_SETFL, fl | O_DIRECT);
	if (ret < 0) {
		pr_warn("Failed to set O_DIRECT on pages fd %d: %s\n", fd, strerror(errno));
		return 0;
	}

	/*
	 * PAGE_SIZE is not a compile-time constant on aarch64, so the
	 * probe buffer is allocated via posix_memalign() instead of a
	 * stack array with __attribute__((aligned)).
	 */
	memerr = posix_memalign(&probe, PAGE_SIZE, PAGE_SIZE);
	if (memerr) {
		pr_err("O_DIRECT probe alloc failed on pages fd %d: %s\n", fd, strerror(memerr));
		return -1;
	}

	probe_ret = pread(fd, probe, PAGE_SIZE, 0);
	xfree(probe);

	if (probe_ret >= 0) {
		pr_debug("O_DIRECT enabled on pages fd %d\n", fd);
		return 1;
	}

	if (errno != EINVAL) {
		pr_perror("O_DIRECT probe failed on pages fd %d", fd);
		return -1;
	}

	pr_info("O_DIRECT rejected at read time on pages fd %d, using buffered I/O\n", fd);
	if (fcntl(fd, F_SETFL, fl) < 0) {
		pr_perror("Failed to clear O_DIRECT on pages fd %d", fd);
		return -1;
	}

	/*
	 * Hint the kernel that fallback buffered reads will mostly
	 * advance through the pages file in offset order.
	 */
	posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
	return 0;
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
	pr->compressed_size_index = 0;
	pr->region_block_offset = 0;
	pr->bunch.iov_len = 0;
	pr->bunch.iov_base = NULL;
	pr->pmes = NULL;
	pr->pieok = false;
	pr->disable_dedup = false;
	pr->use_direct = false;

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

	pr->pi = open_pages_image_at(dfd, flags, pr->pmi, &pr->pages_img_id);
	if (!pr->pi) {
		close_page_read(pr);
		return -1;
	}

	{
		int pfd = img_raw_fd(pr->pi);

		/*
		 * O_DIRECT requires reads to be aligned in offset and
		 * length. Compressed pages are stored as variable-length
		 * blocks packed contiguously, so reads are unaligned and
		 * O_DIRECT would fail with EINVAL. Use buffered I/O for
		 * compressed images.
		 */
		if (pfd >= 0 && !opts.stream && !opts.compress_mode) {
			int direct = probe_pages_o_direct(pfd);

			if (direct < 0) {
				close_page_read(pr);
				return -1;
			}
			pr->use_direct = (direct == 1);
		}
	}

	/*
	 * Hint the kernel to use aggressive readahead on the pages
	 * image. pread() does not advance the file offset, so the
	 * kernel's sequential-access heuristic may not trigger
	 * without this hint.
	 */
	if (img_raw_fd(pr->pi) >= 0)
		posix_fadvise(img_raw_fd(pr->pi), 0, 0, POSIX_FADV_SEQUENTIAL);

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
	pr->id = ids++;
	pr->img_id = img_id;

	if (remote)
		pr->maybe_read_page = maybe_read_page_remote;
	else if (opts.stream && opts.compress_mode)
		pr->maybe_read_page = maybe_read_page_img_streamer_compressed;
	else if (opts.stream)
		pr->maybe_read_page = maybe_read_page_img_streamer;
	else if (opts.compress_mode)
		pr->maybe_read_page = maybe_read_page_local_compressed;
	else {
		pr->maybe_read_page = maybe_read_page_local;
		if (!pr->parent && !opts.lazy_pages)
			pr->pieok = true;
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
	dst->reset(dst);
}
