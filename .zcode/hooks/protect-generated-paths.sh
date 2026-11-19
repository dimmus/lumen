#!/usr/bin/env bash
# PreToolUse guard (Edit|Write|ApplyPatch) for the Lumen repo and its worktrees:
# generated and fetched content is never hand-edited (AGENTS.md §4.4/§4.5).
#   build/generated/**   — produced by CMake protocol codegen (protocol_manifest.py)
#   protocols/upstream/** — fetched verbatim by scripts/fetch-protocols.sh
# Regeneration happens through Bash (cmake / fetch script), which the Bash-side
# companion hook (.zcode/hooks/guard-bash-edits.sh) screens.
#
# FAIL CLOSED: if the hook cannot determine the target path — python3 missing,
# JSON malformed, extraction error — it blocks (exit 2) with the reason. A
# guard that fails open is not a guard.
# Exit codes: 0 = allow, 2 = block (stderr reason is shown to the caller).
input=$(cat)

path=""
if command -v python3 >/dev/null 2>&1; then
  path=$(printf '%s' "$input" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("tool_input",{}).get("file_path",""))' 2>/dev/null)
  if [[ -z "$path" ]] && printf '%s' "$input" | grep -q '"file_path"'; then
    # python3 is present but could not parse the input, and a file_path is in
    # it: malformed JSON. A lossy regex fallback could truncate the path and
    # let the edit through — fail closed instead.
    echo "Blocked (fail closed): a file_path is present but the input is not parseable JSON." >&2
    exit 2
  fi
else
  # No python3 at all: extract the JSON string the conservative way.
  if printf '%s' "$input" | grep -q '"file_path"'; then
    path=$(printf '%s' "$input" | sed -n 's/.*"file_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)
    if [[ -z "$path" ]]; then
      echo "Blocked (fail closed): python3 is missing and the fallback could not extract file_path." >&2
      exit 2
    fi
  fi
fi

case "$path" in
  */build/generated/*|build/generated/*|*/protocols/upstream/*|protocols/upstream/*)
    echo "Blocked: '$path' is generated/fetched content — never hand-edit it. Regenerate instead:" >&2
    echo "  build/generated/     → cmake protocol codegen via protocols/manifest.toml (AGENTS.md §4.4)" >&2
    echo "  protocols/upstream/  → ./scripts/fetch-protocols.sh (AGENTS.md §4.5)" >&2
    exit 2
    ;;
esac
exit 0
