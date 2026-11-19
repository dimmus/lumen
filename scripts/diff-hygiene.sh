#!/usr/bin/env bash
# Diff hygiene for a Lumen change: the mechanical checks the reviewer should
# not have to eyeball.
#
#   1. Line-ending violations: .gitattributes is the policy (`* text=auto
#      eol=lf` — everything LF except *.bat/*.cmd/*.ps1). A changed file with
#      CR bytes in the working tree violates it, and because git normalizes
#      on commit the pollution is INVISIBLE to git diff — it breaks scripts
#      and editors instead, later. (The recorded trap: scripted/Python edits
#      flipping endings; two incidents before .gitattributes pinned LF.)
#   2. Whole-file rewrites: a large file where ~every line changed is almost
#      never intentional — name it before review does.
#   3. clang-format --dry-run --Werror on the changed C++ files (.clang-format
#      is the contract; style was previously unenforced → churn in every diff).
#
#   scripts/diff-hygiene.sh [BASE]     # default HEAD: the working-tree diff
#
# Exit 0 clean, 1 findings (each with the file and the evidence), 2 misuse.
set -u

BASE="${1:-HEAD}"
repo="$(git rev-parse --show-toplevel 2>/dev/null)" || { echo "not a git repo" >&2; exit 2; }
cd "$repo"

# Changed files: committed diff BASE..HEAD plus the working tree, plus untracked.
mapfile -t files < <(
  { git diff --name-only "$BASE" -- 2>/dev/null
    git diff --name-only HEAD -- 2>/dev/null | grep -vxF -f <(git diff --name-only "$BASE" 2>/dev/null) 2>/dev/null
    git ls-files --others --exclude-standard
  } | sort -u
)
[[ ${#files[@]} -gt 0 ]] || { echo "diff hygiene: no changed files against $BASE"; exit 0; }

fail=0
checked=0

# --- 1: line-ending policy, worktree-wide -------------------------------------
# A CRLF-only flip is INVISIBLE to git diff (git normalizes on commit), so
# this scans the whole worktree — tracked + untracked files — not just the
# changed list. *.bat/*.cmd/*.ps1 are eol=crlf by .gitattributes: policy.
# A file whose BASE blob was already CRLF is pre-existing debt (e.g. evidence
# logs force-added past the attributes) — reported once as debt, not failed;
# a fresh flip (BASE blob LF, worktree CRLF) or an untracked CRLF file fails.
crlf_hits=$(git ls-files -z --cached --others --exclude-standard \
  | xargs -0 -r grep -lI $'\r' 2>/dev/null \
  | grep -vE '\.(bat|cmd|ps1)$' || true)
debt=0
if [[ -n "$crlf_hits" ]]; then
  while IFS= read -r f; do
    if git cat-file -e "$BASE:$f" 2>/dev/null \
       && [[ "$(git show "$BASE:$f" 2>/dev/null | grep -c $'\r' || true)" -gt 0 ]]; then
      debt=$((debt + 1))   # pre-existing: the blob already carried CRLF
      continue
    fi
    echo "HYGIENE: $f — CRLF in the working tree; .gitattributes says eol=lf and git normalizes on commit, so git diff does NOT show this. Fix the endings (scripted/Python edits are the usual cause)."
    fail=1
  done <<<"$crlf_hits"
  if (( debt > 0 )); then
    echo "diff hygiene: $debt pre-existing CRLF file(s) already in $BASE (baseline debt — recorded, not failed on)"
  fi
fi

# --- 2: whole-file rewrites among the changed files ----------------------------
for f in "${files[@]}"; do
  [[ -f "$f" ]] || continue
  case "$f" in
    *.png|*.jpg|*.webp|*.otf|*.ttf|*.zst|*.gz) continue ;;
  esac
  checked=$((checked + 1))

  # Whole-file rewrite: nearly every line changed in a file with real history.
  if git cat-file -e "$BASE:$f" 2>/dev/null; then
    read -r add del < <(git diff --numstat "$BASE" -- "$f" | awk '{print $1" "$2}' | head -1)
    before=$(git show "$BASE:$f" | wc -l)
    if [[ -n "${add:-}" && "${add:-0}" != "-" && "$before" -gt 300 ]]; then
      if (( add + del > 180 * before / 100 )); then
        echo "HYGIENE: $f — whole-file rewrite (+$add/-$del of $before lines). Usually a line-ending or re-indent accident; if intentional, say so in the commit message."
        fail=1
      fi
    fi
  fi
done

# --- 3: clang-format on changed C++ ------------------------------------------
cxx=()
for f in "${files[@]}"; do
  case "$f" in *.cpp|*.cppm|*.cc|*.cxx|*.h|*.hpp) [[ -f "$f" ]] && cxx+=("$f") ;; esac
done
if [[ ${#cxx[@]} -gt 0 ]]; then
  if command -v clang-format >/dev/null 2>&1; then
    # --Werror makes every needed reformat an error; -n writes nothing.
    local fmtlog; fmtlog=$(mktemp /tmp/diff-hygiene-fmt.XXXXXX)
    if ! clang-format --dry-run --Werror --style=file "${cxx[@]}" >/dev/null 2>"$fmtlog"; then
      echo "HYGIENE: clang-format disagrees on:" >&2
      grep -oE '^[^ ]+\.(cpp|cppm|h|hpp)' "$fmtlog" 2>/dev/null | sort -u | sed 's/^/  /' >&2
      rm -f "$fmtlog"
      echo "  run: clang-format -i --style=file <files> (the .clang-format file is the contract)" >&2
      fail=1
    fi
  else
    echo "diff hygiene: clang-format not found — format check SKIPPED (install clang-format or check locally)"
  fi
fi

echo "diff hygiene: $checked file(s) ending/rewrite-checked, ${#cxx[@]} format-checked against $BASE"
exit "$fail"
