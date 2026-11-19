// Fuzz swaybar's status_command reader: protocol detection, the i3bar JSON
// header, the streaming i3bar block array and the plain-text protocol
// (swaybar/status_line.c status_handle_readable, swaybar/i3bar.c).
//
// The status command's stdout reaches swaybar through a non-blocking pipe, so
// the input is written into a real pipe in chunks and status_handle_readable
// is called after each chunk, exactly as swaybar's event loop does when the
// fd becomes readable. The write end stays open (the command is "still
// running"), so the reader stops on EAGAIN.
//
// status_line_init() forks `sh -c <status_command>`, so the struct is set up
// here the way status_line_init() leaves it, without the child.
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "log.h"
#include "loop.h"
#include "swaybar/bar.h"
#include "swaybar/i3bar.h"
#include "swaybar/status_line.h"

static struct swaybar bar;

int LLVMFuzzerInitialize(int *argc, char ***argv) {
	sway_log_init(SWAY_SILENT, NULL);
	bar.eventloop = loop_create();
	signal(SIGPIPE, SIG_IGN);
	return 0;
}

// Mirrors status_line_free() minus the kill()/waitpid() of the child we did
// not fork. After status_error() the fds are already closed (nothing opened a
// new fd since), so fclose() only frees the FILE wrappers. The i3bar state
// (blocks + tokener) is freed whenever it was created, also after an error:
// status_line_free() only frees it while the protocol is still I3BAR, which
// leaks once per swaybar process but would grow the fuzzer's heap per input.
static void free_status(struct status_line *status) {
	fclose(status->read);
	fclose(status->write);
	if (status->tokener) {
		struct i3bar_block *block, *tmp;
		wl_list_for_each_safe(block, tmp, &status->blocks, link) {
			wl_list_remove(&block->link);
			i3bar_block_unref(block);
		}
		json_tokener_free(status->tokener);
	}
	free(status->buffer);
	free(status);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	int out[2], in[2];
	if (pipe(out) != 0 || pipe(in) != 0) {
		abort();
	}
	struct status_line *status = calloc(1, sizeof(*status));
	status->bar = &bar;
	status->stop_signal = SIGSTOP;
	status->cont_signal = SIGCONT;
	status->buffer_size = 8192;
	status->buffer = malloc(status->buffer_size);
	status->read_fd = out[0];
	fcntl(status->read_fd, F_SETFL, O_NONBLOCK);
	status->write_fd = in[1];
	fcntl(status->write_fd, F_SETFL, O_NONBLOCK);
	status->read = fdopen(status->read_fd, "r");
	status->write = fdopen(status->write_fd, "w");
	fcntl(out[1], F_SETFL, O_NONBLOCK);

	size_t off = 0;
	while (off < size && status->protocol != PROTOCOL_ERROR) {
		ssize_t n = write(out[1], data + off, size - off);
		if (n <= 0) {
			break; // pipe full: swaybar did not drain it, stop delivering
		}
		off += (size_t)n;
		status_handle_readable(status);
	}

	free_status(status);
	close(out[1]);
	close(in[0]);
	return 0;
}
