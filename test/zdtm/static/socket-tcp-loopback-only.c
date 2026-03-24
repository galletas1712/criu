#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>

#include "zdtmtst.h"

const char *test_doc = "Check that --tcp-loopback-only preserves loopback TCP connections";
const char *test_author = "NVIDIA";

static int port = 8880;

int main(int argc, char **argv)
{
	int server_fd, accepted_fd, client_fd;
	char value = 'a';
	char got = 0;

	test_init(argc, argv);
	signal(SIGPIPE, SIG_IGN);

	server_fd = tcp_init_server(AF_INET, &port);
	if (server_fd < 0) {
		pr_err("Server initialization failed\n");
		return 1;
	}

	client_fd = tcp_init_client(AF_INET, "127.0.0.1", port);
	if (client_fd < 0) {
		pr_err("Client initialization failed\n");
		return 1;
	}

	accepted_fd = tcp_accept_server(server_fd);
	if (accepted_fd < 0) {
		pr_err("Accept failed\n");
		return 1;
	}

	close(server_fd);

	test_daemon();
	test_waitsig();

	if (write(client_fd, &value, sizeof(value)) != sizeof(value)) {
		fail("write client");
		return 1;
	}

	if (read(accepted_fd, &got, sizeof(got)) != sizeof(got) || got != value) {
		fail("read accepted");
		return 1;
	}

	if (write(accepted_fd, &value, sizeof(value)) != sizeof(value)) {
		fail("write accepted");
		return 1;
	}

	if (read(client_fd, &got, sizeof(got)) != sizeof(got) || got != value) {
		fail("read client");
		return 1;
	}

	pass();
	return 0;
}
