// Fuzz swaybar's handling of the bar configuration sway sends over IPC:
// ipc_initialize() -> ipc_parse_config() (swaybar/ipc.c): bar id lookup, mode,
// position, fonts, gaps, colors, bindings, outputs, tray outputs/bindings,
// status_command, workspace options; plus common/ipc-client.c reply framing.
//
// swaybar is started with a bar id (as `swaybar -b <id>` and sway itself do),
// so ipc_initialize() sends one IPC_GET_BAR_CONFIG request and parses sway's
// reply. The input is the reply PAYLOAD: sway writes the i3-ipc header
// ("i3-ipc", u32 length, u32 type) itself, so the harness frames the payload
// with its true length the same way. A writer thread plays sway on the other
// end of a socketpair (so payloads larger than the socket buffer still
// arrive), then answers the IPC_SUBSCRIBE that follows with {"success":true}.
// Nothing parsed here is executed: status_command is only stored.
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "ipc.h"
#include "ipc-client.h"
#include "log.h"
#include "swaybar/bar.h"
#include "swaybar/config.h"
#include "swaybar/ipc.h"

struct reply {
	int fd;
	const uint8_t *data;
	size_t size;
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

static void write_reply(int fd, uint32_t type, const void *payload, uint32_t len) {
	char header[14]; // magic + u32 length + u32 type (common/ipc-client.c)
	memcpy(header, "i3-ipc", 6);
	memcpy(header + 6, &len, sizeof(len));
	memcpy(header + 10, &type, sizeof(type));
	write_all(fd, header, sizeof(header));
	write_all(fd, payload, len);
}

static void *sway_side(void *arg) {
	struct reply *r = arg;
	write_reply(r->fd, IPC_GET_BAR_CONFIG, r->data, (uint32_t)r->size);
	static const char ok[] = "{\"success\":true}";
	write_reply(r->fd, IPC_SUBSCRIBE, ok, sizeof(ok) - 1);
	return NULL;
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
	sway_log_init(SWAY_SILENT, NULL);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size > UINT32_MAX) {
		return -1; // the i3-ipc length field is a u32
	}
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
		abort();
	}
	struct swaybar bar;
	memset(&bar, 0, sizeof(bar));
	bar.id = strdup("bar-0");
	bar.config = init_config();
	bar.ipc_socketfd = sv[0];
	bar.ipc_event_socketfd = sv[0];

	struct reply r = { .fd = sv[1], .data = data, .size = size };
	pthread_t th;
	if (pthread_create(&th, NULL, sway_side, &r) != 0) {
		abort();
	}
	ipc_initialize(&bar);
	pthread_join(th, NULL);

	close(sv[0]);
	close(sv[1]);
	free_config(bar.config);
	free(bar.id);
	return 0;
}
