// Fuzz the sway compositor's config language the way `sway --validate -c <file>`
// does (sway/main.c): after server_init(), load_main_config(path, false, true)
// -> load_config() -> read_config() (sway/config.c: line continuation, block
// nesting, variable expansion) -> config_command() / execute_command()
// (sway/commands.c and every sway/commands/*.c handler: bindsym, output, input,
// workspace, bar, set, mode, font, colors, criteria, ...).
//
// The input is the config file's contents, written to a scratch file under
// $TMPDIR. Validation never runs `exec` commands and never touches outputs:
// load_main_config() frees the validated config and restores the previous one
// (NULL here) at the end, so no state carries over between inputs.
//
// Shell command substitution is the one other way a config runs commands:
// `include <path>` and expand_path() (output background / color_profile, input
// xkb_file) call wordexp(..., 0), which runs `$(...)` / backticks via /bin/sh.
// This target is linked with -Wl,--wrap=wordexp (mayhem/wordexp_nocmd.c), which
// adds WRDE_NOCMD, so such an input takes sway's own WRDE_CMDSUB error path and
// never runs anything; tilde, variable and glob expansion are unchanged.
//
// The server is initialised once, as sway does, with wlroots' headless backend
// and pixman renderer (WLR_BACKENDS / WLR_RENDERER, wlroots' own env switches),
// so no GPU, seat or display is needed. `include` lines can read other files
// read-only; that is the program's behaviour, not harness state.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include "log.h"
#include "sway/config.h"
#include "sway/server.h"

extern struct sway_server server; // sway/main.c

static char cfg_path[4096];

int LLVMFuzzerInitialize(int *argc, char ***argv) {
	const char *tmp = getenv("TMPDIR");
	if (!tmp || !*tmp) {
		tmp = "/tmp";
	}
	char rt[4096];
	snprintf(rt, sizeof(rt), "%s/sway-fuzz-XXXXXX", tmp);
	if (!mkdtemp(rt)) {
		abort();
	}
	setenv("XDG_RUNTIME_DIR", rt, 1);
	setenv("WLR_BACKENDS", "headless", 1);
	setenv("WLR_RENDERER", "pixman", 1);
	unsetenv("WAYLAND_DISPLAY");
	unsetenv("DISPLAY");
	unsetenv("SWAYSOCK");
	sway_log_init(SWAY_SILENT, NULL);
	wlr_log_init(WLR_SILENT, NULL);
	if (!server_init(&server)) {
		fprintf(stderr, "fuzz_sway_config: server_init failed\n");
		abort();
	}
	snprintf(cfg_path, sizeof(cfg_path), "%s/config", rt);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	FILE *f = fopen(cfg_path, "wb");
	if (!f) {
		abort();
	}
	if (size && fwrite(data, 1, size, f) != size) {
		abort();
	}
	fclose(f);
	load_main_config(cfg_path, false, true);
	return 0;
}
