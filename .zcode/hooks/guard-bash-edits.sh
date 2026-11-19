#!/usr/bin/env bash
# PreToolUse guard (Bash) for the Lumen repo and its worktrees: an agent can
# edit files through the shell (sed -i, perl -i, python heredocs, redirects)
# instead of the Edit/Write tools — this hook closes that bypass for the same
# protected paths the tool-side hook (.zcode/hooks/protect-generated-paths.sh)
# enforces: generated protocol code and fetched upstream protocols are never
# hand-edited (AGENTS.md §4.4/§4.5); regeneration via cmake / fetch-protocols.sh
# is the sanctioned path and does not match these patterns.
#
# It screens the command string for BOTH a protected path token AND a
# file-mutating pattern aimed at it. Legit regeneration commands (cmake,
# ninja, ./scripts/fetch-protocols.sh) contain neither an in-place editor nor
# a redirect onto a protected path and pass through. BEST-EFFORT accident
# backstop, not a security boundary: unusual spellings (sed --in-place,
# git checkout <ref> --, install, pathlib write_text) can pass — the
# Edit/Write-side hook is the hard gate; this closes the common accidents.
#
# FAIL CLOSED on its own errors (unparseable input): exit 2 with the reason.
# Exit codes: 0 = allow, 2 = block (stderr reason is shown to the caller).
input=$(cat)

cmd=""
if command -v python3 >/dev/null 2>&1; then
  cmd=$(printf '%s' "$input" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("tool_input",{}).get("command",""))' 2>/dev/null)
  if [[ -z "$cmd" ]] && printf '%s' "$input" | grep -q '"command"'; then
    # python3 is present but could not parse the input, and a command is in
    # it: malformed JSON. A lossy regex fallback could truncate the command
    # and let the write through — fail closed instead.
    echo "Blocked (fail closed): a command is present but the input is not parseable JSON." >&2
    exit 2
  fi
else
  if printf '%s' "$input" | grep -q '"command"'; then
    cmd=$(printf '%s' "$input" | sed -n 's/.*"command"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)
    if [[ -z "$cmd" ]]; then
      echo "Blocked (fail closed): python3 is missing and the fallback could not extract the command." >&2
      exit 2
    fi
  fi
fi

[[ -n "$cmd" ]] && case "$cmd" in
  *build/generated/*|*protocols/upstream/*)
    # The command names a protected path. Block when it also carries a
    # write-ish pattern — an in-place editor, a redirect, a copy/move/dd onto
    # it, or an interpreter (python/ruby) whose code will open it for writing
    # (covering heredocs and -c one-liners we cannot fully parse).
    if printf '%s' "$cmd" | grep -qE -- '-i([[:space:]]|$)|sed -i|perl -i|awk -i|gawk -i|>>?[^ ]*|tee |dd |mv |cp |rsync |patch |ed |ex |> *build/generated|> *protocols/upstream|open\([^)]*,[[:space:]]*.[wa]|File\([^)]*,[[:space:]]*.[wa]'; then
      echo "Blocked: this Bash command writes into generated/fetched content —" >&2
      echo "  build/generated/     → regenerate: cmake protocol codegen via protocols/manifest.toml (AGENTS.md §4.4)" >&2
      echo "  protocols/upstream/  → refetch: ./scripts/fetch-protocols.sh (AGENTS.md §4.5)" >&2
      echo "  (Read-only use of these paths is fine; the hook matched a write pattern.)" >&2
      exit 2
    fi
    ;;
esac
exit 0
