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

