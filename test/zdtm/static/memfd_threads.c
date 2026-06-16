#include <fcntl.h>
#include <linux/memfd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "zdtmtst.h"

const char *test_doc = "memfd content restore with many parked threads";
const char *test_author = "Dan Feigin <dfeigin@nvidia.com>";

/*
 * This is the shape that exercises the async memfd-fill daemon: a process
 * that owns a memfd (so CRIU runs the asynchronous content fill) AND has
 * many live threads, restored into a fresh pid namespace. The daemon's
 * worker threads run in the restored pid namespace and take TIDs there; if
 * the daemon is still alive when the restorer recreates an application
 * thread at its original TID via clone3(set_tid=...), the clone fails with
 * EEXIST ("Unable to create a thread: -17"). The fix drains and reaps the
 * daemon during CR_STATE_PRE_RESTORER, before any application thread is
 * cloned. memfd06 (no threads) cannot catch this.
 */

#define NR_THREADS 64
#define LEN	   PAGE_SIZE

#define err(exitcode, msg, ...)                \
	({                                     \
		pr_perror(msg, ##__VA_ARGS__); \
		exit(exitcode);                \
	})

static task_waiter_t tw[NR_THREADS];

static int _memfd_create(const char *name, unsigned int flags)
{
	return syscall(SYS_memfd_create, name, flags);
}

static void fill_pattern(char *buf, int variant)
{
	unsigned int seed = 0x9e3779b9U ^ (variant * 0x45d9f3bU);
	size_t i;

	for (i = 0; i < LEN; i++) {
		seed = seed * 1103515245U + 12345U;
		buf[i] = (char)(seed >> 24);
	}
}

static void *parked_thread(void *arg)
{
	long idx = (long)arg;

	/* Signal "alive", then park until the main task wakes us post-restore. */
	task_waiter_complete(&tw[idx], 1);
	task_waiter_wait4(&tw[idx], 2);

	return NULL;
}

int main(int argc, char *argv[])
{
	pthread_t threads[NR_THREADS];
	char expected[LEN];
	char buf[LEN];
	void *map;
	int fd;
	long i;

	test_init(argc, argv);

	for (i = 0; i < NR_THREADS; i++)
		task_waiter_init(&tw[i]);

	fd = _memfd_create("memfd-threads", MFD_CLOEXEC);
	if (fd < 0)
		err(1, "Can't call memfd_create");

	fill_pattern(expected, 0);
	if (write(fd, expected, LEN) != LEN)
		err(1, "write error");

	map = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		err(1, "Can't mmap memfd");

	for (i = 0; i < NR_THREADS; i++) {
		if (pthread_create(&threads[i], NULL, parked_thread, (void *)i))
			err(1, "Can't pthread_create");
	}

	/* Make sure every thread is alive and parked before C/R. */
	for (i = 0; i < NR_THREADS; i++)
		task_waiter_wait4(&tw[i], 1);

	test_daemon();
	test_waitsig();

	/* memfd content must survive restore, via both the mapping and the fd. */
	if (memcmp(map, expected, LEN)) {
		fail("shared mapping content mismatch");
		return 1;
	}

	if (pread(fd, buf, LEN, 0) != LEN) {
		fail("memfd read failed");
		return 1;
	}
	if (memcmp(buf, expected, LEN)) {
		fail("memfd fd content mismatch");
		return 1;
	}

	/* Release the parked threads and reap them. */
	for (i = 0; i < NR_THREADS; i++)
		task_waiter_complete(&tw[i], 2);
	for (i = 0; i < NR_THREADS; i++)
		pthread_join(threads[i], NULL);

	pass();

	return 0;
}
