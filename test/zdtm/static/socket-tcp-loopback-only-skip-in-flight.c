#include <arpa/inet.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "zdtmtst.h"

const char *test_doc = "Check that --tcp-loopback-only still rejects non-loopback listeners with --skip-in-flight";
const char *test_author = "NVIDIA";

static int port = 8880;
static const char nonloopback_addr[] = "192.0.2.1";

static int setup_nonloopback_lo(void)
{
	if (unshare(CLONE_NEWNET)) {
		pr_perror("unshare");
		return -1;
	}

	if (system("ip link set up dev lo")) {
		pr_err("failed to bring loopback up\n");
		return -1;
	}

	if (system("ip addr add 192.0.2.1/32 dev lo")) {
		pr_err("failed to add non-loopback address to loopback device\n");
		return -1;
	}

	return 0;
}

static int tcp_init_server_at(const char *addr, int *port)
{
	struct sockaddr_in sockaddr;
	int fd;
	int one = 1;

	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		pr_perror("socket");
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))) {
		pr_perror("setsockopt");
		close(fd);
		return -1;
	}

	memset(&sockaddr, 0, sizeof(sockaddr));
	sockaddr.sin_family = AF_INET;
	sockaddr.sin_port = htons(*port);
	if (inet_pton(AF_INET, addr, &sockaddr.sin_addr) != 1) {
		pr_err("failed to parse address %s\n", addr);
		close(fd);
		return -1;
	}

	if (bind(fd, (struct sockaddr *)&sockaddr, sizeof(sockaddr))) {
		pr_perror("bind");
		close(fd);
		return -1;
	}

	if (listen(fd, 1)) {
		pr_perror("listen");
		close(fd);
		return -1;
	}

	return fd;
}

int main(int argc, char **argv)
{
	int server_fd, client_fd;

	test_init(argc, argv);
	signal(SIGPIPE, SIG_IGN);

	if (setup_nonloopback_lo())
		return 1;

	server_fd = tcp_init_server_at(nonloopback_addr, &port);
	if (server_fd < 0)
		return 1;

	client_fd = tcp_init_client(AF_INET, (char *)nonloopback_addr, port);
	if (client_fd < 0) {
		pr_err("Client initialization failed\n");
		return 1;
	}

	test_daemon();
	test_waitsig();

	close(client_fd);
	close(server_fd);
	pass();
	return 0;
}
