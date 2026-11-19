// Fuzz swaymsg's handling of sway's IPC replies: reply framing
// (common/ipc-client.c ipc_recv_response), JSON parsing, success() and the
// pretty printers for commands, workspaces, inputs, seats, outputs, the tree,
// version and config (swaymsg/main.c).
//
// swaymsg/main.c is compiled with -Dmain=swaymsg_main (build plumbing only), and
// each input runs `swaymsg -p -s <socket> -t <type>` against a fake sway: a
// thread listening on a private Unix socket answers the one request with an
// i3-ipc reply. Input byte 0 picks the message type (one of swaymsg's own -t
// names); the rest is the reply payload, framed with its true length and the
// request's type exactly as sway frames it. Stdout (the pretty print) goes to
// /dev/null. getopt's optind is reset per input.
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "ipc.h"

int swaymsg_main(int argc, char **argv);

static const struct {
	const char *name;
	uint32_t type;
} types[] = {
	{"command", IPC_COMMAND},
	{"get_workspaces", IPC_GET_WORKSPACES},
	{"get_seats", IPC_GET_SEATS},
	{"get_inputs", IPC_GET_INPUTS},
	{"get_outputs", IPC_GET_OUTPUTS},
	{"get_tree", IPC_GET_TREE},
	{"get_marks", IPC_GET_MARKS},
	{"get_bar_config", IPC_GET_BAR_CONFIG},
	{"get_version", IPC_GET_VERSION},
	{"get_binding_modes", IPC_GET_BINDING_MODES},
	{"get_binding_state", IPC_GET_BINDING_STATE},
	{"get_config", IPC_GET_CONFIG},
	{"send_tick", IPC_SEND_TICK},
};

static char sock_path[108];
static int listen_fd = -1;

struct reply {
	uint32_t type;
	const uint8_t *data;
	uint32_t size;
};

static void write_all(int fd, const void *buf, size_t len) {
	const char *p = buf;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n <= 0) {
			return;
		}
		p += n;
		len -= (size_t)n;
	}
}

static void *sway_side(void *arg) {
	struct reply *r = arg;
	int fd = accept(listen_fd, NULL, NULL);
	if (fd < 0) {
		return NULL;
	}
	char header[14]; // magic + u32 length + u32 type (common/ipc-client.c)
	memcpy(header, "i3-ipc", 6);
	memcpy(header + 6, &r->size, sizeof(r->size));
	memcpy(header + 10, &r->type, sizeof(r->type));
	write_all(fd, header, sizeof(header));
	write_all(fd, r->data, r->size);
	// wait for swaymsg to close its end so the reply is fully consumed
	char buf[256];
	while (read(fd, buf, sizeof(buf)) > 0) {
	}
	close(fd);
	return NULL;
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
	const char *tmp = getenv("TMPDIR");
	char tmpl[4096];
	snprintf(tmpl, sizeof(tmpl), "%s/swaymsg.XXXXXX", tmp && *tmp ? tmp : "/tmp");
	char *dir = mkdtemp(tmpl);
	if (!dir || strlen(dir) + 6 >= sizeof(sock_path)) {
		abort();
	}
	snprintf(sock_path, sizeof(sock_path), "%s/sock", dir);
	listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	strcpy(addr.sun_path, sock_path);
	if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0
			|| listen(listen_fd, 4) != 0) {
		abort();
	}
	int null = open("/dev/null", O_WRONLY);
	if (null >= 0) {
		dup2(null, STDOUT_FILENO);
		close(null);
	}
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 1 || size - 1 > UINT32_MAX) {
		return -1; // byte 0 is the message type; the i3-ipc length is a u32
	}
	size_t t = data[0] % (sizeof(types) / sizeof(*types));
	struct reply r = { .type = types[t].type, .data = data + 1, .size = (uint32_t)(size - 1) };
	pthread_t th;
	if (pthread_create(&th, NULL, sway_side, &r) != 0) {
		abort();
	}
	char *argv[] = {"swaymsg", "-p", "-s", sock_path, "-t", (char *)types[t].name, NULL};
	optind = 1;
	swaymsg_main(6, argv);
	pthread_join(th, NULL);
	return 0;
}
