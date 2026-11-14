# Binary freshness of a build directory, asked of the build system itself. Sourced by the
# smoke scripts; not run on its own.
#
#   build_fresh BUILD TARGET...     0 fresh, 1 stale, 2 could not ask
#   $build_fresh_why                the reason (the first step Ninja would run, when stale)
#
# A smoke that runs a binary older than its sources would test yesterday's code and look
# like a pass, so the scripts gate on freshness. The naive test — is any file under modules/
# newer than the binary? — false-fails the moment a module the binary does not link is
# touched (lx.shell.cppm moved, lumen-compositor stayed: a gate was blocked on exactly
# that), and with shared libraries it never clears once a relink is skipped. So the
# determination is left to Ninja, which knows the real dependency closure (sources, headers,
# C++ module interfaces, depfile-discovered includes, changed command lines):
#
#   1. The manifest first. The glob verification a build would start with (cmake -P
#      VerifyGlobs.cmake — it only touches its stamp when a CONFIGURE_DEPENDS glob result
#      changed, exactly as a build would), then the inputs of build.ninja's own edge
#      (`ninja -t query build.ninja`): a newer CMakeLists.txt or a new globbed source means
#      the manifest is stale, and an unregenerated manifest would hide a new translation
#      unit from step 2.
#   2. A dry run of the targets through a wrapper manifest that only includes build.ninja.
#      Without the wrapper the dry run answers nothing: build.ninja's verification edge is
#      always dirty, and `ninja -n` stops after "Re-running CMake..." (a dry run cannot
#      restat) without looking at the requested target. A dry run writes nothing
#      (.ninja_log and .ninja_deps are opened for write by real runs only).
#
# "Fresh" is Ninja's own "ninja: no work to do." — matched positively, so an output format
# change makes the check fail loudly (stale) instead of silently passing a stale binary.
# Without build.ninja (an installed tree, another generator) there is nothing to ask:
# build_fresh returns 2 and the caller decides.

build_fresh_why=""

build_fresh() { # BUILD TARGET...
  local build=$1
  shift
  build_fresh_why=""
  # Absolute: the wrapper manifest below includes build.ninja by path, and Ninja resolves
  # an include relative to the file that names it, not to this shell's directory.
  build=$(cd "$build" 2>/dev/null && pwd) || { build_fresh_why="no such build directory"; return 2; }

  local ninja="" maker
  maker=$(sed -n 's/^CMAKE_MAKE_PROGRAM:FILEPATH=//p' "$build/CMakeCache.txt" 2>/dev/null)
  [[ -n "$maker" && -x "$maker" ]] && ninja=$maker
  [[ -z "$ninja" ]] && ninja=$(command -v ninja || true)
  if [[ ! -f "$build/build.ninja" || -z "$ninja" ]]; then
    build_fresh_why="no Ninja manifest in $build to ask"
    return 2
  fi

  # Step 1: the manifest's inputs, after the glob check a build would run.
  local verify="$build/CMakeFiles/VerifyGlobs.cmake"
  if [[ -f "$verify" ]] && ! cmake -P "$verify" >/dev/null 2>&1; then
    build_fresh_why="the CONFIGURE_DEPENDS glob check failed: re-run cmake"
    return 1
  fi
  local input
  while IFS= read -r input; do
    [[ "$input" == "|| "* ]] && continue    # order-only: never makes the manifest dirty
    input=${input#| }                       # implicit inputs are listed after a bar
    [[ "$input" == /* ]] || input="$build/$input"
    if [[ -e "$input" && "$input" -nt "$build/build.ninja" ]]; then
      build_fresh_why="the build files changed ($input is newer than build.ninja): re-run cmake"
      return 1
    fi
  done < <("$ninja" -C "$build" -t query build.ninja 2>/dev/null |
           sed -n '/^  input:/,/^  outputs:/p' | sed -n 's/^    //p')

  # Step 2: would `ninja TARGET...` do any work? Asked through a wrapper manifest, so the
  # always-dirty manifest edge (above) is not what gets answered.
  local wrapper path out rc
  wrapper=$(mktemp) || { build_fresh_why="cannot create a temporary manifest"; return 2; }
  path="$build/build.ninja"
  path=${path//\$/\$\$}; path=${path// /\$ }; path=${path//:/\$:}   # Ninja's path escapes
  printf 'include %s\n' "$path" >"$wrapper"
  out=$("$ninja" -C "$build" -f "$wrapper" -n "$@" 2>&1)
  rc=$?
  rm -f "$wrapper"
  if (( rc != 0 )); then
    build_fresh_why="ninja could not say: ${out##*$'\n'}"
    return 2
  fi
  if [[ "$out" == *"ninja: no work to do."* ]]; then
    build_fresh_why="ninja: no work to do"
    return 0
  fi
  build_fresh_why=$(sed -n 's/^\[[0-9/]*\] //p' <<<"$out" | head -1)
  build_fresh_why="ninja would run: ${build_fresh_why:-$(head -1 <<<"$out")}"
  return 1
}
