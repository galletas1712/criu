#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "zdtmtst.h"

const char *test_doc = "Check guarded CUDA staging zero filtering";
const char *test_author = "Schwinn Saereesitthipitak <schwinns@nvidia.com>";

#define STAGING_SIZE  (16UL << 20)
#define STAGING_PAGES (STAGING_SIZE / PAGE_SIZE)

static void fill_staging(uint8_t *staging)
{
	memset(staging, 0, STAGING_SIZE);
	staging[17] = 0x11;
	staging[1023 * PAGE_SIZE + 17] = 0x22;
	staging[1024 * PAGE_SIZE + 17] = 0x33;
	staging[(STAGING_PAGES - 1) * PAGE_SIZE + 17] = 0x44;
}

static uint8_t *alloc_staging(bool multiple, uint8_t **second)
{
	size_t size = STAGING_SIZE + 2 * PAGE_SIZE;
	uint8_t *reservation, *staging;

	if (multiple)
		size = 2 * STAGING_SIZE + 3 * PAGE_SIZE;
	reservation = mmap(NULL, size, PROT_NONE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (reservation == MAP_FAILED)
		return NULL;

	staging = reservation + PAGE_SIZE;
	if (mprotect(staging, STAGING_SIZE, PROT_READ | PROT_WRITE))
		return NULL;

	if (multiple) {
		*second = staging + STAGING_SIZE + PAGE_SIZE;
		if (mprotect(*second, STAGING_SIZE, PROT_READ | PROT_WRITE))
			return NULL;
	}

	return staging;
}

static int check_staging(const uint8_t *staging)
{
	size_t page;

	for (page = 0; page < STAGING_PAGES; page++) {
		uint8_t expected = 0;
		size_t offset;

		if (page == 0)
			expected = 0x11;
		else if (page == 1023)
			expected = 0x22;
		else if (page == 1024)
			expected = 0x33;
		else if (page == STAGING_PAGES - 1)
			expected = 0x44;

		for (offset = 0; offset < PAGE_SIZE; offset++) {
			uint8_t value = offset == 17 ? expected : 0;

			if (staging[page * PAGE_SIZE + offset] != value) {
				test_msg("Staging mismatch page=%zu offset=%zu\n",
					 page, offset);
				return -1;
			}
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	const char *mode;
	uint8_t *second = NULL;
	uint8_t *staging;
	uint8_t *near_miss;
	bool multiple;
	size_t page;

	test_init(argc, argv);

	mode = getenv("CUDA_STAGING_ZERO_TEST_MODE");
	multiple = mode && !strcmp(mode, "multiple");
	staging = alloc_staging(multiple, &second);
	if (!staging) {
		pr_perror("Unable to create staging mapping");
		return 1;
	}
	if (mode && !strcmp(mode, "bad-guard") &&
	    mprotect(staging - PAGE_SIZE, PAGE_SIZE, PROT_READ)) {
		pr_perror("Unable to invalidate staging guard");
		return 1;
	}

	fill_staging(staging);
	if (second)
		fill_staging(second);

	near_miss = mmap(NULL, STAGING_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (near_miss == MAP_FAILED) {
		pr_perror("Unable to create near-miss mapping");
		return 1;
	}
	for (page = 0; page < STAGING_PAGES; page++)
		near_miss[page * PAGE_SIZE + 31] = (page % 251) + 1;
	if (mprotect(near_miss, STAGING_SIZE, PROT_READ)) {
		pr_perror("Unable to make near-miss mapping read-only");
		return 1;
	}

	test_daemon();
	test_waitsig();

	if (check_staging(staging)) {
		fail("Guarded staging content changed");
		return 1;
	}
	if (second && check_staging(second)) {
		fail("Second guarded staging content changed");
		return 1;
	}
	for (page = 0; page < STAGING_PAGES; page++) {
		if (near_miss[page * PAGE_SIZE + 31] != (page % 251) + 1) {
			fail("Near-miss content changed");
			return 1;
		}
	}

	pass();
	return 0;
}
