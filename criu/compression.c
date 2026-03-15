#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include <lz4.h>

#include "page.h"
#include "util.h"
#include "log.h"
#include "compression.h"
#include "common/xmalloc.h"

#undef LOG_PREFIX
#define LOG_PREFIX "compression: "

int compress_data(const char *input_data, size_t input_size,
		  char *compressed_data, size_t output_size,
		  int acceleration)
{
	int ret;

	if (acceleration < 1)
		acceleration = 1;

	ret = LZ4_compress_fast(input_data, compressed_data, input_size,
				output_size, acceleration);
	if (ret <= 0) {
		pr_err("Failed to compress data: %d\n", ret);
		return -1;
	}

	return ret;
}

int decompress_data(const char *compressed_data, int compressed_size,
		    int original_size, char *decompressed_data)
{
	int ret;
	ret = LZ4_decompress_safe(compressed_data, decompressed_data,
				  compressed_size, original_size);
	if (ret != original_size) {
		pr_err("Decompression failed: expected %d bytes, got %d\n",
		       original_size, ret);
		return -1;
	}

	return 0;
}

/*
 * Region-mode helpers. A region is a run of @n_pages consecutive pages
 * that share a single LZ4 block. The caller stores the returned size in
 * pagemap_entry.compressed_size[]. Sentinel values:
 *   0                  -> the entire region is zero-filled (no payload).
 *   n_pages*PAGE_SIZE  -> region was incompressible / not worth it; the
 *                         payload is stored raw.
 *   anything else      -> LZ4-compressed payload of exactly that many
 *                         bytes.
 */
int compress_region(const char *src, unsigned int n_pages, char *dst,
		    size_t dst_cap, int acceleration)
{
	size_t region_bytes = (size_t)n_pages * PAGE_SIZE;
	unsigned int i;
	int ret;

	if (n_pages == 0 || n_pages > MAX_REGION_PAGES) {
		pr_err("compress_region: invalid n_pages %u\n", n_pages);
		return -1;
	}

	/* Cheap pre-pass: every page in the region zero-filled? */
	for (i = 0; i < n_pages; i++) {
		if (!page_is_all_zero(src + (size_t)i * PAGE_SIZE))
			break;
	}
	if (i == n_pages)
		return 0;

	if (dst_cap < region_bytes) {
		pr_err("compress_region: dst buffer (%zu) smaller than region (%zu)\n",
		       dst_cap, region_bytes);
		return -1;
	}

	if (acceleration < 1)
		acceleration = 1;

	ret = LZ4_compress_fast(src, dst, region_bytes, dst_cap, acceleration);
	if (ret <= 0 || (size_t)ret >= REGION_COMPRESSION_THRESHOLD(region_bytes)) {
		/*
		 * LZ4 can fail when dst_cap is below LZ4 worst-case bound
		 * (which the caller should size correctly), or when the
		 * compressed size hits the threshold and we'd rather store
		 * raw. Either way, fall back to raw.
		 */
		memcpy(dst, src, region_bytes);
		return region_bytes;
	}

	return ret;
}

int decompress_region(const char *src, int compressed_size,
		      unsigned int n_pages, char *dst)
{
	size_t region_bytes = (size_t)n_pages * PAGE_SIZE;

	if (n_pages == 0 || n_pages > MAX_REGION_PAGES) {
		pr_err("decompress_region: invalid n_pages %u\n", n_pages);
		return -1;
	}

	if (compressed_size == 0) {
		memset(dst, 0, region_bytes);
		return 0;
	}

	if ((size_t)compressed_size == region_bytes) {
		memcpy(dst, src, region_bytes);
		return 0;
	}

	if ((size_t)compressed_size > region_bytes) {
		pr_err("decompress_region: compressed_size %d > region %zu\n",
		       compressed_size, region_bytes);
		return -1;
	}

	return decompress_data(src, compressed_size, region_bytes, dst);
}

/*
 * Wire protocol header between the PIE restorer and the helper daemon.
 * Must match pipe_preadv_limited() in pie/restorer.c.
 *
 * region_pages == 0 indicates per-page compression: each compressed_size[]
 * entry is one page, and n_blocks == n_pages.
 *
 * region_pages > 0 indicates region compression: each compressed_size[]
 * entry covers up to region_pages pages, and a parallel block_pages[]
 * array (uint16_t per block) is sent after compressed_size[] giving the
 * actual page count of each block (the last block of any pagemap entry
 * spanning this request may be shorter than region_pages).
 *
 * In all cases sum(block_pages) == n_pages and sum(compressed_size) ==
 * total_compressed_size.
 */
struct pipe_hdr {
	pid_t remote_pid;
	off_t offs;
	uint64_t total_compressed_size;
	int n_pages;
	int nr_iovs;
	int n_blocks;
	uint32_t region_pages;
} __attribute__((packed));

/*
 * Read exactly @size bytes from @fd, handling short reads.
 */
static int read_full(int fd, void *buf, size_t size)
{
	size_t rd = 0;

	while (rd < size) {
		ssize_t ret = read(fd, (char *)buf + rd, size - rd);

		if (ret < 0) {
			pr_perror("Failed reading from pipe");
			return -1;
		}
		if (ret == 0) {
			pr_err("Unexpected EOF reading from pipe\n");
			return -1;
		}
		rd += ret;
	}
	return 0;
}

static int pread_img_data(int fd, void *buf, size_t count, off_t offset)
{
	ssize_t rd = 0;

	while (rd < count) {
		ssize_t ret = pread(fd, (char *)buf + rd, count - rd, offset + rd);

		if (ret < 0) {
			pr_perror("Failed reading compressed data");
			return -1;
		}
		if (ret == 0) {
			pr_err("Unexpected EOF reading compressed data\n");
			return -1;
		}
		rd += ret;
	}
	return 0;
}

static int write_pipe_result(int fd, ssize_t value)
{
	size_t done = 0;

	while (done < sizeof(value)) {
		ssize_t ret = write(fd, (char *)&value + done, sizeof(value) - done);

		if (ret <= 0) {
			pr_perror("Failed writing result");
			return -1;
		}
		done += ret;
	}
	return 0;
}

static int validate_compressed_sizes(uint32_t *compressed_size, int n_pages,
				     uint64_t total_compressed_size)
{
	uint64_t sum = 0;

	for (int i = 0; i < n_pages; i++) {
		if (compressed_size[i] > PAGE_COMPRESSED_SIZE_BOUND) {
			pr_err("Page %d: compressed_size %u exceeds bound\n",
			       i, compressed_size[i]);
			return -1;
		}
		sum += compressed_size[i];
	}
	if (sum != total_compressed_size) {
		pr_err("Compressed size mismatch: sum %" PRIu64 " != total %" PRIu64 "\n",
		       sum, total_compressed_size);
		return -1;
	}
	return 0;
}

static int validate_compressed_sizes_region(uint32_t *compressed_size,
					    uint16_t *block_pages,
					    int n_blocks,
					    int total_pages,
					    uint64_t total_compressed_size,
					    unsigned int region_pages)
{
	uint64_t sum_cs = 0;
	uint64_t sum_pages = 0;
	int i;

	for (i = 0; i < n_blocks; i++) {
		size_t bound;

		if (block_pages[i] == 0 || block_pages[i] > region_pages) {
			pr_err("Block %d: invalid block_pages %u (region=%u)\n",
			       i, block_pages[i], region_pages);
			return -1;
		}
		bound = REGION_COMPRESSED_SIZE_BOUND(block_pages[i]);
		if (compressed_size[i] > bound) {
			pr_err("Block %d: compressed_size %u exceeds bound %zu\n",
			       i, compressed_size[i], bound);
			return -1;
		}
		sum_cs += compressed_size[i];
		sum_pages += block_pages[i];
	}
	if (sum_cs != total_compressed_size) {
		pr_err("Compressed size mismatch: sum %" PRIu64 " != total %" PRIu64 "\n",
		       sum_cs, total_compressed_size);
		return -1;
	}
	if (sum_pages != (uint64_t)total_pages) {
		pr_err("Block pages mismatch: sum %" PRIu64 " != total %d\n",
		       sum_pages, total_pages);
		return -1;
	}
	return 0;
}

static void decompress_pages(char *decompressed_buf, char *compressed_buf,
			     const uint32_t *compressed_size, int n_pages)
{
	off_t comp_off = 0;

	for (int i = 0; i < n_pages; i++) {
		char *dst = decompressed_buf + (i * PAGE_SIZE);

		if (compressed_size[i] == 0) {
			/* Zero page, already zeroed by MADV_DONTNEED */
		} else if (compressed_size[i] == PAGE_SIZE) {
			memcpy(dst, compressed_buf + comp_off, PAGE_SIZE);
		} else if (decompress_data(compressed_buf + comp_off,
					   compressed_size[i], PAGE_SIZE, dst)) {
			pr_err("Decompression failed for page %d\n", i);
			exit(1);
		}
		comp_off += compressed_size[i];
	}
}

/*
 * Region-mode equivalent of decompress_pages(). Each compressed_size[i]
 * covers block_pages[i] consecutive pages. Per-page compressed_size_out[]
 * is populated for build_write_iovecs(): 0 for all-zero pages (so they
 * can be skipped from process_vm_writev), PAGE_SIZE otherwise.
 */
static void decompress_regions(char *decompressed_buf, char *compressed_buf,
			       const uint32_t *compressed_size, const uint16_t *block_pages,
			       int n_blocks, uint32_t *compressed_size_out)
{
	off_t comp_off = 0;
	int page_idx = 0;
	int b, p;

	for (b = 0; b < n_blocks; b++) {
		unsigned int bp = block_pages[b];
		size_t block_bytes = (size_t)bp * PAGE_SIZE;
		char *dst = decompressed_buf + (size_t)page_idx * PAGE_SIZE;
		uint32_t cs = compressed_size[b];

		if (cs == 0) {
			/* All-zero region: buffer already zeroed via MADV_DONTNEED */
		} else if ((size_t)cs == block_bytes) {
			memcpy(dst, compressed_buf + comp_off, block_bytes);
		} else if (decompress_region(compressed_buf + comp_off,
					     cs, bp, dst)) {
			pr_err("Region decompression failed at block %d\n", b);
			exit(1);
		}

		for (p = 0; p < (int)bp; p++) {
			const char *page = dst + (size_t)p * PAGE_SIZE;

			compressed_size_out[page_idx++] =
				(cs == 0 || page_is_all_zero(page)) ?
					0 : PAGE_SIZE;
		}
		comp_off += cs;
	}
}

/* Check if addr is right after the end of iov */
static inline bool iov_extends(const struct iovec *iov, const void *addr)
{
	return iov->iov_base + iov->iov_len == addr;
}

/*
 * Build iovec pairs for process_vm_writev(), skipping zero pages.
 * Returns the number of iovecs built and total bytes to write.
 */
static int build_write_iovecs(struct iovec *local_iovs, struct iovec *remote_iovs_wr,
			      struct iovec *remote_iovs, char *decompressed_buf,
			      const uint32_t *compressed_size, int nr_iovs,
			      ssize_t *bytes_to_write)
{
	int nio = 0;
	int pi = 0;

	*bytes_to_write = 0;

	for (int i = 0; i < nr_iovs; i++) {
		int iov_pages = remote_iovs[i].iov_len / PAGE_SIZE;
		char *rbase = remote_iovs[i].iov_base;

		for (int j = 0; j < iov_pages; j++, pi++) {
			char *laddr = decompressed_buf + pi * PAGE_SIZE;

			if (compressed_size[pi] == 0) {
				rbase += PAGE_SIZE;
				continue;
			}

			if (nio > 0 && iov_extends(&local_iovs[nio - 1], laddr) &&
				       iov_extends(&remote_iovs_wr[nio - 1], rbase)) {
				local_iovs[nio - 1].iov_len += PAGE_SIZE;
				remote_iovs_wr[nio - 1].iov_len += PAGE_SIZE;
			} else {
				local_iovs[nio].iov_base = laddr;
				local_iovs[nio].iov_len = PAGE_SIZE;
				remote_iovs_wr[nio].iov_base = rbase;
				remote_iovs_wr[nio].iov_len = PAGE_SIZE;
				nio++;
			}
			rbase += PAGE_SIZE;
			*bytes_to_write += PAGE_SIZE;
		}
	}
	return nio;
}

/*
 * Write iovecs to a remote process, handling short writes and
 * the IOV_MAX limit on the number of iovecs per call.
 */
static int vm_writev_all(pid_t pid, struct iovec *local, struct iovec *remote,
			 int nio, ssize_t total)
{
	int iov_off = 0;
	ssize_t written = 0;

	while (written < total) {
		int cnt = nio - iov_off;
		ssize_t ret;

		if (cnt > IOV_MAX)
			cnt = IOV_MAX;

		ret = process_vm_writev(pid, local + iov_off, cnt, remote + iov_off, cnt, 0);
		if (ret < 0) {
			pr_perror("process_vm_writev failed");
			return -1;
		}
		if (ret == 0) {
			pr_err("process_vm_writev returned 0\n");
			return -1;
		}
		written += ret;

		/* Skip fully written iovecs */
		while (iov_off < nio && ret >= (ssize_t)local[iov_off].iov_len) {
			ret -= local[iov_off].iov_len;
			iov_off++;
		}
		/* Adjust partially written iovec */
		if (ret > 0 && iov_off < nio) {
			local[iov_off].iov_base += ret;
			local[iov_off].iov_len -= ret;
			remote[iov_off].iov_base += ret;
			remote[iov_off].iov_len -= ret;
		}
	}

	return 0;
}

int start_vma_io_pipe_daemon(int pages_img_fd, int pipe_fds[2][2])
{
	pid_t child_pid;
	int pipe_read_fd = pipe_fds[0][0], pipe_write_fd = pipe_fds[1][1];

	/* Pre-allocated reusable buffers */
	uint32_t *compressed_size = NULL;
	size_t cs_cap = 0;
	uint32_t *page_cs_shadow = NULL;
	size_t pcs_cap = 0;
	uint16_t *block_pages = NULL;
	size_t bp_cap = 0;
	struct iovec *remote_iovs = NULL, *local_iovs = NULL;
	struct iovec *remote_iovs_wr = NULL;  /* write-side remote iovecs */
	size_t iovs_cap = 0;
	char *compressed_buf = NULL, *decompressed_buf = NULL;
	size_t comp_cap = 0, decomp_cap = 0;

	child_pid = fork();
	if (child_pid == -1) {
		pr_perror("Failed to fork");
		return -1;
	}

	if (child_pid > 0) {
		return child_pid;
	}

	close(pipe_fds[1][0]);
	close(pipe_fds[0][1]);

	/* Hint the kernel for sequential readahead on the pages image */
	posix_fadvise(pages_img_fd, 0, 0, POSIX_FADV_SEQUENTIAL);

	/*
	 * Protocol (must match pipe_preadv_limited() in restorer):
	 *   1. struct pipe_hdr (pid, offs, total_cs, n_pages, nr_iovs,
	 *                       n_blocks, region_pages)
	 *   2. uint32_t compressed_size[n_blocks]
	 *   3. uint16_t block_pages[n_blocks]   (only when region_pages > 0)
	 *   4. struct iovec iovs[nr_iovs]       (remote dest layout)
	 *
	 * Response: ssize_t total_uncompressed_size
	 */
	while (1) {
		struct pipe_hdr hdr;
		ssize_t total_uncompressed;
		int ret;

		ret = read(pipe_read_fd, &hdr, sizeof(hdr));
		if (ret < 0) {
			pr_perror("Failed reading header");
			exit(1);
		}
		if (ret == 0)
			break; /* EOF, restorer closed the pipe */

		if ((size_t)ret < sizeof(hdr)) {
			if (read_full(pipe_read_fd,
				      (char *)&hdr + ret,
				      sizeof(hdr) - ret))
				exit(1);
		}

		if (hdr.n_pages <= 0 || hdr.nr_iovs <= 0 || hdr.n_blocks <= 0) {
			pr_err("Invalid header: n_pages=%d nr_iovs=%d n_blocks=%d\n",
			       hdr.n_pages, hdr.nr_iovs, hdr.n_blocks);
			exit(1);
		}
		if (hdr.region_pages > MAX_REGION_PAGES) {
			pr_err("Invalid header: region_pages=%u > %d\n",
			       hdr.region_pages, MAX_REGION_PAGES);
			exit(1);
		}
		if (hdr.region_pages == 0 && hdr.n_blocks != hdr.n_pages) {
			pr_err("Per-page mode but n_blocks(%d) != n_pages(%d)\n",
			       hdr.n_blocks, hdr.n_pages);
			exit(1);
		}

		/* Grow per-block compressed_size array if needed */
		if ((size_t)hdr.n_blocks > cs_cap) {
			cs_cap = hdr.n_blocks;
			compressed_size = xrealloc(compressed_size, cs_cap * sizeof(uint32_t));
			if (!compressed_size)
				exit(1);
		}

		if (read_full(pipe_read_fd, compressed_size, hdr.n_blocks * sizeof(uint32_t)))
			exit(1);

		if (hdr.region_pages > 0) {
			if ((size_t)hdr.n_blocks > bp_cap) {
				bp_cap = hdr.n_blocks;
				block_pages = xrealloc(block_pages, bp_cap * sizeof(uint16_t));
				if (!block_pages)
					exit(1);
			}
			if (read_full(pipe_read_fd, block_pages,
				      hdr.n_blocks * sizeof(uint16_t)))
				exit(1);
			if (validate_compressed_sizes_region(compressed_size,
							     block_pages,
							     hdr.n_blocks,
							     hdr.n_pages,
							     hdr.total_compressed_size,
							     hdr.region_pages))
				exit(1);
		} else {
			if (validate_compressed_sizes(compressed_size, hdr.n_blocks,
						      hdr.total_compressed_size))
				exit(1);
		}
		/*
		 * Grow iovec arrays if needed. The write-side arrays
		 * need room for up to n_pages entries because zero-page
		 * splitting may produce one iovec per non-zero page.
		 */
		{
			size_t need = hdr.n_pages > hdr.nr_iovs ?
				      hdr.n_pages : hdr.nr_iovs;

			if (need > iovs_cap) {
				size_t sz = need * sizeof(struct iovec);

				iovs_cap = need;
				remote_iovs = xrealloc(remote_iovs, sz);
				local_iovs = xrealloc(local_iovs, sz);
				remote_iovs_wr = xrealloc(remote_iovs_wr, sz);
				if (!remote_iovs || !local_iovs || !remote_iovs_wr)
					exit(1);
			}
		}

		if (read_full(pipe_read_fd, remote_iovs, hdr.nr_iovs * sizeof(struct iovec)))
			exit(1);

		/* Grow compressed data buffer if needed */
		if (hdr.total_compressed_size > comp_cap) {
			comp_cap = hdr.total_compressed_size;
			compressed_buf = xrealloc(compressed_buf, comp_cap);
			if (!compressed_buf)
				exit(1);
		}

		if (pread_img_data(pages_img_fd, compressed_buf,
				   hdr.total_compressed_size, hdr.offs))
			exit(1);
		/*
		 * Release page cache for the data we just read.
		 * This reduces memory pressure during restore of
		 * large processes.
		 */
		posix_fadvise(pages_img_fd, hdr.offs,
			      hdr.total_compressed_size,
			      POSIX_FADV_DONTNEED);

		/*
		 * Grow decompressed buffer if needed. Use mmap for
		 * page alignment. This enables the fast GUP path in
		 * process_vm_writev() and allows THP backing via
		 * MADV_HUGEPAGE to reduce TLB misses.
		 */
		total_uncompressed = (ssize_t)hdr.n_pages * PAGE_SIZE;
		if ((size_t)total_uncompressed > decomp_cap) {
			if (decompressed_buf)
				munmap(decompressed_buf, decomp_cap);
			decomp_cap = total_uncompressed;
			decompressed_buf = mmap(NULL, decomp_cap,
						PROT_READ | PROT_WRITE,
						MAP_PRIVATE | MAP_ANONYMOUS,
						-1, 0);
			if (decompressed_buf == MAP_FAILED) {
				pr_perror("Failed to mmap decompression buffer");
				exit(1);
			}
			madvise(decompressed_buf, decomp_cap, MADV_HUGEPAGE);
		}


		/* Re-zero buffer so zero-page slots are clean */
		madvise(decompressed_buf, total_uncompressed, MADV_DONTNEED);

		if (hdr.region_pages == 0) {
			decompress_pages(decompressed_buf, compressed_buf,
					 compressed_size, hdr.n_pages);
		} else {
			/*
			 * Region mode: decompress into the scratch and
			 * fold per-page zero info into a shadow array
			 * that build_write_iovecs() consumes the same way
			 * it does in per-page mode.
			 */
			if ((size_t)hdr.n_pages > pcs_cap) {
				pcs_cap = hdr.n_pages;
				page_cs_shadow = xrealloc(page_cs_shadow,
							  pcs_cap * sizeof(uint32_t));
				if (!page_cs_shadow)
					exit(1);
			}
			decompress_regions(decompressed_buf, compressed_buf,
					   compressed_size, block_pages,
					   hdr.n_blocks, page_cs_shadow);
		}

		{
			int nio;
			ssize_t bytes_to_write;
			const uint32_t *cs_for_iovecs = hdr.region_pages == 0 ?
				compressed_size : page_cs_shadow;

			nio = build_write_iovecs(local_iovs, remote_iovs_wr,
						 remote_iovs, decompressed_buf,
						 cs_for_iovecs, hdr.nr_iovs,
						 &bytes_to_write);

			if (vm_writev_all(hdr.remote_pid, local_iovs,
					  remote_iovs_wr, nio, bytes_to_write))
				exit(1);

			total_uncompressed = bytes_to_write;
		}

		if (write_pipe_result(pipe_write_fd, total_uncompressed))
			exit(1);
	}

	xfree(compressed_size);
	xfree(page_cs_shadow);
	xfree(block_pages);
	xfree(remote_iovs);
	xfree(remote_iovs_wr);
	xfree(local_iovs);
	xfree(compressed_buf);
	if (decompressed_buf)
		munmap(decompressed_buf, decomp_cap);
	exit(0);
}
