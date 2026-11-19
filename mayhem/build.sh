#!/usr/bin/env bash
#
# mayhem/build.sh — build sway's fuzz harnesses and the parser self-test.
#
# Runs inside the commit image (mayhem/Dockerfile) as `mayhem` in /mayhem.
#
# What is built: the parsers of sway's helper programs that read untrusted data
# (swaybar's status_command reader + i3bar JSON, swaybar's tray icon-theme loader,
# swaybar's IPC/bar-config JSON handling, swaynag's config file), compiled from
# upstream's own sources with meson's project arguments, into libsway-fuzz.a. These files
# need only common/, json-c, cairo/pango and generated Wayland client headers; none
# of them links wlroots. The compositor (sway/) is built with meson against the wlroots
# 0.21 chain the Dockerfile pre-builds under /opt/toolchains/deps (fuzz_sway_config).
#
# Outputs (in $SRC):
#   fuzz_<name>, fuzz_<name>-standalone   per harness in mayhem/harnesses/
#   mayhem/build/selftest                 known-answer test run by mayhem/test.sh
# Intermediate files go to mayhem/build/ (gitignored by upstream's `build/` rule).
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${SRC:=/mayhem}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"
B="$SRC/mayhem/build"
mkdir -p "$B/gen" "$B/include" "$B/fuzz" "$B/test"

# 1) Generated files meson would produce: config.h (at <builddir>/include/config.h, which
#    upstream headers reach as "../include/config.h") and the Wayland client headers +
#    protocol code (wayland-scanner, exactly as protocols/meson.build runs it).
cat > "$B/gen/config.h.tmp" <<'EOF'
#define HAVE_GDK_PIXBUF 0
#define HAVE_LIBSYSTEMD 1
#define HAVE_LIBELOGIND 0
#define HAVE_BASU 0
#define HAVE_TRAY 1
EOF
# = meson -Dtray=enabled -Dsd-bus-provider=libsystemd -Dgdk-pixbuf=disabled
cmp -s "$B/gen/config.h.tmp" "$B/include/config.h" || mv "$B/gen/config.h.tmp" "$B/include/config.h"
rm -f "$B/gen/config.h.tmp"

WLP="$(pkg-config --variable=pkgdatadir wayland-protocols)"
PROTOS=(
  "$WLP/stable/tablet/tablet-v2.xml"
  "$WLP/stable/xdg-shell/xdg-shell.xml"
  "$WLP/staging/cursor-shape/cursor-shape-v1.xml"
  "$WLP/unstable/xdg-output/xdg-output-unstable-v1.xml"
  "$SRC/protocols/wlr-layer-shell-unstable-v1.xml"
)
PROTO_C=()
for xml in "${PROTOS[@]}"; do
  base="$(basename "$xml" .xml)"
  wayland-scanner client-header "$xml" "$B/gen/$base-client-protocol.h"
  wayland-scanner private-code "$xml" "$B/gen/$base-protocol.c"
  PROTO_C+=("$B/gen/$base-protocol.c")
done

# meson.build's project arguments for the C sources.
PROJ_DEFS=(-std=c11 -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L -D_C11_SOURCE
  '-DSYSCONFDIR="/etc"' '-DSWAY_VERSION="1.13-dev"' -Wno-unused-parameter)
PKGS="json-c pangocairo cairo pango wayland-client wayland-cursor libsystemd"
INC=(-I"$SRC/include" -I"$B/include" -I"$B/gen")
read -r -a PKG_CFLAGS <<< "$(pkg-config --cflags $PKGS)"
read -r -a PKG_LIBS <<< "$(pkg-config --libs $PKGS)"

# The upstream sources under test (each is compiled exactly as meson would): lib_sway_common,
# lib_sway_client, swaybar without main.c, swaynag's config parser.
LIB_SRCS=(
  common/list.c common/log.c common/loop.c common/stringop.c common/util.c
  common/ipc-client.c common/cairo.c common/pango.c
  client/pool-buffer.c
  swaybar/bar.c swaybar/config.c swaybar/i3bar.c swaybar/image.c swaybar/input.c
  swaybar/ipc.c swaybar/render.c swaybar/status_line.c
  swaybar/tray/host.c swaybar/tray/icon.c swaybar/tray/item.c swaybar/tray/tray.c
  swaybar/tray/watcher.c
  swaynag/config.c swaynag/types.c
)
# swaymsg's main.c, with main renamed so a harness can drive it (no behavior change).
SWAYMSG_SRC=swaymsg/main.c

# build_lib <outdir> <cflags...>: compile LIB_SRCS + protocol code into <outdir>/libsway-fuzz.a
build_lib() {
  local out="$1"; shift
  local objs=() pids=()
  for s in "${LIB_SRCS[@]}" "${PROTO_C[@]}"; do
    local o="$out/$(echo "${s#$SRC/}" | tr '/' '_').o"
    "$CC" "$@" "${PROJ_DEFS[@]}" "${INC[@]}" "${PKG_CFLAGS[@]}" -c "$s" -o "$o" &
    pids+=($!)
    objs+=("$o")
    while [ "$(jobs -rp | wc -l)" -ge "$MAYHEM_JOBS" ]; do sleep 0.1; done
  done
  local p
  for p in "${pids[@]}"; do wait "$p"; done
  "$CC" "$@" "${PROJ_DEFS[@]}" "${INC[@]}" "${PKG_CFLAGS[@]}" -Dmain=swaymsg_main \
    -c "$SWAYMSG_SRC" -o "$out/swaymsg_main.c.o"
  objs+=("$out/swaymsg_main.c.o")
  rm -f "$out/libsway-fuzz.a"
  ar rcs "$out/libsway-fuzz.a" "${objs[@]}"
}

# 2) The project, sanitized + instrumented for fuzzing.
# shellcheck disable=SC2086
build_lib "$B/fuzz" $SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link -ffunction-sections

# LeakSanitizer off for every ASan binary (leaks are not the bug class fuzzed here).
# shellcheck disable=SC2086
"$CC" $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.c" -o "$B/fuzz/lsan_off.o"
# shellcheck disable=SC2086
"$CC" $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$B/fuzz/standalone_main.o"
# wordexp() with WRDE_NOCMD for fuzz_sway_config: config inputs never run shell commands
# (`include $(...)`); see mayhem/wordexp_nocmd.c.
# shellcheck disable=SC2086
"$CC" $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/wordexp_nocmd.c" -o "$B/fuzz/wordexp_nocmd.o"

# 2b) The compositor itself (sway/), with meson exactly as upstream builds it, sanitized and
#     instrumented, against the wlroots chain pre-built in the image under $SWAY_DEPS (Dockerfile).
#     Offline: --wrap-mode=nodownload, every dependency is resolved by pkg-config. The harness links
#     the `sway` executable's objects, with main() renamed in main.c.o (objcopy, no source change).
: "${SWAY_DEPS:=/opt/toolchains/deps/wlroots-0.21.0-dev}"
SB="$B/compositor"
export PKG_CONFIG_PATH="$SWAY_DEPS/lib/pkgconfig:$SWAY_DEPS/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
if [ ! -f "$SB/build.ninja" ]; then
  # shellcheck disable=SC2086
  CFLAGS="$SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link" LDFLAGS="$SANITIZER_FLAGS" \
    meson setup "$SB" "$SRC" --buildtype=plain --wrap-mode=nodownload -Db_lundef=false \
      -Dwerror=false -Dman-pages=disabled -Dtray=enabled -Dsd-bus-provider=libsystemd \
      -Dgdk-pixbuf=disabled -Dswaybar=false -Dswaynag=false -Ddefault-wallpaper=false \
      -Dzsh-completions=false -Dbash-completions=false -Dfish-completions=false
fi
ninja -C "$SB" -j "$MAYHEM_JOBS" sway/sway
SWAY_OBJS=()
for o in "$SB"/sway/sway.p/*.o; do
  case "$o" in
    */main.c.o) objcopy --redefine-sym main=sway_main "$o" "$B/fuzz/sway_main_renamed.o"
                     SWAY_OBJS+=("$B/fuzz/sway_main_renamed.o") ;;
    *) SWAY_OBJS+=("$o") ;;
  esac
done
# The link line meson uses for the sway executable (its dependency libraries).
read -r -a SWAY_LIBS <<< "$(awk '/^build sway\/sway: /{f=1} f&&/LINK_ARGS/{sub(/^ *LINK_ARGS = /,""); print; exit}' "$SB/build.ninja")"
SWAY_INC=(-I"$SRC/include" -I"$SB/include" -I"$SB/protocols" -I"$SB")
read -r -a SWAY_CFLAGS <<< "$(pkg-config --cflags wlroots-0.21 json-c pixman-1 xkbcommon wayland-server libinput pangocairo)"

for h in "$SRC"/mayhem/harnesses/fuzz_*.c; do
  name="$(basename "$h" .c)"
  if [ "$name" = fuzz_sway_config ]; then
    # shellcheck disable=SC2086
    "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS "${PROJ_DEFS[@]}" "${SWAY_INC[@]}" "${SWAY_CFLAGS[@]}" \
      -c "$h" -o "$B/fuzz/$name.o"
    for mode in fuzz standalone; do
      if [ $mode = fuzz ]; then drv=($LIB_FUZZING_ENGINE); out="$SRC/$name"
      else drv=("$B/fuzz/standalone_main.o"); out="$SRC/$name-standalone"; fi
      # shellcheck disable=SC2086
      # (run from the meson build dir: its LINK_ARGS name build-relative paths)
      (cd "$SB" && "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS "${drv[@]}" "$B/fuzz/$name.o" "$B/fuzz/lsan_off.o" \
        "$B/fuzz/wordexp_nocmd.o" -Wl,--wrap=wordexp \
        "${SWAY_OBJS[@]}" "${SWAY_LIBS[@]}" "$SB/common/libsway-common.a" \
        -Wl,-rpath,"$SWAY_DEPS/lib" -lm -lpthread -o "$out")
    done
    continue
  fi
  # shellcheck disable=SC2086
  "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS "${PROJ_DEFS[@]}" "${INC[@]}" "${PKG_CFLAGS[@]}" \
    -c "$h" -o "$B/fuzz/$name.o"
  # shellcheck disable=SC2086
  "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$B/fuzz/$name.o" "$B/fuzz/lsan_off.o" \
    "$B/fuzz/libsway-fuzz.a" "${PKG_LIBS[@]}" -lm -lpthread -Wl,--gc-sections -o "$SRC/$name"
  # shellcheck disable=SC2086
  "$CC" $SANITIZER_FLAGS $DEBUG_FLAGS "$B/fuzz/standalone_main.o" "$B/fuzz/$name.o" "$B/fuzz/lsan_off.o" \
    "$B/fuzz/libsway-fuzz.a" "${PKG_LIBS[@]}" -lm -lpthread -Wl,--gc-sections -o "$SRC/$name-standalone"
done

# 3) The known-answer self-test, with normal flags (no sanitizers), run by mayhem/test.sh.
# shellcheck disable=SC2086
build_lib "$B/test" -O1 -g -ffunction-sections $COVERAGE_FLAGS
# shellcheck disable=SC2086
"$CC" -O1 -g $COVERAGE_FLAGS "${PROJ_DEFS[@]}" "${INC[@]}" "${PKG_CFLAGS[@]}" \
  "$SRC/mayhem/tests/selftest.c" "$B/test/libsway-fuzz.a" "${PKG_LIBS[@]}" -lm -Wl,--gc-sections -o "$B/selftest"

echo "build.sh: done"
