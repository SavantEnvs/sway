// Fuzz swaynag's config file parser (swaynag/config.c swaynag_load_config):
// [type] sections, every other line turned into a --long-option and handed to
// swaynag_parse_options (colors, sizes, edges, layers, fonts, buttons), then
// the type merge swaynag's main() performs.
//
// The swaynag struct and type list are set up per input exactly as
// swaynag/main.c does before it loads the config; stdin is /dev/null so the
// --detailed-message option (reads stdin) is deterministic.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "list.h"
#include "log.h"
#include "swaynag/config.h"
#include "swaynag/swaynag.h"
#include "swaynag/types.h"

static char path[4096];

int LLVMFuzzerInitialize(int *argc, char ***argv) {
	sway_log_init(SWAY_SILENT, NULL);
	int fd = open("/dev/null", O_RDONLY);
	if (fd >= 0) {
		dup2(fd, STDIN_FILENO);
		close(fd);
	}
	const char *tmp = getenv("TMPDIR");
	snprintf(path, sizeof(path), "%s/swaynag-config.XXXXXX", tmp && *tmp ? tmp : "/tmp");
	fd = mkstemp(path);
	if (fd < 0) {
		abort();
	}
	close(fd);
	return 0;
}

static void free_swaynag(struct swaynag *swaynag) {
	for (int i = 0; i < swaynag->buttons->length; ++i) {
		struct swaynag_button *button = swaynag->buttons->items[i];
		free(button->text);
		free(button->action);
		free(button);
	}
	list_free(swaynag->buttons);
	free(swaynag->message);
	free(swaynag->details.message);
	free(swaynag->details.details_text);
	free(swaynag->details.button_up.text);
	free(swaynag->details.button_down.text);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	FILE *f = fopen(path, "w");
	if (!f) {
		abort();
	}
	fwrite(data, 1, size, f);
	fclose(f);

	struct swaynag swaynag;
	memset(&swaynag, 0, sizeof(swaynag));
	list_t *types = create_list();
	swaynag_types_add_default(types);
	swaynag.buttons = create_list();
	wl_list_init(&swaynag.outputs);
	wl_list_init(&swaynag.seats);
	struct swaynag_button *button_close = calloc(1, sizeof(*button_close));
	button_close->text = strdup("X");
	button_close->type = SWAYNAG_ACTION_DISMISS;
	list_add(swaynag.buttons, button_close);
	swaynag.details.details_text = strdup("Toggle details");

	if (swaynag_load_config(path, &swaynag, types) == 0) {
		if (!swaynag.type) {
			swaynag.type = swaynag_type_get(types, "error");
		}
		struct swaynag_type *type = swaynag_type_new(swaynag.type->name);
		swaynag_type_merge(type, swaynag_type_get(types, "<defaults>"));
		swaynag_type_merge(type, swaynag_type_get(types, "<config>"));
		swaynag_type_merge(type, swaynag.type);
		swaynag_type_free(type);
	}

	swaynag_types_free(types);
	free_swaynag(&swaynag);
	return 0;
}
