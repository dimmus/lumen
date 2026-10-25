# Sourced by install-deps.sh and run-desktop.sh: whether nouveau offers atomic modesetting,
# which the compositor's KMS path requires (a card without it is refused, drm_err::no_atomic).
#
# Linux before 7.4 leaves it off on nouveau unless nouveau.atomic=1 (kernel commit
# 2652aca460dc turned it on by default for NV50+). GPUs before NV50 (GeForce 8) have none.

# The nouveau-driven cards (/dev/dri/cardN), one per line; empty when nouveau drives none.
nouveau_cards() {
  local d
  for d in /sys/class/drm/card*; do
    [[ "${d##*/}" =~ ^card[0-9]+$ ]] || continue
    [[ "$(basename "$(readlink -f "$d/device/driver" 2>/dev/null)" 2>/dev/null)" == nouveau ]] \
      && echo "/dev/dri/${d##*/}"
  done
}

# on | off | pending (configured in /etc/modprobe.d after this boot: takes effect at the next).
# /sys/module/nouveau/parameters/atomic is root-only (0400): read through sudo -n when that
# needs no password; otherwise the kernel command line, /etc/modprobe.d and the kernel
# version decide.
nouveau_atomic_state() {
  local v kver conf boot newest=0 m
  v="$(cat /sys/module/nouveau/parameters/atomic 2>/dev/null || sudo -n cat /sys/module/nouveau/parameters/atomic 2>/dev/null)"
  if [[ -n "$v" ]]; then
    # 1, or -1 ("auto": on for NV50+ from Linux 7.4).
    if [[ "$v" == 0 ]]; then
      conf="$(grep -lsE '^[[:space:]]*options[[:space:]]+nouveau([[:space:]].*)?[[:space:]]atomic=1' /etc/modprobe.d/*.conf)"
      [[ -n "$conf" ]] && echo pending || echo off
    else
      echo on
    fi
    return
  fi
  [[ " $(</proc/cmdline) " == *" nouveau.atomic=0 "* ]] && { echo off; return; }
  [[ " $(</proc/cmdline) " == *" nouveau.atomic=1 "* ]] && { echo on; return; }
  conf="$(grep -lsE '^[[:space:]]*options[[:space:]]+nouveau([[:space:]].*)?[[:space:]]atomic=1' /etc/modprobe.d/*.conf)"
  if [[ -n "$conf" ]]; then
    # Written before this boot: the module was loaded with it.
    boot="$(sed -n 's/^btime //p' /proc/stat)"
    for m in $conf; do
      (( $(stat -c %Y "$m") > newest )) && newest="$(stat -c %Y "$m")"
    done
    (( newest < boot )) && echo on || echo pending
    return
  fi
  kver="$(uname -r)"
  kver="${kver%%-*}"
  [[ "$(printf '%s\n%s\n' 7.4 "$kver" | sort -V | head -1)" == 7.4 ]] && echo on || echo off
}
