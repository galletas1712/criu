#include <arpa/inet.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "zdtmtst.h"

const char *test_doc = "Check that --tcp-loopback-only rejects wildcard IPv4 TCP listeners";
const char *test_author = "NVIDIA";

static int port = 8880;

static int tcp_init_server_at(int *port)
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
	sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);

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
	int server_fd;

	test_init(argc, argv);
	signal(SIGPIPE, SIG_IGN);

	server_fd = tcp_init_server_at(&port);
	if (server_fd < 0)
		return 1;

	test_daemon();
	test_waitsig();

	close(server_fd);
	pass();
	return 0;
}
