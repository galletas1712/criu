#ifndef __CR_COMPRESSION_H__
#define __CR_COMPRESSION_H__

#include "page.h"

/*
 * Compression mode for memory pages. Stored in opts.compress_mode and
 * encoded in inventory_entry.compress and criu_opts.compress on the wire.
 *
 * Single source of truth: COMPRESS_OFF (=0) means no compression. There
 * is no separate "compression enabled" boolean; sites that want a
 * predicate use `if (opts.compress_mode)`.
 */
enum compress_mode {
	COMPRESS_OFF		= 0,
	COMPRESS_PER_PAGE	= 1,
	COMPRESS_REGION		= 2,
};

/*
 * Region mode: maximum and default region size in pages. The cap of
 * 1024 pages keeps the daemon's per-region scratch buffer at most 4 MiB
 * and bounds the worst-case compressed-size buffer at <~4.1 MiB.
 */
#define MAX_REGION_PAGES	1024
#define DEFAULT_REGION_PAGES	64 /* 256 KiB */

/* LZ4 worst-case compressed size for one page: src + src/255 + 16 */
#define PAGE_COMPRESSED_SIZE_BOUND (PAGE_SIZE + (PAGE_SIZE / 255) + 16)

/*
 * LZ4 worst-case compressed size for a region of n_pages pages.
 * Same formula as PAGE_COMPRESSED_SIZE_BOUND but with n_pages*PAGE_SIZE
 * as the input size.
 */
#define REGION_COMPRESSED_SIZE_BOUND(n_pages) \
	((size_t)(n_pages) * PAGE_SIZE + ((size_t)(n_pages) * PAGE_SIZE / 255) + 16)

/*
 * Compression threshold: store raw if compressed size is above this.
 * Pages that only compress by a small amount are not worth the
 * decompression cost on restore.
 */
#define PAGE_COMPRESSION_THRESHOLD (PAGE_SIZE * 7 / 8)
#define REGION_COMPRESSION_THRESHOLD(region_bytes) ((region_bytes) * 7 / 8)

/*
 * Default LZ4 acceleration level for LZ4_compress_fast().
 * Acceleration controls how many positions the compressor
 * probes in its hash table when searching for matches.
 * Value 1 gives the best ratio (~780 MB/s compression).
 * Higher values skip more match candidates, resulting in
 * faster compression but fewer and shorter matches.
 * Decompression speed is not affected (~4970 MB/s always).
 * Valid range: 1 to LZ4_MAX_ACCELERATION.
 */
#define LZ4_MAX_ACCELERATION	65537
#define LZ4_DEFAULT_ACCELERATION 1

/*
 * Detect zero-filled pages. Uses unsigned long comparison for speed
 * (8 bytes per iteration on 64-bit), mirroring the kernel's
 * is_folio_zero_filled() approach (see mm/page_io.c).
 */
static inline bool page_is_all_zero(const char *page)
{
	const unsigned long *p = (const unsigned long *)page;
	unsigned int last = PAGE_SIZE / sizeof(unsigned long) - 1;
	unsigned int i;

	/*
	 * Check last word first: pages are often zero at the start
	 * but have non-zero data near the end (e.g. stack, heap).
	 * See kernel commit 0ca0c24e3211 ("mm: zswap: check the last
	 * page in a folio first").
	 */
	if (p[last])
		return false;

	for (i = 0; i < last; i++) {
		if (p[i])
			return false;
	}
	return true;
}

#ifdef CONFIG_LZ4

int compress_data(const char *input_data, size_t input_size,
		  char *compressed_data, size_t output_size,
		  int acceleration);
int decompress_data(const char *compressed_data, int compressed_size,
		    int original_size, char *decompressed_data);

/*
 * Compress @n_pages consecutive pages from @src as a single LZ4 block
 * into @dst (capacity @dst_cap). Returns one of:
 *   0                   -> the region is entirely zero-filled (caller
 *                          stores no payload).
 *   n_pages * PAGE_SIZE -> store-raw fallback (no useful compression);
 *                          @dst contains a verbatim copy of @src.
 *   N (in between)      -> LZ4 block of N bytes is in @dst.
 * Returns -1 on internal error.
 */
int compress_region(const char *src, unsigned int n_pages, char *dst,
		    size_t dst_cap, int acceleration);

/*
 * Inverse of compress_region(). @compressed_size is the value the
 * caller stored at compression time. Always writes n_pages*PAGE_SIZE
 * bytes into @dst.
 */
int decompress_region(const char *src, int compressed_size,
		      unsigned int n_pages, char *dst);

/*
 * Start a helper daemon that decompresses memory pages on behalf of
 * the PIE restorer. The restorer code runs as position-independent
 * executable injected into the restored process address space, so it
 * cannot link against external libraries such as LZ4. The daemon
 * runs as a regular process, reads compressed pages from the image
 * file, decompresses them, and writes the result into the target
 * address space via process_vm_writev(). Communication with the
 * restorer happens over a pair of pipes.
 */
int start_vma_io_pipe_daemon(int pages_img_fd, int pipe_fds[2][2]);

#else /* !CONFIG_LZ4 */

static inline int compress_data(const char *in, size_t in_sz, char *out,
				size_t out_sz, int acceleration)
{
	return -1;
}

static inline int decompress_data(const char *in, int in_sz, int out_sz,
				  char *out)
{
	return -1;
}

static inline int compress_region(const char *src, unsigned int n_pages,
				  char *dst, size_t dst_cap, int accel)
{
	return -1;
}

static inline int decompress_region(const char *src, int comp_sz,
				    unsigned int n_pages, char *dst)
{
	return -1;
}

static inline int start_vma_io_pipe_daemon(int fd, int pipes[2][2])
{
	return -1;
}

#endif /* CONFIG_LZ4 */

#endif /* __CR_COMPRESSION_H__ */
