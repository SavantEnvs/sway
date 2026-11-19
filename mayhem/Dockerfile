# syntax=docker/dockerfile:1.7
#
# Commit-image Dockerfile (SMALL form, PREFERRED) — builds ON TOP of the org C/C++ base
# image (ghcr.io/savantenvs/base). The base already provides debian + clang/llvm +
# libFuzzer/afl++ + the identities + the build-contract ENV, so this only adds the
# per-commit build (built on top of the prebuilt base, rather than building the toolchain in-image).
#
# Build (from the repo root):
#   DOCKER_BUILDKIT=1 docker build -f mayhem/Dockerfile \
#       --build-arg COMMIT=$(git rev-parse HEAD) \
#       [--build-arg SOURCE_DATE_EPOCH=$(date +%s)] \
#       -t mytag .

# The org base, pinned to a fixed DATE-tagged base (YYYYMMDD; shared by all integrations
# minted against it). Use the current dated tag; pin by digest in production: ...base@sha256:<digest>.
FROM ghcr.io/savantenvs/base:20261023

ARG COMMIT
ARG REPO_URL
# SOURCE_DATE_EPOCH stays an ARG (available to build.sh); do NOT bake it as an ENV — an
# empty value breaks clang ("must be a non-negative decimal integer") both here and for
# any in-image rebuild. build.sh unsets it when empty.
ARG SOURCE_DATE_EPOCH

# Sanitizer override: the base exports SANITIZER_FLAGS (ASan+UBSan, halting) as the default.
# To change it for a build (e.g. ASan-only, or add MSan): --build-arg SANITIZER_FLAGS="...".
# Declare it WITHOUT a default (no '='): an undefaulted ARG lets the base ENV flow through to
# build.sh when not passed, and an explicit --build-arg overrides it. (An empty default — `=` —
# would instead shadow the base ENV and silently wipe the sanitizers.)
ARG SANITIZER_FLAGS
# Coverage (optional, off by default): instrument the TEST build to measure how much of the project
# the suite exercises — --build-arg COVERAGE_FLAGS="-fprofile-instr-generate -fcoverage-mapping".
# Empty by default (no effect on the normal oracle build); build.sh appends it to the test build only.
ARG COVERAGE_FLAGS=
# Build parallelism (optional). build.sh falls back to nproc when unset: --build-arg MAYHEM_JOBS=8
ARG MAYHEM_JOBS=

# Build dependencies of the swaybar/swaynag/common sources the harnesses compile (Debian packages,
# re-pinned to SOURCE_DATE_EPOCH when set): json-c, cairo/pango, wayland client + scanner +
# protocol XMLs, libsystemd (swaybar tray sd-bus provider), pkg-config.
USER root
RUN [ -n "$SOURCE_DATE_EPOCH" ] && repro-sources-list.sh || true ; \
    apt-get update && apt-get install -yq --no-install-recommends \
        libjson-c-dev libpango1.0-dev libcairo2-dev libwayland-dev libwayland-bin \
        wayland-protocols libsystemd-dev pkgconf \
        meson ninja-build git ca-certificates libpcre2-dev libevdev-dev libudev-dev libinput-dev \
        libseat-dev libxml2-dev libffi-dev libexpat1-dev hwdata libdisplay-info-dev xkb-data \
 && rm -rf /var/lib/apt/lists/*

# The compositor (sway/) needs wlroots >=0.21,<0.22, which is only on wlroots' main branch, plus
# newer wayland, wayland-protocols, xkbcommon, pixman and libdrm than Debian ships. Build them once,
# pinned, from wlroots' own meson wraps into /opt/toolchains/deps (SPEC 6.2 item 19); build.sh then
# finds them via pkg-config and runs offline. Uninstrumented (they are not the fuzz target), with
# DWARF 3 debug info; only the headless backend + pixman renderer are used at run time.
ARG WLROOTS_REV=7b200fc35230db3136f9f21eb277051ad98cb169
ENV SWAY_DEPS=/opt/toolchains/deps/wlroots-0.21.0-dev
RUN set -eux; D=$SWAY_DEPS; \
    git clone -q https://gitlab.freedesktop.org/wlroots/wlroots.git $D/src; \
    cd $D/src; git checkout -q $WLROOTS_REV; \
    pin() { sed -i "s/^revision = .*/revision = $2/" subprojects/$1.wrap; }; \
    pin wayland f2ecaa61a3a82944ae069de5413104ab7ba2109f; \
    pin wayland-protocols 3eab2dccf4bfdd85c16d312a6c8c8fbc2a6e73a8; \
    pin libdrm 33fab8cbca7f7581e955a675597d14412702edc4; \
    pin libxkbcommon 15b2b4e47a350712c0ebd120a100c95a7c17e04e; \
    pin pixman b88b92e512a33a222e08352e87ff90a57d33c7b5; \
    pin libdisplay-info af382cf311ce60983708a9e664728a61843143e7; \
    CC=clang CFLAGS="-O1 -g -gdwarf-3" meson setup build --prefix=$D --libdir=lib --buildtype=plain \
      --wrap-mode=default -Dexamples=false -Dxwayland=disabled -Dbackends=libinput \
      -Drenderers=[] -Dallocators=[] -Dsession=enabled \
      -Dwayland:documentation=false -Dwayland:tests=false \
      -Dlibdrm:werror=false -Dlibdrm:nouveau=disabled -Dlibdrm:intel=disabled \
      -Dlibdrm:radeon=disabled -Dlibdrm:amdgpu=disabled -Dlibdrm:vmwgfx=disabled -Dlibdrm:tests=false \
      -Dlibxkbcommon:enable-docs=false -Dlibxkbcommon:enable-x11=false -Dlibxkbcommon:xkb-config-root=/usr/share/X11/xkb -Dlibxkbcommon:x-locale-root=/usr/share/X11/locale \
      -Dpixman:tests=disabled -Dpixman:demos=disabled; \
    ninja -C build install; \
    test -f $D/lib/pkgconfig/wlroots-0.21.pc
USER mayhem

# Source enters from the build CONTEXT (the CI checkout): the FULL repo (incl. .git
# history) is copied to /mayhem as `mayhem`; build.sh builds in place.
USER mayhem
WORKDIR /mayhem
COPY --chown=2000:2000 . /mayhem

# Anything a Dockerfile step materializes into the tree (downloaded fixtures, generated headers,
# vendored deps) MUST be committed or (re)created by build.sh: rlenv's patch grader runs
# `git clean -ffdX` before every graded build, so a gitignored, untracked file is gone by then and
# every honest patch scores zero (SPEC §6.2 item 16; verify-repo.sh checks it).
#
# OPTIONAL compile-cache pre-fill (SPEC §6.2 item 18). Bases minted with /opt/mayhem/ccache/prefill
# run the build under rlenv's pinned ccache, so the patch grader's first (otherwise cold) rebuild is
# warm — this is what keeps large C/C++ targets inside the build time limit. Older bases fall back to
# the plain build; a missing or stale pre-fill only costs a cold cache. Never enable ccache in
# build.sh or via ENV/PATH, and keep the compiler resolving by name (CC=clang) or nothing is cached.
RUN if [ -x /opt/mayhem/ccache/prefill ]; then /opt/mayhem/ccache/prefill bash mayhem/build.sh; \
    else bash mayhem/build.sh; fi

# Run the project's functional tests at build time IF the repo ships them (mayhem/test.sh is
# optional — omit it only when the repo has no tests meaningful for RL). A failing suite fails the
# image build, so the commit image is guaranteed to build AND pass its own tests.
RUN if [ -f mayhem/test.sh ]; then bash mayhem/test.sh; else echo "no mayhem/test.sh — repo ships no functional tests"; fi

LABEL org.opencontainers.image.source="${REPO_URL}" \
      org.opencontainers.image.revision="${COMMIT}" \
      rlenv.tier="commit" \
      rlenv.source_date_epoch="${SOURCE_DATE_EPOCH}" \
      rlenv.user="mayhem"

USER mayhem
WORKDIR /mayhem
