#!/usr/bin/env bash
# PreToolUse guard (Edit|Write|ApplyPatch) for the Lumen repo and its worktrees:
# generated and fetched content is never hand-edited (AGENTS.md §4.4/§4.5).
#   build/generated/**   — produced by CMake protocol codegen (protocol_manifest.py)
#   protocols/upstream/** — fetched verbatim by scripts/fetch-protocols.sh
# Regeneration happens through Bash (cmake / fetch script), which this hook does
# not touch — it only stops hand edits of the artifacts.
# Exit codes: 0 = allow, 2 = block (stderr reason is shown to the caller).
input=$(cat)
path=$(printf '%s' "$input" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("tool_input",{}).get("file_path",""))' 2>/dev/null)
case "$path" in
  */build/generated/*|*/protocols/upstream/*)
    echo "Blocked: '$path' is generated/fetched content — never hand-edit it. Regenerate instead:" >&2
    echo "  build/generated/     → cmake protocol codegen via protocols/manifest.toml (AGENTS.md §4.4)" >&2
    echo "  protocols/upstream/  → ./scripts/fetch-protocols.sh (AGENTS.md §4.5)" >&2
    exit 2
    ;;
esac
exit 0
