#include "criu-log.h"
#include "plugin.h"
#include "util.h"
#include "cr_options.h"
#include "pid.h"
#include "proc_parse.h"
#include "seize.h"
#include "fault-injection.h"

#include <common/list.h>
#include <compel/infect.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>

/* cuda-checkpoint binary should live in your PATH */
#define CUDA_CHECKPOINT "cuda-checkpoint"

/* cuda-checkpoint --action flags */
#define ACTION_LOCK	  "lock"
#define ACTION_CHECKPOINT "checkpoint"
#define ACTION_RESTORE	  "restore"
#define ACTION_UNLOCK	  "unlock"

typedef enum {
	CUDA_TASK_RUNNING = 0,
	CUDA_TASK_LOCKED,
	CUDA_TASK_CHECKPOINTED,
	CUDA_TASK_UNKNOWN = -1
} cuda_task_state_t;

#define CUDA_CKPT_BUF_SIZE (128)
#define CUDA_NVIDIA_FD_IMAGE "cuda-nvidia-fd.%x"
#define CUDA_NVIDIA_FD_IMAGE_VERSION 1
#define NVIDIA_DEV_MAJOR 195
#define NVIDIA_CTL_MINOR 255
#define NVIDIA_MODESET_MINOR 254
#define CUDA_SETFL_MASK (O_APPEND | O_ASYNC | O_NONBLOCK | O_NDELAY | O_DIRECT | O_NOATIME)

#ifdef LOG_PREFIX
#undef LOG_PREFIX
#endif
#define LOG_PREFIX "cuda_plugin: "

/* Disable plugin functionality if cuda-checkpoint is not in $PATH or driver
 * version doesn't support --action flag
 */
bool plugin_disabled = false;

bool plugin_added_to_inventory = false;

struct pid_info {
	int pid;
	char checkpointed;
	cuda_task_state_t initial_task_state;
	struct list_head list;
};

struct cuda_nvidia_fd_image {
	uint32_t version;
	uint32_t major;
	uint32_t minor;
	int32_t flags;
	char path[PATH_MAX];
};

/* Used to track which PID's we've paused CUDA operations on so far so we can
 * release them after we're done with the DUMP
 */
static LIST_HEAD(cuda_pids);

static void dealloc_pid_buffer(struct list_head *pid_buf)
{
	struct pid_info *info;
	struct pid_info *n;

	list_for_each_entry_safe(info, n, pid_buf, list) {
		list_del(&info->list);
		xfree(info);
	}
}

static int launch_cuda_checkpoint(const char **args, char *buf, int buf_size)
{
#define READ  0
#define WRITE 1
	int fd[2], buf_off;

	if (pipe(fd) != 0) {
		pr_perror("Couldn't create pipes for reading cuda-checkpoint output");
		return -1;
	}

	buf[0] = '\0';

	int child_pid = fork();
	if (child_pid == -1) {
		pr_perror("Failed to fork to exec cuda-checkpoint");
		close(fd[READ]);
		close(fd[WRITE]);
		return -1;
	}

	if (child_pid == 0) { // child
		if (dup2(fd[WRITE], STDOUT_FILENO) == -1) {
			pr_perror("unable to clone fd %d->%d", fd[WRITE], STDOUT_FILENO);
			_exit(EXIT_FAILURE);
		}
		if (dup2(fd[WRITE], STDERR_FILENO) == -1) {
			pr_perror("unable to clone fd %d->%d", fd[WRITE], STDERR_FILENO);
			_exit(EXIT_FAILURE);
		}
		close(fd[READ]);

		close_fds(STDERR_FILENO + 1);

		execvp(args[0], (char **)args);

		/* We can't use pr_error() as log file fd is closed. */
		fprintf(stderr, "execvp(\"%s\") failed: %s\n", args[0], strerror(errno));

		_exit(EXIT_FAILURE);
	}

	close(fd[WRITE]);
	buf_off = 0;
	/* Reserve one byte for the null charracter. */
	buf_size--;
	while (buf_off < buf_size) {
		int bytes_read;
		bytes_read = read(fd[READ], buf + buf_off, buf_size - buf_off);
		if (bytes_read == -1) {
			pr_perror("Unable to read output of cuda-checkpoint");
			goto err;
		}
		if (bytes_read == 0)
			break;
		buf_off += bytes_read;
	}
	buf[buf_off] = '\0';

	/* Clear out any of the remaining output in the pipe in case the buffer wasn't large enough */
	while (true) {
		char scratch[1024];
		int bytes_read;
		bytes_read = read(fd[READ], scratch, sizeof(scratch));
		if (bytes_read == -1) {
			pr_perror("Unable to read output of cuda-checkpoint");
			goto err;
		}
		if (bytes_read == 0)
			break;
	}
	close(fd[READ]);

	int status, exit_code = -1;
	if (waitpid(child_pid, &status, 0) == -1) {
		pr_perror("Unable to wait for the cuda-checkpoint process %d", child_pid);
		goto err;
	}
	if (WIFSIGNALED(status)) {
		int sig = WTERMSIG(status);
		pr_err("cuda-checkpoint unexpectedly signaled with %d: %s\n", sig, strsignal(sig));
	} else if (WIFEXITED(status)) {
		exit_code = WEXITSTATUS(status);
	} else {
		pr_err("cuda-checkpoint exited improperly: %u\n", status);
	}

	if (exit_code != EXIT_SUCCESS)
		pr_debug("cuda-checkpoint output ===>\n%s\n"
			 "<=== cuda-checkpoint output\n",
			 buf);

	return exit_code;
err:
	kill(child_pid, SIGKILL);
	waitpid(child_pid, NULL, 0);
	return -1;
}

static bool cuda_all_digits(const char *s)
{
	if (!s[0])
		return false;

	for (; s[0]; s++) {
		if (!isdigit((unsigned char)s[0]))
			return false;
	}
	return true;
}

static bool cuda_is_nvidia_device_name(const char *name)
{
	if (!strcmp(name, "nvidiactl") || !strcmp(name, "nvidia-modeset") ||
	    !strcmp(name, "nvidia-uvm") || !strcmp(name, "nvidia-uvm-tools"))
		return true;

	if (!strncmp(name, "nvidia-cap", strlen("nvidia-cap")))
		return cuda_all_digits(name + strlen("nvidia-cap"));

	if (!strncmp(name, "nvidia", strlen("nvidia")))
		return cuda_all_digits(name + strlen("nvidia"));

	return false;
}

static int cuda_normalize_nvidia_device_path(int fd, const struct stat *st, char *path, size_t path_size)
{
	char fd_path[64];
	char link_path[PATH_MAX];
	const char *name;
	ssize_t len;
	int ret;

	snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);
	len = readlink(fd_path, link_path, sizeof(link_path) - 1);
	if (len >= 0) {
		link_path[len] = '\0';
		name = strrchr(link_path, '/');
		name = name ? name + 1 : link_path;

		if (cuda_is_nvidia_device_name(name)) {
			if (!strncmp(name, "nvidia-cap", strlen("nvidia-cap"))) {
				ret = snprintf(path, path_size, "/dev/nvidia-caps/%s", name);
				if (ret < 0 || (size_t)ret >= path_size)
					return -1;
			} else {
				ret = snprintf(path, path_size, "/dev/%s", name);
				if (ret < 0 || (size_t)ret >= path_size)
					return -1;
			}
			return 0;
		}
	}

	if (major(st->st_rdev) != NVIDIA_DEV_MAJOR)
		return -ENOTSUP;

	switch (minor(st->st_rdev)) {
	case NVIDIA_CTL_MINOR:
		name = "nvidiactl";
		break;
	case NVIDIA_MODESET_MINOR:
		name = "nvidia-modeset";
		break;
	default:
		ret = snprintf(path, path_size, "/dev/nvidia%u", minor(st->st_rdev));
		if (ret < 0 || (size_t)ret >= path_size)
			return -1;
		return 0;
	}

	ret = snprintf(path, path_size, "/dev/%s", name);
	if (ret < 0 || (size_t)ret >= path_size)
		return -1;
	return 0;
}

static int cuda_read_full(int fd, void *buf, size_t len)
{
	char *p = buf;

	while (len > 0) {
		ssize_t ret = read(fd, p, len);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (ret == 0)
			return -EIO;
		p += ret;
		len -= ret;
	}
	return 0;
}

static int cuda_write_full(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len > 0) {
		ssize_t ret = write(fd, p, len);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (ret == 0)
			return -EIO;
		p += ret;
		len -= ret;
	}
	return 0;
}

static int cuda_restore_fd_status_flags(int fd, int flags)
{
	int ret;
	int restored;

	ret = fcntl(fd, F_GETFL, 0);
	if (ret < 0)
		return -errno;

	restored = (ret & ~CUDA_SETFL_MASK) | (flags & CUDA_SETFL_MASK);
	if (fcntl(fd, F_SETFL, restored) < 0)
		return -errno;

	return 0;
}

int cuda_plugin_dump_file(int fd, int id)
{
	struct cuda_nvidia_fd_image image = {
		.version = CUDA_NVIDIA_FD_IMAGE_VERSION,
	};
	char img_path[PATH_MAX];
	struct stat st;
	int ret;
	int img_fd;

	if (plugin_disabled)
		return -ENOTSUP;

	if (fstat(fd, &st) < 0) {
		pr_perror("Unable to stat fd %d", fd);
		return -1;
	}

	if (!S_ISCHR(st.st_mode))
		return -ENOTSUP;

	ret = cuda_normalize_nvidia_device_path(fd, &st, image.path, sizeof(image.path));
	if (ret == -ENOTSUP)
		return -ENOTSUP;
	if (ret < 0)
		return -1;

	ret = fcntl(fd, F_GETFL, 0);
	if (ret < 0) {
		pr_perror("Unable to get status flags for NVIDIA fd %d", fd);
		return -1;
	}

	image.flags = ret;
	image.major = major(st.st_rdev);
	image.minor = minor(st.st_rdev);

	if (!plugin_added_to_inventory) {
		if (add_inventory_plugin(CR_PLUGIN_DESC.name)) {
			pr_err("Failed to add CUDA plugin to inventory image\n");
			return -1;
		}
		plugin_added_to_inventory = true;
	}

	snprintf(img_path, sizeof(img_path), CUDA_NVIDIA_FD_IMAGE, id);
	img_fd = openat(criu_get_image_dir(), img_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (img_fd < 0) {
		pr_perror("Unable to open %s", img_path);
		return -1;
	}

	ret = cuda_write_full(img_fd, &image, sizeof(image));
	if (ret < 0) {
		errno = -ret;
		pr_perror("Unable to write %s", img_path);
		close(img_fd);
		return -1;
	}

	if (close(img_fd) < 0) {
		pr_perror("Unable to close %s", img_path);
		return -1;
	}

	pr_info("Dumped NVIDIA device fd %d id %#x path %s dev %u:%u flags %#x\n",
		fd, id, image.path, image.major, image.minor, image.flags);
	return 0;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__DUMP_EXT_FILE, cuda_plugin_dump_file)

int cuda_plugin_restore_file(int id, bool *retry_needed)
{
	struct cuda_nvidia_fd_image image;
	char img_path[PATH_MAX];
	int ret;
	int img_fd;
	int fd;
	struct stat restored_st;

	*retry_needed = false;

	if (plugin_disabled)
		return -ENOTSUP;

	snprintf(img_path, sizeof(img_path), CUDA_NVIDIA_FD_IMAGE, id);
	img_fd = openat(criu_get_image_dir(), img_path, O_RDONLY);
	if (img_fd < 0) {
		if (errno == ENOENT)
			return -ENOTSUP;
		pr_perror("Unable to open %s", img_path);
		return -1;
	}

	ret = cuda_read_full(img_fd, &image, sizeof(image));
	close(img_fd);
	if (ret < 0) {
		errno = -ret;
		pr_perror("Unable to read %s", img_path);
		return -1;
	}

	if (image.version != CUDA_NVIDIA_FD_IMAGE_VERSION ||
	    !cuda_is_nvidia_device_name(strrchr(image.path, '/') ? strrchr(image.path, '/') + 1 : image.path)) {
		pr_err("Invalid NVIDIA fd image %s for id %#x\n", img_path, id);
		return -1;
	}

	fd = open(image.path, (image.flags & O_ACCMODE) | O_CLOEXEC);
	if (fd < 0) {
		pr_perror("Unable to restore NVIDIA device fd id %#x path %s", id, image.path);
		return -1;
	}

	if (fstat(fd, &restored_st) < 0) {
		pr_perror("Unable to stat restored NVIDIA device fd id %#x path %s", id, image.path);
		close(fd);
		return -1;
	}
	if (!S_ISCHR(restored_st.st_mode)) {
		pr_err("Restored NVIDIA fd id %#x path %s is not a character device\n", id, image.path);
		close(fd);
		return -1;
	}
	if (image.major == NVIDIA_DEV_MAJOR &&
	    (major(restored_st.st_rdev) != image.major || minor(restored_st.st_rdev) != image.minor)) {
		pr_err("Restored NVIDIA fd id %#x path %s dev changed from %u:%u to %u:%u\n",
		       id, image.path, image.major, image.minor,
		       major(restored_st.st_rdev), minor(restored_st.st_rdev));
		close(fd);
		return -1;
	}

	ret = cuda_restore_fd_status_flags(fd, image.flags);
	if (ret < 0) {
		errno = -ret;
		pr_perror("Unable to restore status flags for NVIDIA device fd id %#x path %s", id, image.path);
		close(fd);
		return -1;
	}

	pr_info("Restored NVIDIA device fd id %#x path %s dev %u:%u flags %#x as fd %d\n",
		id, image.path, image.major, image.minor, image.flags, fd);
	return fd;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__RESTORE_EXT_FILE, cuda_plugin_restore_file)

/**
 * Checks if a given flag is supported by the cuda-checkpoint utility
 *
 * Returns:
 *  1 if the flag is supported,
 *  0 if the flag is not supported,
 *  -1 if there was an error launching the cuda-checkpoint utility.
 */
static int cuda_checkpoint_supports_flag(const char *flag)
{
	char msg_buf[2048];
	const char *args[] = { CUDA_CHECKPOINT, "-h", NULL };

	if (launch_cuda_checkpoint(args, msg_buf, sizeof(msg_buf)) != 0)
		return -1;

	if (strstr(msg_buf, flag) == NULL)
		return 0;

	return 1;
}

/* Retrieve the cuda restore thread TID from the root pid */
static int get_cuda_restore_tid(int root_pid)
{
	char pid_buf[16];
	char pid_out[CUDA_CKPT_BUF_SIZE];

	snprintf(pid_buf, sizeof(pid_buf), "%d", root_pid);

	const char *args[] = { CUDA_CHECKPOINT, "--get-restore-tid", "--pid", pid_buf, NULL };
	int ret = launch_cuda_checkpoint(args, pid_out, sizeof(pid_out));
	if (ret != 0) {
		pr_err("Failed to launch cuda-checkpoint to retrieve restore tid: %s\n", pid_out);
		return -1;
	}

	return atoi(pid_out);
}

static int cuda_process_checkpoint_action(int pid, const char *action, unsigned int timeout, char *msg_buf,
					  int buf_size)
{
	char pid_buf[16];
	char timeout_buf[16];

	snprintf(pid_buf, sizeof(pid_buf), "%d", pid);

	const char *args[] = { CUDA_CHECKPOINT, "--action", action, "--pid", pid_buf, NULL /* --timeout */,
			       NULL /* timeout_val */, NULL };
	if (timeout > 0) {
		snprintf(timeout_buf, sizeof(timeout_buf), "%d", timeout);
		args[5] = "--timeout";
		args[6] = timeout_buf;
	}

	return launch_cuda_checkpoint(args, msg_buf, buf_size);
}

static int interrupt_restore_thread(int restore_tid, k_rtsigset_t *restore_sigset)
{
	/* Since we resumed a thread that CRIU previously already froze we need to
	 * INTERRUPT it once again, task was already SEIZE'd so we don't need to do
	 * a compel_interrupt_task()
	 */
	if (ptrace(PTRACE_INTERRUPT, restore_tid, NULL, 0)) {
		pr_perror("Could not interrupt cuda restore tid %d after checkpoint, process may be in strange state",
			  restore_tid);
		return -1;
	}

	struct proc_status_creds creds;
	if (compel_wait_task(restore_tid, -1, parse_pid_status, NULL, &creds.s, NULL) != COMPEL_TASK_ALIVE) {
		pr_err("compel_wait_task failed after interrupt\n");
		return -1;
	}

	if (ptrace(PTRACE_SETOPTIONS, restore_tid, NULL, PTRACE_O_SUSPEND_SECCOMP | PTRACE_O_TRACESYSGOOD)) {
		pr_perror("Failed to set ptrace options on interrupt for restore tid %d", restore_tid);
		return -1;
	}

	if (ptrace(PTRACE_SETSIGMASK, restore_tid, sizeof(*restore_sigset), restore_sigset)) {
		pr_perror("Unable to restore original sigmask to restore tid %d", restore_tid);
		return -1;
	}

	return 0;
}

static int resume_restore_thread(int restore_tid, k_rtsigset_t *save_sigset)
{
	k_rtsigset_t block;

	if (ptrace(PTRACE_GETSIGMASK, restore_tid, sizeof(*save_sigset), save_sigset)) {
		pr_perror("Failed to get current sigmask for restore tid %d", restore_tid);
		return -1;
	}

	ksigfillset(&block);
	ksigdelset(&block, SIGTRAP);

	if (ptrace(PTRACE_SETSIGMASK, restore_tid, sizeof(block), &block)) {
		pr_perror("Failed to block signals on restore tid %d", restore_tid);
		return -1;
	}

	// Clear out PTRACE_O_SUSPEND_SECCOMP when we resume the restore thread
	if (ptrace(PTRACE_SETOPTIONS, restore_tid, NULL, 0)) {
		pr_perror("Could not clear ptrace options on restore tid %d", restore_tid);
		return -1;
	}

	if (ptrace(PTRACE_CONT, restore_tid, NULL, 0)) {
		pr_perror("Could not resume cuda restore tid %d", restore_tid);
		return -1;
	}

	return 0;
}

int cuda_plugin_checkpoint_devices(int pid)
{
	if (plugin_disabled) {
		return -ENOTSUP;
	}

	pr_info("Dynamo snapshot-agent owns CUDA checkpoint; skipping checkpoint devices on pid %d\n", pid);
	return 0;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__CHECKPOINT_DEVICES, cuda_plugin_checkpoint_devices);

int cuda_plugin_pause_devices(int pid)
{
	if (plugin_disabled) {
		return -ENOTSUP;
	}

	pr_info("Dynamo snapshot-agent owns CUDA checkpoint; skipping pause devices on pid %d\n", pid);
	return 0;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__PAUSE_DEVICES, cuda_plugin_pause_devices)

int resume_device(int pid, int checkpointed, cuda_task_state_t initial_task_state)
{
	char msg_buf[CUDA_CKPT_BUF_SIZE];
	int status;
	int ret = 0;
	int int_ret;
	k_rtsigset_t save_sigset;

	if (initial_task_state == CUDA_TASK_UNKNOWN) {
		pr_info("skip resume for PID %d (unknown state)\n", pid);
		return 0;
	}

	int restore_tid = get_cuda_restore_tid(pid);
	if (restore_tid == -1) {
		pr_info("No need to resume devices on pid %d\n", pid);
		return 0;
	}

	pr_info("resuming devices on pid %d\n", pid);
	/* The resuming process has to stay frozen during this time otherwise
	 * attempting to access a UVM pointer will crash if we haven't restored the
	 * underlying mappings yet
	 */
	pr_debug("Restore thread pid %d found for real pid %d\n", restore_tid, pid);
	/* wakeup the restore thread so we can handle the restore for this pid,
	 * rseq_cs has to be restored before execution
	 */
	if (resume_restore_thread(restore_tid, &save_sigset)) {
		return -1;
	}

	if (checkpointed && (initial_task_state == CUDA_TASK_RUNNING || initial_task_state == CUDA_TASK_LOCKED)) {
		/* If the process was "locked" or "running" before checkpointing it, we need to restore it */
		status = cuda_process_checkpoint_action(pid, ACTION_RESTORE, 0, msg_buf, sizeof(msg_buf));
		if (status) {
			pr_err("RESUME_DEVICES RESTORE failed with %s\n", msg_buf);
			ret = -1;
			goto interrupt;
		}
	}

	if (initial_task_state == CUDA_TASK_RUNNING) {
		/* If the process was "running" before we paused it, we need to unlock it */
		status = cuda_process_checkpoint_action(pid, ACTION_UNLOCK, 0, msg_buf, sizeof(msg_buf));
		if (status) {
			pr_err("RESUME_DEVICES UNLOCK failed with %s\n", msg_buf);
			ret = -1;
		}
	}

interrupt:
	int_ret = interrupt_restore_thread(restore_tid, &save_sigset);

	return ret != 0 ? ret : int_ret;
}

int cuda_plugin_resume_devices_late(int pid)
{
	if (plugin_disabled) {
		return -ENOTSUP;
	}

	pr_info("Dynamo snapshot-agent owns CUDA restore; skipping resume devices on pid %d\n", pid);
	return 0;
}
CR_PLUGIN_REGISTER_HOOK(CR_PLUGIN_HOOK__RESUME_DEVICES_LATE, cuda_plugin_resume_devices_late)

/**
 * Check if a CUDA device is available on the system
 */
static bool is_cuda_device_available(void)
{
	const char *gpu_path = "/proc/driver/nvidia/gpus/";
	struct stat sb;

	if (stat(gpu_path, &sb) != 0)
		return false;

	return S_ISDIR(sb.st_mode);
}

int cuda_plugin_init(int stage)
{
	int ret;

	/* Disable CUDA checkpointing with pre-dump */
	if (stage == CR_PLUGIN_STAGE__PRE_DUMP) {
		plugin_disabled = true;
		return 0;
	}

	if (stage == CR_PLUGIN_STAGE__RESTORE) {
		if (!check_and_remove_inventory_plugin(CR_PLUGIN_DESC.name, strlen(CR_PLUGIN_DESC.name))) {
			plugin_disabled = true;
			return 0;
		}
	}

	if (!fault_injected(FI_PLUGIN_CUDA_FORCE_ENABLE) && !is_cuda_device_available()) {
		pr_info("No GPU device found; CUDA plugin is disabled\n");
		plugin_disabled = true;
		return 0;
	}

	ret = cuda_checkpoint_supports_flag("--action");
	if (ret == -1) {
		pr_warn("check that %s is present in $PATH\n", CUDA_CHECKPOINT);
		plugin_disabled = true;
		return 0;
	}

	if (ret == 0) {
		pr_warn("cuda-checkpoint --action flag not supported, an r555 or higher version driver is required. Disabling CUDA plugin\n");
		plugin_disabled = true;
		return 0;
	}

	pr_info("initialized: %s stage %d\n", CR_PLUGIN_DESC.name, stage);

	/* In the DUMP stage track all the PID's we've paused CUDA operations on to
	 * release them when we're done if the user requested the leave-running option
	 */
	if (stage == CR_PLUGIN_STAGE__DUMP) {
		INIT_LIST_HEAD(&cuda_pids);
	}

	set_compel_interrupt_only_mode();

	return 0;
}

void cuda_plugin_fini(int stage, int ret)
{
	if (plugin_disabled) {
		return;
	}

	pr_info("finished %s stage %d err %d\n", CR_PLUGIN_DESC.name, stage, ret);

	/* Release all the paused PID's at the end of the DUMP stage in case the
	 * user provides the -R (leave-running) flag or an error occurred
	 */
	if (stage == CR_PLUGIN_STAGE__DUMP && (opts.final_state == TASK_ALIVE || ret != 0)) {
		struct pid_info *info;
		list_for_each_entry(info, &cuda_pids, list) {
			resume_device(info->pid, info->checkpointed, info->initial_task_state);
		}
	}
	if (stage == CR_PLUGIN_STAGE__DUMP) {
		dealloc_pid_buffer(&cuda_pids);
	}
}
CR_PLUGIN_REGISTER("cuda_plugin", cuda_plugin_init, cuda_plugin_fini)
