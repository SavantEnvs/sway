// Known-answer tests for the sway parsers the fuzz harnesses reach (upstream
// ships no test suite). Each check prints "PASS <name>" or "FAIL <name>";
// mayhem/test.sh counts them. Built by mayhem/build.sh with normal flags.
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "list.h"
#include "log.h"
#include "loop.h"
#include "swaybar/bar.h"
#include "swaybar/config.h"
#include "swaybar/i3bar.h"
#include "swaybar/status_line.h"
#include "swaybar/tray/icon.h"
#include "swaynag/config.h"
#include "swaynag/swaynag.h"
#include "swaynag/types.h"
#include "util.h"

static int failures = 0;

static void check(const char *name, int ok) {
	printf("%s %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) {
		failures++;
	}
}

static struct swaybar bar;

static struct status_line *feed_status(const char *text) {
	int out[2], in[2];
	if (pipe(out) != 0 || pipe(in) != 0) {
		abort();
	}
	struct status_line *status = calloc(1, sizeof(*status));
	status->bar = &bar;
	status->buffer_size = 8192;
	status->buffer = malloc(status->buffer_size);
	status->read_fd = out[0];
	fcntl(out[0], F_SETFL, O_NONBLOCK);
	status->write_fd = in[1];
	fcntl(in[1], F_SETFL, O_NONBLOCK);
	status->read = fdopen(out[0], "r");
	status->write = fdopen(in[1], "w");
	if (write(out[1], text, strlen(text)) != (ssize_t)strlen(text)) {
		abort();
	}
	status_handle_readable(status);
	return status;
}

static void test_status_line(void) {
	struct status_line *s = feed_status(
		"{\"version\":1,\"click_events\":true,\"stop_signal\":10}\n"
		"[\n[{\"full_text\":\"hello\",\"short_text\":\"hi\",\"color\":\"#ff0000\","
		"\"background\":\"#00ff00aa\",\"align\":\"center\",\"urgent\":true,"
		"\"name\":\"n1\",\"separator\":false,\"separator_block_width\":5,"
		"\"min_width\":\"xxxx\",\"markup\":\"pango\",\"border_top\":2},"
		"{\"full_text\":\"second\",\"min_width\":42}],\n");
	check("i3bar_protocol_detected", s->protocol == PROTOCOL_I3BAR);
	check("i3bar_click_events", s->click_events);
	check("i3bar_stop_signal", s->stop_signal == 10);
	int n = 0;
	struct i3bar_block *b, *first = NULL, *second = NULL;
	// i3bar_parse_json() inserts each block at the list head (render walks it in reverse)
	wl_list_for_each_reverse(b, &s->blocks, link) {
		if (n == 0) first = b;
		if (n == 1) second = b;
		n++;
	}
	check("i3bar_block_count", n == 2);
	check("i3bar_full_text", first && first->full_text && strcmp(first->full_text, "hello") == 0);
	check("i3bar_short_text", first && first->short_text && strcmp(first->short_text, "hi") == 0);
	check("i3bar_color", first && first->color_set && first->color == 0xff0000ff);
	check("i3bar_background", first && first->background == 0x00ff00aa);
	check("i3bar_align", first && first->align && strcmp(first->align, "center") == 0);
	check("i3bar_urgent_separator", first && first->urgent && !first->separator
		&& first->separator_block_width == 5);
	check("i3bar_markup", first && first->markup);
	check("i3bar_min_width_str", first && first->min_width_str
		&& strcmp(first->min_width_str, "xxxx") == 0);
	check("i3bar_border_top", first && first->border_top == 2);
	check("i3bar_min_width_int", second && second->min_width == 42);

	struct status_line *t = feed_status("first line\nsecond line\n");
	check("text_protocol_detected", t->protocol == PROTOCOL_TEXT);
	check("text_protocol_text", t->text && strcmp(t->text, "first line\nsecond line") == 0);

	struct status_line *e = feed_status("{\"version\":1}\n{oops");
	check("i3bar_invalid_json_error", e->protocol == PROTOCOL_ERROR
		&& e->text && strcmp(e->text, "[invalid i3bar json]") == 0);
}

static void test_icon_theme(const char *root) {
	char p[4096];
	const char *dirs[] = {"data", "data/icons", "data/icons/t", "data/icons/t/16x16",
		"data/icons/t/16x16/apps", "data/icons/t/scalable", "home"};
	for (size_t i = 0; i < sizeof(dirs) / sizeof(*dirs); i++) {
		snprintf(p, sizeof(p), "%s/%s", root, dirs[i]);
		mkdir(p, 0700);
	}
	FILE *f;
	const char *files[] = {"data/icons/t/16x16/apps/app.png", "data/icons/t/scalable/big.png"};
	for (size_t i = 0; i < 2; i++) {
		snprintf(p, sizeof(p), "%s/%s", root, files[i]);
		fclose(fopen(p, "w"));
	}
	snprintf(p, sizeof(p), "%s/data/icons/bad", root);
	mkdir(p, 0700);
	snprintf(p, sizeof(p), "%s/data/icons/bad/index.theme", root);
	f = fopen(p, "w");
	fputs("[Icon Theme]\nName=Bad,Name\nComment=x\nDirectories=a\n[a]\nSize=1\n", f);
	fclose(f);
	snprintf(p, sizeof(p), "%s/data/icons/t/index.theme", root);
	f = fopen(p, "w");
	fputs("[Icon Theme]\nName=TestTheme\nComment=known answer\nInherits=hicolor\n"
		"Directories=16x16/apps,scalable\n\n[16x16/apps]\nSize=16\nType=Fixed\n\n"
		"[scalable]\nSize=48\nMinSize=24\nMaxSize=256\nType=Scalable\n", f);
	fclose(f);
	char v[4096];
	snprintf(v, sizeof(v), "%s/data", root);
	setenv("XDG_DATA_HOME", v, 1);
	snprintf(v, sizeof(v), "%s/none", root);
	setenv("XDG_DATA_DIRS", v, 1);
	snprintf(v, sizeof(v), "%s/home", root);
	setenv("HOME", v, 1);

	list_t *themes, *basedirs;
	init_themes(&themes, &basedirs);
	struct icon_theme *t = NULL;
	for (int i = 0; i < themes->length; i++) {
		struct icon_theme *c = themes->items[i];
		if (c->name && strcmp(c->name, "TestTheme") == 0) t = c;
	}
	check("icon_theme_loaded", t != NULL);
	check("icon_theme_comment", t && t->comment && strcmp(t->comment, "known answer") == 0);
	check("icon_theme_subdirs", t && t->subdirs->length == 2);
	struct icon_theme_subdir *sd = t && t->subdirs->length == 2 ? t->subdirs->items[1] : NULL;
	check("icon_theme_scalable_subdir", sd && sd->type == SCALABLE
		&& sd->min_size == 24 && sd->max_size == 256);
	int min = 0, max = 0;
	char *icon = find_icon(themes, basedirs, "app", 16, "TestTheme", &min, &max);
	check("icon_lookup_fixed", icon && strstr(icon, "/t/16x16/apps/app.png") && min == 16 && max == 16);
	free(icon);
	icon = find_icon(themes, basedirs, "big", 100, "TestTheme", &min, &max);
	check("icon_lookup_scalable", icon && strstr(icon, "/t/scalable/big.png") && min == 24 && max == 256);
	free(icon);
	icon = find_icon(themes, basedirs, "missing", 16, "TestTheme", &min, &max);
	check("icon_lookup_missing", icon == NULL);
	int bad = 0;
	for (int i = 0; i < themes->length; i++) {
		struct icon_theme *c = themes->items[i];
		bad += c->name && strcmp(c->name, "Bad") == 0;
	}
	check("icon_theme_bad_name_rejected", bad == 0);
	free(icon);
	finish_themes(themes, basedirs);
}

static void test_swaynag(const char *root) {
	char path[4096];
	snprintf(path, sizeof(path), "%s/swaynag.conf", root);
	FILE *f = fopen(path, "w");
	fputs("edge=bottom\nbackground=#102030\n[warning]\nbackground=#ffc100ff\n"
		"text=000000\nbutton-gap=7\nlayer=overlay\n[custom]\nborder-bottom-size=9\n", f);
	fclose(f);
	struct swaynag swaynag;
	memset(&swaynag, 0, sizeof(swaynag));
	list_t *types = create_list();
	swaynag_types_add_default(types);
	swaynag.buttons = create_list();
	struct swaynag_button *close_btn = calloc(1, sizeof(*close_btn));
	close_btn->text = strdup("X");
	list_add(swaynag.buttons, close_btn);
	int rc = swaynag_load_config(path, &swaynag, types);
	check("swaynag_config_ok", rc == 0);
	struct swaynag_type *cfg = swaynag_type_get(types, "<config>");
	check("swaynag_general_background", cfg && cfg->background == 0x102030ff);
	check("swaynag_general_edge", cfg && cfg->anchors ==
		(ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
		| ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT));
	struct swaynag_type *w = swaynag_type_get(types, "warning");
	check("swaynag_type_background", w && w->background == 0xffc100ff);
	check("swaynag_type_text", w && w->text == 0x000000ff);
	check("swaynag_type_button_gap", w && w->button_gap == 7);
	check("swaynag_type_layer", w && w->layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
	struct swaynag_type *c = swaynag_type_get(types, "custom");
	check("swaynag_new_type", c && c->bar_border_thickness == 9);

	snprintf(path, sizeof(path), "%s/bad.conf", root);
	f = fopen(path, "w");
	fputs("[broken\n", f);
	fclose(f);
	check("swaynag_malformed_rejected", swaynag_load_config(path, &swaynag, types) != 0);
	swaynag_types_free(types);
}

int main(void) {
	sway_log_init(SWAY_SILENT, NULL);
	signal(SIGPIPE, SIG_IGN);
	bar.eventloop = loop_create();
	const char *tmp = getenv("TMPDIR");
	char tmpl[4096];
	snprintf(tmpl, sizeof(tmpl), "%s/sway-selftest.XXXXXX", tmp && *tmp ? tmp : "/tmp");
	char *root = mkdtemp(tmpl);
	if (!root) {
		perror("mkdtemp");
		return 2;
	}
	test_status_line();
	test_icon_theme(root);
	test_swaynag(root);
	return failures ? 1 : 0;
}
