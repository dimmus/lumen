#!/usr/bin/env bash
# Install what Lumen needs from the distribution's own repositories, check the toolchain, and
# fetch the upstream protocol XML.
#
#   ./scripts/install-deps.sh             # build and run
#   ./scripts/install-deps.sh dev         # also develop and test
#   ./scripts/install-deps.sh dev -n      # print what it would install, change nothing
#   ./scripts/install-deps.sh --check     # only check the toolchain and libraries
#
# Default set — what building and running a session needs: Clang with libstdc++ 16
# (std::inplace_vector; `import std;` is built from its bits/std.cc) and clang-scan-deps,
# CMake 3.28+, Ninja, pkg-config, git and Python 3.11+ (tomllib — tomli on an older Python) for
# fetch-protocols.sh and protocol_manifest.py, glslc, every library cmake/LumenPlatform.cmake
# looks for, and what a session runs on: Mesa's EGL/GBM, the Vulkan driver of the GPUs found in
# /sys/class/drm, xkeyboard-config, a fallback font, the icon themes the shell looks in (Adwaita,
# hicolor) and foot, run-desktop.sh's first client.
#
# dev adds: compiler-rt (the asan/tsan presets, LUMEN_BUILD_FUZZERS), clang-tidy and
# clang-format, the Vulkan validation layer and lavapipe (test_ui_golden_vk_validation, the
# Vulkan tests without a GPU), libpipewire (lumen-portal-probe), the clients and tools the smokes
# drive (alacritty, gtk4-demo, gtk4-widget-factory, weston's demo clients, vkcube, wev, grim,
# wl-clipboard, cliphist, wf-recorder, ffmpeg, wlr-randr, kanshi, notify-send, gdbus), the portals
# with PipeWire and WirePlumber, a colour emoji font, heaptrack, gdb, strace, perf and ccache.
#
# There is no C++ TOML library to install: lx.config reads config.toml itself. The only TOML
# reader the build uses is Python's (protocols/manifest.toml).
#
# Packages a release does not have are reported and skipped; a toolchain that cannot build
# Lumen is an error (exit 1), with what to do about it. Package installation runs through sudo
# unless already root; fetch-protocols.sh runs as the invoking user, also under sudo.
#
# Options:
#   dev, --dev        the development set as well
#   -n, --dry-run     print the package manager command; install, check and fetch nothing
#   -y, --yes         do not ask for confirmation (the package manager's own -y/--noconfirm)
#   --no-fetch        do not run scripts/fetch-protocols.sh
#   --check           only the toolchain and library check (no install, no fetch)
#   --distro ID       skip /etc/os-release detection: arch, debian, ubuntu, fedora, opensuse
#
# Supported: Arch and derivatives (pacman), Debian testing/unstable, Ubuntu and derivatives (apt),
# Fedora (dnf), openSUSE Tumbleweed (zypper). libstdc++ 16 decides how old a release can be:
# Fedora 44+, Ubuntu 26.04+, Debian testing (forky). On an older apt release the script stops
# before installing anything; elsewhere the toolchain check fails.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEV=0
DRY=0
YES=0
FETCH=1
CHECK_ONLY=0
DISTRO=""

while (( $# > 0 )); do
  case "$1" in
    dev|--dev) DEV=1 ;;
    -n|--dry-run) DRY=1 ;;
    -y|--yes) YES=1 ;;
    --no-fetch) FETCH=0 ;;
    --check) CHECK_ONLY=1 ;;
    --distro) DISTRO="${2:?--distro needs an ID}"; shift ;;
    -h|--help) sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (see --help)"; exit 2 ;;
  esac
  shift
done

say()  { printf '==> %s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
fail() { printf 'FAIL: %s\n' "$*" >&2; FAILED=1; }
FAILED=0

# ── Distribution ─────────────────────────────────────────────────────────────
# ID first, then ID_LIKE, so a derivative (Manjaro, Mint, Nobara) takes its base's path.
detect_family() {
  local ids="" id
  if [[ -n "$DISTRO" ]]; then
    ids="$DISTRO"
  elif [[ -r /etc/os-release ]]; then
    ids="$(. /etc/os-release; echo "${ID:-} ${ID_LIKE:-}")"
  fi
  for id in $ids; do
    case "$id" in
      arch) echo arch; return ;;
      ubuntu) echo ubuntu; return ;;
      debian) echo debian; return ;;
      fedora) echo fedora; return ;;
      opensuse*|suse) echo opensuse; return ;;
    esac
  done
  echo unknown
}
FAMILY="$(detect_family)"

# ── What Lumen looks for (cmake/LumenPlatform.cmake) ────────────────────────
# Without a required module the session does not work (no input, no display, no text); an
# optional one costs the feature named next to it.
PC_REQUIRED=(wayland-server wayland-client wayland-scanner libdrm libinput libudev xkbcommon
             egl glesv2 gbm harfbuzz freetype2 fontconfig libsystemd)
declare -A PC_OPTIONAL=(
  [vulkan]="the Vulkan backend (the default on hardware with a Vulkan driver)"
  [libdisplay-info]="EDID make/model/serial beyond the base block"
  [fribidi]="bidirectional text (UAX #9)"
  [libunibreak]="line breaking and grapheme clusters (UAX #14, #29)"
  [libpng]="PNG icons and test_ui_golden"
  [librsvg-2.0]="SVG icons"
  [libturbojpeg]="JPEG backgrounds (needs libjpeg-turbo 3.0+)"
  [libwebp]="WebP backgrounds"
)
PC_ALL=("${PC_REQUIRED[@]}" "${!PC_OPTIONAL[@]}")
# The commands the smokes run: checked for by --check dev, and installed by path on Fedora, whose
# repositories index /usr/bin (no package name to guess).
DEV_COMMANDS=(vkcube alacritty gtk4-demo gtk4-widget-factory weston weston-simple-egl
              weston-simple-shm weston-simple-timing weston-constraints wev grim wl-copy
              cliphist wf-recorder ffprobe wlr-randr kanshi notify-send gdbus heaptrack gdb
              strace perf ccache)

# The GPU vendors whose Mesa Vulkan driver to install: Intel, AMD, and NVIDIA when nouveau
# drives it (NVIDIA's own driver brings its own Vulkan, EGL and GBM). vmwgfx and virtio have
# none: those take the GL path.
gpu_vendors() {
  local d vendor driver
  for d in /sys/class/drm/card*; do
    [[ "${d##*/}" =~ ^card[0-9]+$ && -r "$d/device/vendor" ]] || continue
    vendor="$(<"$d/device/vendor")"
    driver="$(basename "$(readlink -f "$d/device/driver" 2>/dev/null)" 2>/dev/null)"
    case "$vendor" in
      0x8086) echo intel ;;
      0x1002) echo amd ;;
      0x10de) [[ "$driver" == nouveau ]] && echo nouveau \
                || warn "NVIDIA GPU on the '$driver' driver: its Vulkan/EGL/GBM come with that driver" ;;
    esac
  done | sort -u
}
VENDORS="$(gpu_vendors)"

PKGS=()        # what to ask the package manager for
SKIPPED=()     # names this release does not have
CXX_HINT=""    # the compilers to configure with, when plain clang++ is not the one installed
CC_HINT=""

# ── Arch ─────────────────────────────────────────────────────────────────────
plan_arch() {
  local v
  PKGS=(clang gcc cmake ninja pkgconf git python shaderc
        wayland libdrm libdisplay-info libinput systemd-libs libxkbcommon libglvnd mesa
        harfbuzz freetype2 fontconfig fribidi libunibreak libpng librsvg libjpeg-turbo libwebp
        vulkan-headers vulkan-icd-loader
        xkeyboard-config ttf-dejavu hicolor-icon-theme adwaita-icon-theme foot)
  for v in $VENDORS; do
    case "$v" in
      intel) PKGS+=(vulkan-intel) ;;
      amd) PKGS+=(vulkan-radeon) ;;
      nouveau) PKGS+=(vulkan-nouveau) ;;
    esac
  done
  if (( DEV )); then
    # clang-tidy and clang-format are in clang.
    PKGS+=(compiler-rt vulkan-validation-layers vulkan-swrast vulkan-tools libpipewire
           pipewire wireplumber xdg-desktop-portal xdg-desktop-portal-wlr xdg-desktop-portal-gtk
           alacritty gtk4-demos weston wev grim wl-clipboard cliphist wf-recorder ffmpeg
           wlr-randr kanshi libnotify glib2 noto-fonts-emoji heaptrack gdb strace perf ccache)
  fi
  python_needs_tomli && PKGS+=(python-tomli)
  local p have=()
  for p in "${PKGS[@]}"; do
    if pacman -Si "$p" >/dev/null 2>&1; then have+=("$p"); else SKIPPED+=("$p"); fi
  done
  PKGS=("${have[@]}")
}
install_arch() {
  local args=(-S --needed)
  (( YES )) && args+=(--noconfirm)
  run_root pacman "${args[@]}" "${PKGS[@]}"
}

# ── Debian, Ubuntu ───────────────────────────────────────────────────────────
# Not `| grep -q`: grep stops at the first match, and pipefail fails on apt-cache's SIGPIPE.
apt_has() { [[ "$(apt-cache policy "$1" 2>/dev/null)" =~ Candidate:\ [^\(] ]]; }
# The highest N for which package PREFIX<N>SUFFIX can be installed.
apt_newest() {
  local name n
  for n in $(apt-cache pkgnames "$1" 2>/dev/null | while read -r name; do
               name="${name#"$1"}"; name="${name%"$2"}"
               [[ "$name" =~ ^[0-9]+$ ]] && echo "$name"
             done | sort -rn); do
    apt_has "$1$n$2" && { echo "$n"; return; }
  done
}
plan_debian() {
  if [[ -z "$(ls -A /var/lib/apt/lists 2>/dev/null | grep -v -e lock -e partial)" ]]; then
    if (( DRY )); then
      warn "no apt package lists: run apt-get update for an accurate dry run"
    else
      run_root apt-get update || return 1
    fi
  fi
  # Versioned toolchain: the newest Clang and libstdc++ the release has, not its defaults.
  local llvm gcc
  llvm="$(apt_newest clang- '')"
  gcc="$(apt_newest libstdc++- -dev)"
  if [[ -z "$llvm" ]]; then
    fail "no clang-N package in the apt sources"; return 1
  fi
  # Too old a release to build Lumen: say so before installing a toolchain that cannot.
  if [[ -z "$gcc" ]] || (( gcc < 16 )); then
    fail "newest libstdc++ in the apt sources is ${gcc:-none}; Lumen needs libstdc++ 16 (GCC 16):" \
         "Ubuntu 26.04+, Debian testing"
    return 1
  fi
  PKGS=("clang-$llvm" "clang-tools-$llvm" "libstdc++-$gcc-dev" cmake ninja-build pkgconf git
        python3 ca-certificates glslc
        libwayland-dev libwayland-bin libdrm-dev libdisplay-info-dev libinput-dev libudev-dev
        libxkbcommon-dev libegl-dev libgles-dev libgbm-dev libharfbuzz-dev libfreetype-dev
        libfontconfig-dev libfribidi-dev libunibreak-dev libsystemd-dev libpng-dev
        librsvg2-dev libturbojpeg0-dev libwebp-dev libvulkan-dev
        libegl-mesa0 libgbm1 libgl1-mesa-dri mesa-vulkan-drivers xkb-data fonts-dejavu-core
        hicolor-icon-theme adwaita-icon-theme foot)
  if (( DEV )); then
    # mesa-vulkan-drivers has lavapipe; gtk-4-examples has gtk4-demo and gtk4-widget-factory.
    PKGS+=("libclang-rt-$llvm-dev" "clang-tidy-$llvm" "clang-format-$llvm"
           vulkan-validationlayers vulkan-tools libpipewire-0.3-dev pipewire wireplumber
           xdg-desktop-portal xdg-desktop-portal-wlr xdg-desktop-portal-gtk
           alacritty gtk-4-examples weston wev grim wl-clipboard cliphist wf-recorder ffmpeg
           wlr-randr kanshi libnotify-bin libglib2.0-bin fonts-noto-color-emoji
           heaptrack gdb strace ccache)
    [[ "$FAMILY" == ubuntu ]] && PKGS+=(linux-tools-generic) || PKGS+=(linux-perf)
  fi
  python_needs_tomli && PKGS+=(python3-tomli)
  local p have=()
  for p in "${PKGS[@]}"; do
    if apt_has "$p"; then have+=("$p"); else SKIPPED+=("$p"); fi
  done
  PKGS=("${have[@]}")
  # The presets configure with plain clang++ and the C compiler cc (the protocol glue), which
  # only the default version's `clang` package and gcc provide.
  local plain
  plain="$(clang++ -dumpversion 2>/dev/null | cut -d. -f1)"
  [[ "$plain" == "$llvm" ]] || { CXX_HINT="clang++-$llvm"; CC_HINT="clang-$llvm"; }
}
install_debian() {
  local args=(install --no-install-recommends)
  (( YES )) && args+=(-y)
  if (( YES )); then
    run_root env DEBIAN_FRONTEND=noninteractive apt-get "${args[@]}" "${PKGS[@]}"
  else
    run_root apt-get "${args[@]}" "${PKGS[@]}"
  fi
}

# ── Fedora, openSUSE: libraries by the pkg-config module they provide ──────
rpm_pkgconfig() {
  local m
  for m in "${PC_ALL[@]}"; do PKGS+=("pkgconfig($m)"); done
  (( DEV )) && PKGS+=("pkgconfig(libpipewire-0.3)" pipewire wireplumber
                      xdg-desktop-portal xdg-desktop-portal-wlr xdg-desktop-portal-gtk)
}
plan_fedora() {
  local c
  PKGS=(clang gcc libstdc++-devel cmake ninja-build pkgconf-pkg-config git python3
        ca-certificates vulkan-headers /usr/bin/clang-scan-deps /usr/bin/glslc
        mesa-dri-drivers mesa-vulkan-drivers xkeyboard-config dejavu-sans-fonts
        dejavu-sans-mono-fonts hicolor-icon-theme adwaita-icon-theme foot)
  rpm_pkgconfig
  if (( DEV )); then
    # dnf resolves /usr/bin paths (they are in the repositories' primary metadata), which
    # finds weston-demo and gtk4-devel-tools without naming them.
    for c in "${DEV_COMMANDS[@]}"; do PKGS+=("/usr/bin/$c"); done
    PKGS+=(compiler-rt clang-tools-extra vulkan-validation-layers google-noto-color-emoji-fonts)
  fi
  python_needs_tomli && PKGS+=(python3-tomli)
}
install_fedora() {
  # dnf reports what a release lacks and goes on.
  # No weak dependencies, as apt's --no-install-recommends: they bring ffmpeg and codecs.
  local args=(install --skip-unavailable --setopt=install_weak_deps=False)
  (( YES )) && args+=(-y)
  run_root dnf "${args[@]}" "${PKGS[@]}"
}
plan_opensuse() {
  # openSUSE ships GCC versions side by side, and Clang takes the newest one installed.
  # (No awk: the base image does not have it.)
  local gcc v
  gcc="$(zypper -q search -t package 'gcc*-c++' 2>/dev/null \
         | sed -nE 's/^[^|]*\| *gcc([0-9]+)-c\+\+ .*/\1/p' | sort -rn | head -1)"
  # clang has clang-scan-deps, shaderc glslc.
  PKGS=(clang ${gcc:+"gcc$gcc-c++"} gcc-c++ cmake ninja pkgconf git python3 ca-certificates
        shaderc vulkan-headers Mesa-dri xkeyboard-config dejavu-fonts hicolor-icon-theme
        adwaita-icon-theme foot)
  for v in $VENDORS; do
    case "$v" in
      intel) PKGS+=(libvulkan_intel) ;;
      amd) PKGS+=(libvulkan_radeon) ;;
      nouveau) PKGS+=(libvulkan_nouveau) ;;
    esac
  done
  rpm_pkgconfig
  # zypper does not resolve /usr/bin paths, so the smokes' commands by name. compiler-rt is
  # libclang_rt<N>, which clang pulls in. openSUSE's weston has no demo clients
  # (weston-simple-*); --check names what is missing.
  (( DEV )) && PKGS+=(clang-tools libvulkan_lvp vulkan-validationlayers vulkan-tools
                      alacritty gtk4-devel-tools weston wev grim wl-clipboard cliphist
                      wf-recorder ffmpeg wlr-randr kanshi libnotify-tools glib2-tools
                      google-noto-coloremoji-fonts heaptrack gdb strace perf ccache)
  python_needs_tomli && PKGS+=(python3-tomli)
}
install_opensuse() {
  # --ignore-unknown: a name the release lacks is reported, not fatal.
  local args=(--ignore-unknown)
  (( YES )) && args+=(--non-interactive)
  run_root zypper "${args[@]}" install --no-recommends "${PKGS[@]}"
}

# ── Helpers ──────────────────────────────────────────────────────────────────
python_needs_tomli() {
  command -v python3 >/dev/null || return 1
  ! python3 -c 'import tomllib' 2>/dev/null && ! python3 -c 'import tomli' 2>/dev/null
}
run_root() {
  if (( DRY )); then
    printf '  %s' "$([[ $EUID -eq 0 ]] || echo 'sudo ')"
    printf '%q ' "$@"
    printf '\n'
    return 0
  fi
  if [[ $EUID -eq 0 ]]; then "$@"; else sudo "$@"; fi
}
# Runs as the user who invoked the script, so nothing under the checkout ends up root's.
run_user() {
  if [[ $EUID -eq 0 && -n "${SUDO_USER:-}" ]]; then sudo -u "$SUDO_USER" -- "$@"; else "$@"; fi
}
version_ge() { [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" == "$2" ]]; }

# ── Toolchain and library check ──────────────────────────────────────────────
newest_versioned_clang() {
  compgen -c clang++- | sed -nE 's/^clang\+\+-([0-9]+)$/\1/p' | sort -rn | head -1
}
check() {
  local cxx="${CXX:-$CXX_HINT}" tmp ver major scan v m dir
  if [[ -z "$cxx" ]]; then
    cxx=clang++
    if ! command -v clang++ >/dev/null && [[ -n "$(newest_versioned_clang)" ]]; then
      cxx="clang++-$(newest_versioned_clang)"
      CXX_HINT="$cxx"
      CC_HINT="clang-${cxx#clang++-}"
    fi
  fi
  say "Checking the toolchain ($cxx)"
  if ! command -v "$cxx" >/dev/null; then
    fail "$cxx not found"
  else
    ver="$("$cxx" -dumpversion 2>/dev/null)"
    major="${ver%%.*}"
    [[ "$("$cxx" --version 2>/dev/null)" == *clang* ]] || fail "$cxx is not Clang"
    # Lumen is developed against Clang 23 (docs/architecture.md notes 23.1 specifics).
    [[ "$major" =~ ^[0-9]+$ ]] && (( major < 23 )) \
      && warn "Clang $ver: Lumen is developed with Clang 23; older releases are untested"
    tmp="$(mktemp -d)"
    printf '%s\n' '#include <inplace_vector>' '#include <print>' \
      'int main() { std::inplace_vector<int, 2> v; v.push_back(1); std::println("{}", v[0]); }' \
      > "$tmp/probe.cpp"
    if ! "$cxx" -std=c++26 -fsyntax-only "$tmp/probe.cpp" 2>"$tmp/err"; then
      fail "$cxx -std=c++26 cannot compile std::inplace_vector: Lumen needs libstdc++ 16+ (GCC 16)"
      sed -n '1,3s/^/      /p' "$tmp/err" >&2
    fi
    m="$("$cxx" -print-file-name=libstdc++.modules.json 2>/dev/null)"
    [[ "$m" == /* && -f "$m" ]] \
      || m="$("$cxx" -print-file-name=libc++.modules.json 2>/dev/null)"
    [[ "$m" == /* && -f "$m" ]] \
      || fail "no libstdc++.modules.json: \`import std;\` needs the C++ standard library's module source"
    # CMake's module scanner: next to the compiler or on PATH, versioned or not.
    dir="$(dirname "$(readlink -f "$(command -v "$cxx")")")"
    scan=""
    for v in "$dir/clang-scan-deps" "clang-scan-deps-$major" clang-scan-deps; do
      command -v "$v" >/dev/null && { scan="$v"; break; }
    done
    [[ -n "$scan" ]] || fail "clang-scan-deps not found (CMake needs it to build C++ modules)"
    if (( DEV )); then
      printf 'int main() { return 0; }\n' > "$tmp/san.c"
      "$cxx" -x c -fsanitize=address,undefined "$tmp/san.c" -o "$tmp/a" 2>/dev/null \
        || warn "-fsanitize=address does not link: no compiler-rt (asan preset)"
      "$cxx" -x c -fsanitize=thread "$tmp/san.c" -o "$tmp/a" 2>/dev/null \
        || warn "-fsanitize=thread does not link: no compiler-rt (tsan preset)"
      printf 'int LLVMFuzzerTestOneInput(const char*, unsigned long) { return 0; }\n' > "$tmp/fz.c"
      "$cxx" -x c -fsanitize=fuzzer "$tmp/fz.c" -o "$tmp/a" 2>/dev/null \
        || warn "-fsanitize=fuzzer does not link: no libFuzzer (LUMEN_BUILD_FUZZERS)"
    fi
    rm -rf "$tmp"
  fi

  # project() enables C too, for the wayland-scanner glue.
  command -v "${CC:-${CC_HINT:-cc}}" >/dev/null || fail "no C compiler (${CC:-${CC_HINT:-cc}})"
  if command -v cmake >/dev/null; then
    v="$(cmake --version | sed -nE '1s/.* ([0-9][0-9.]*).*/\1/p')"
    version_ge "$v" 3.28 || fail "CMake $v: Lumen needs 3.28+ (C++ modules)"
  else
    fail "cmake not found"
  fi
  command -v ninja >/dev/null || fail "ninja not found"
  command -v git >/dev/null || fail "git not found (fetch-protocols.sh)"
  if ! command -v python3 >/dev/null; then
    fail "python3 not found (protocol_manifest.py)"
  elif ! python3 -c 'import tomllib' 2>/dev/null && ! python3 -c 'import tomli' 2>/dev/null; then
    fail "python3 has neither tomllib (3.11+) nor tomli: protocol_manifest.py reads manifest.toml"
  fi
  command -v glslc >/dev/null || warn "glslc not found: the Vulkan composite pass is not built"

  say "Checking libraries (pkg-config)"
  if ! command -v pkg-config >/dev/null; then
    fail "pkg-config not found"
    return
  fi
  for m in "${PC_REQUIRED[@]}"; do
    pkg-config --exists "$m" || fail "$m missing"
  done
  for m in "${!PC_OPTIONAL[@]}"; do
    local spec="$m"
    [[ "$m" == libturbojpeg ]] && spec="libturbojpeg >= 3.0"
    pkg-config --exists "$spec" || warn "$m missing: no ${PC_OPTIONAL[$m]}"
  done
  if pkg-config --exists libsystemd; then
    v="$(pkg-config --modversion libsystemd)"
    version_ge "$v" 257 \
      || warn "libsystemd $v < 257: no sd-varlink, so no control socket and no lumen-ctl"
  fi
  if (( DEV )); then
    local f=""
    for dir in /usr/share/vulkan/explicit_layer.d /usr/local/share/vulkan/explicit_layer.d \
               /etc/vulkan/explicit_layer.d; do
      [[ -f "$dir/VkLayer_khronos_validation.json" ]] && f=1
    done
    [[ -n "$f" ]] || warn "no VkLayer_khronos_validation: test_ui_golden_vk_validation is not built"
    pkg-config --exists libpipewire-0.3 \
      || warn "libpipewire-0.3 missing: lumen-portal-probe cannot read ScreenCast streams"
    for v in "${DEV_COMMANDS[@]}" foot; do
      command -v "$v" >/dev/null || warn "$v not found: the smokes that use it skip or fail"
    done
  fi
}

# ── Main ─────────────────────────────────────────────────────────────────────
if (( ! CHECK_ONLY )); then
  case "$FAMILY" in
    arch) plan_arch ;;
    debian|ubuntu) plan_debian || exit 1 ;;
    fedora) plan_fedora ;;
    opensuse) plan_opensuse ;;
    *)
      echo "Unsupported distribution (${DISTRO:-see /etc/os-release}). Install these with your"
      echo "package manager, then run $0 --check:"
      echo "  clang (with clang-scan-deps), libstdc++ 16, cmake 3.28+, ninja, pkg-config, git,"
      echo "  python 3.11+, glslc, and the pkg-config modules: ${PC_ALL[*]}"
      exit 2
      ;;
  esac
  say "Installing for $FAMILY ($( (( DEV )) && echo 'build, run and development' || echo 'build and run' ))${VENDORS:+, Vulkan drivers for: ${VENDORS//$'\n'/ }}"
  (( ${#SKIPPED[@]} )) && warn "not in this release's repositories, skipped: ${SKIPPED[*]}"
  if ! "install_${FAMILY/ubuntu/debian}"; then
    echo "FAIL: the package manager did not finish (see above)" >&2
    [[ "$FAMILY" == arch ]] && echo "      if a download failed, the package database may be old: pacman -Syu" >&2
    exit 1
  fi
fi

if (( DRY )); then
  (( FETCH )) && printf '  %q\n' "$ROOT/scripts/fetch-protocols.sh"
  exit 0
fi

check
if (( FAILED )); then
  echo "The toolchain cannot build Lumen yet (FAIL lines above)." >&2
  exit 1
fi

if (( FETCH && ! CHECK_ONLY )); then
  say "Fetching upstream protocols"
  run_user "$ROOT/scripts/fetch-protocols.sh" || { echo "FAIL: fetch-protocols.sh" >&2; exit 1; }
fi

say "Ready. Build with:"
if [[ -n "$CXX_HINT" ]]; then
  echo "  cmake --preset release -DCMAKE_C_COMPILER=$CC_HINT -DCMAKE_CXX_COMPILER=$CXX_HINT"
else
  echo "  cmake --preset release"
fi
echo "  cmake --build --preset release"
(( DEV )) && echo "  ctest --preset release"
exit 0
