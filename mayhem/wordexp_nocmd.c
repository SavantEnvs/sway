// Link-time environment guard for fuzz_sway_config (linked with -Wl,--wrap=wordexp).
//
// sway expands `include <path>` (sway/config.c load_include_configs) and the paths of
// `output ... background`, `output ... color_profile` and `input ... xkb_file`
// (common/stringop.c expand_path) with wordexp(..., 0). Without WRDE_NOCMD, libc runs
// `$(...)` and backticks through /bin/sh, so a fuzzer input could create or delete files,
// spawn processes, signal the fuzzer or sleep: effects that outlive the input. Running
// a shell command from the user's own config is sway's intended behaviour, not a bug,
// so nothing is hidden by refusing it here. Tilde, variable and glob expansion still
// run; a command substitution makes wordexp return WRDE_CMDSUB, an error path sway
// already handles (the include or path is skipped / reported as invalid).
#include <wordexp.h>

int __real_wordexp(const char *words, wordexp_t *p, int flags);

int __wrap_wordexp(const char *words, wordexp_t *p, int flags) {
	return __real_wordexp(words, p, flags | WRDE_NOCMD);
}
