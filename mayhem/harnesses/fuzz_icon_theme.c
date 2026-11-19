// Fuzz swaybar's tray icon-theme loader: the index.theme ini parser
// (swaybar/tray/icon.c read_theme_file, group_handler, entry_handler) and the
// icon lookup that walks the parsed subdirectories and Inherits chain.
//
// swaybar reads every <basedir>/<theme>/index.theme under $XDG_DATA_HOME/icons
// and $XDG_DATA_DIRS/icons. The harness points those variables at a private
// scratch dir (created once, under $TMPDIR) holding one theme directory
// "fuzz", writes the input as its index.theme and runs init_themes() +
// find_icon() + finish_themes() through the public API. The scratch dir also
// holds a few fixed icon files (never modified) so lookups can succeed.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "list.h"
#include "log.h"
#include "swaybar/tray/icon.h"

static char index_path[4096];

static void mkdirs(const char *root, const char *rel) {
	char p[4096];
	snprintf(p, sizeof(p), "%s/%s", root, rel);
	for (char *s = p + strlen(root) + 1; *s; ++s) {
		if (*s == '/') {
			*s = 0;
			mkdir(p, 0700);
			*s = '/';
		}
	}
	mkdir(p, 0700);
}

static void touch(const char *root, const char *rel) {
	char p[4096];
	snprintf(p, sizeof(p), "%s/%s", root, rel);
	FILE *f = fopen(p, "w");
	if (f) {
		fclose(f);
	}
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
	sway_log_init(SWAY_SILENT, NULL);
	const char *tmp = getenv("TMPDIR");
	char tmpl[4096];
	snprintf(tmpl, sizeof(tmpl), "%s/swayicons.XXXXXX", tmp && *tmp ? tmp : "/tmp");
	char *root = mkdtemp(tmpl);
	if (!root) {
		abort();
	}
	static char data[4096], home[4096], dirs[4096];
	snprintf(data, sizeof(data), "%s/data", root);
	snprintf(home, sizeof(home), "%s/home", root);
	snprintf(dirs, sizeof(dirs), "%s/nodirs", root);
	mkdirs(root, "home");
	mkdirs(root, "data/icons/fuzz/16x16/apps");
	mkdirs(root, "data/icons/fuzz/scalable/apps");
	mkdirs(root, "data/icons/fuzz/32x32");
	touch(root, "data/icons/fuzz/16x16/apps/x.png");
	touch(root, "data/icons/fuzz/scalable/apps/x.png");
	touch(root, "data/icons/fuzz/32x32/x.png");
	touch(root, "data/icons/x.png");
	setenv("HOME", home, 1);
	setenv("XDG_DATA_HOME", data, 1);
	setenv("XDG_DATA_DIRS", dirs, 1);
	snprintf(index_path, sizeof(index_path), "%s/icons/fuzz/index.theme", data);
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	FILE *f = fopen(index_path, "w");
	if (!f) {
		abort();
	}
	fwrite(data, 1, size, f);
	fclose(f);

	list_t *themes = NULL, *basedirs = NULL;
	init_themes(&themes, &basedirs);
	int min_size = 0, max_size = 0;
	char *theme = "fuzz";
	if (themes->length > 0) {
		struct icon_theme *t = themes->items[0];
		theme = t->name;
	}
	char *icon = find_icon(themes, basedirs, "x", 16, theme, &min_size, &max_size);
	free(icon);
	icon = find_icon(themes, basedirs, "x", 48, theme, &min_size, &max_size);
	free(icon);
	finish_themes(themes, basedirs);
	return 0;
}
