#!/usr/bin/env bash
# lumen-session against a fake systemctl (design-session-and-wm T.1): what it imports into
# the user manager (the denylist), that it puts the manager's environment back exactly
# (values that were set, values that were absent, after a crashed compositor too), that it
# refuses a second graphical session, --cleanup, values it cannot read exactly being left
# alone, and the fallback without a user manager.
#
#   test_lumen_session.sh SOURCE_DIR WORK_DIR
set -u
SRC=${1:?source dir}
WORK=${2:?work dir}
rm -rf "$WORK"
mkdir -p "$WORK/bin" "$WORK/state" "$WORK/runtime"
chmod 700 "$WORK/runtime"
STATE="$WORK/state"
fails=0
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1${2:+ — $2}"; fails=$((fails + 1)); }

sed "s|@LUMEN_BINDIR@|$WORK/bin|" "$SRC/scripts/lumen-session.in" >"$WORK/bin/lumen-session"
chmod +x "$WORK/bin/lumen-session"

# ── Fakes ────────────────────────────────────────────────────────────────────
cat >"$WORK/bin/systemctl" <<'EOF'
#!/usr/bin/env bash
# A user manager in files: $STATE/env (NAME=VALUE lines), $STATE/active (unit names).
S="$FAKE_STATE"
echo "systemctl $*" >>"$S/log"
[[ "$1" == --user ]] && shift
[[ -e "$S/no-manager" ]] && exit 1
quiet=0; [[ "$1" == -q ]] && { quiet=1; shift; }
wait_flag=0; [[ "$1" == --wait ]] && { wait_flag=1; shift; }
setvar() { grep -v "^${1%%=*}=" "$S/env" >"$S/env.tmp"; echo "$1" >>"$S/env.tmp"; sort -o "$S/env" "$S/env.tmp"; }
unsetvar() { grep -v "^$1=" "$S/env" >"$S/env.tmp"; mv "$S/env.tmp" "$S/env"; }
case "$1" in
  show-environment) cat "$S/env" ;;
  set-environment) shift; for a in "$@"; do setvar "$a"; done ;;
  unset-environment) shift; for n in "$@"; do unsetvar "$n"; done ;;
  import-environment) shift; for n in "$@"; do setvar "$n=${!n}"; done ;;
  is-active) grep -qx "$2" "$S/active"; exit $? ;;
  reset-failed|stop) ;;
  start)
    if [[ "$2" == lumen-compositor.service ]]; then
      # The compositor exports its variables; a clean exit unsets them, a crash does not.
      setvar WAYLAND_DISPLAY=lumen-0; setvar LUMEN_SOCKET=/run/x/lumen-0.varlink
      setvar XDG_SESSION_TYPE=wayland
      cp "$S/env" "$S/env-during"
      if [[ -e "$S/crash" ]]; then exit 3; fi
      unsetvar WAYLAND_DISPLAY; unsetvar LUMEN_SOCKET; unsetvar XDG_SESSION_TYPE
      unsetvar XDG_CURRENT_DESKTOP; unsetvar XDG_SESSION_DESKTOP
    fi ;;
  show) echo "core-dump" ;;
esac
exit 0
EOF
cat >"$WORK/bin/dbus-update-activation-environment" <<'EOF'
#!/usr/bin/env bash
echo "dbus-update-activation-environment $*" >>"$FAKE_STATE/log"
EOF
cat >"$WORK/bin/loginctl" <<'EOF'
#!/usr/bin/env bash
echo "${FAKE_ACTIVE:-yes}"
EOF
# Fallback binaries: the compositor serves until its socket file is removed.
cat >"$WORK/bin/lumen-compositor" <<'EOF'
#!/usr/bin/env bash
echo "compositor $*" >>"$FAKE_STATE/log"
sleep 2
EOF
cat >"$WORK/bin/lumen-ctl" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
cat >"$WORK/bin/lumen-shell" <<'EOF'
#!/usr/bin/env bash
echo "shell WAYLAND_DISPLAY=$WAYLAND_DISPLAY" >>"$FAKE_STATE/log"
sleep 5
EOF
chmod +x "$WORK"/bin/*

reset_state() {
  rm -rf "$STATE" "$WORK/runtime/lumen"
  mkdir -p "$STATE"
  : >"$STATE/log"
  : >"$STATE/active"
  printf '%s\n' "HOME=$HOME" "PATH=/usr/bin" "WAYLAND_DISPLAY=wayland-1" "FOO=old" \
    "QUOTED=\$'a\\nb'" | sort >"$STATE/env"
}

# The login's environment: what a console login would have.
run_session() {
  env -i HOME="$HOME" PATH="$WORK/bin:/usr/bin:/bin" XDG_RUNTIME_DIR="$WORK/runtime" \
    XDG_CONFIG_HOME="$WORK/config" XDG_CONFIG_DIRS="$WORK/xdg" \
    FAKE_STATE="$STATE" LANG=C.UTF-8 FOO=new BAR=fresh QUOTED=login TERM=linux \
    SSH_CONNECTION="1 2 3 4" XDG_SESSION_ID=1 XDG_SESSION_TYPE=tty LUMEN_DISPLAY=lumen-7 \
    "$@"
}

echo "test_lumen_session"

# 1. A session imports the login's variables (not the denied ones) and restores exactly.
reset_state
cp "$STATE/env" "$STATE/env-before"
run_session "$WORK/bin/lumen-session" -l >"$STATE/out" 2>&1
rc=$?
if (( rc == 0 )) && diff -q "$STATE/env" "$STATE/env-before" >/dev/null; then
  pass "environment restored exactly after the session"
else
  fail "environment restored exactly after the session" "rc=$rc; $(diff "$STATE/env-before" "$STATE/env" | tr '\n' ' ')"
fi
during="$STATE/env-during"
if grep -qx 'FOO=new' "$during" && grep -qx 'BAR=fresh' "$during" && grep -qx 'LANG=C.UTF-8' "$during" &&
   grep -qx 'XDG_CURRENT_DESKTOP=Lumen' "$during" && grep -qx 'XDG_SESSION_DESKTOP=lumen' "$during" &&
   grep -qx 'LUMEN_DISPLAY=lumen-7' "$during" && grep -qx 'XDG_SESSION_ID=1' "$during"; then
  pass "login variables imported (with the desktop defaults)"
else
  fail "login variables imported" "$(tr '\n' ' ' <"$during")"
fi
if ! grep -q '^TERM=\|^SSH_CONNECTION=\|^PWD=\|^SHLVL=' "$during"; then
  pass "denied variables not imported"
else
  fail "denied variables not imported" "$(grep '^TERM=\|^SSH_\|^PWD=\|^SHLVL=' "$during" | tr '\n' ' ')"
fi
if grep -qx "QUOTED=\$'a\\\\nb'" "$during"; then
  pass "a value systemctl had to escape is left alone"
else
  fail "a value systemctl had to escape is left alone" "$(grep QUOTED "$during")"
fi
if grep -q 'start --job-mode=replace-irreversibly lumen-shutdown.target' "$STATE/log" &&
   grep -q '^dbus-update-activation-environment .*FOO' "$STATE/log"; then
  pass "shutdown target started; D-Bus activation environment updated"
else
  fail "shutdown target and activation environment" "$(tr '\n' '|' <"$STATE/log")"
fi
[[ ! -e "$WORK/runtime/lumen/session-env" ]] && pass "record removed at the end" ||
  fail "record removed at the end"

# 2. A compositor that crashed left its variables: they are restored too, exit 1.
reset_state
cp "$STATE/env" "$STATE/env-before"
touch "$STATE/crash"
run_session "$WORK/bin/lumen-session" -l >"$STATE/out" 2>&1
rc=$?
if (( rc == 1 )) && diff -q "$STATE/env" "$STATE/env-before" >/dev/null &&
   grep -q 'journalctl --user' "$STATE/out"; then
  pass "a crashed compositor's variables restored, exit 1 with a hint"
else
  fail "crashed compositor restore" "rc=$rc $(diff "$STATE/env-before" "$STATE/env" | tr '\n' ' ')"
fi

# 3. A running graphical session: refused, nothing touched.
reset_state
cp "$STATE/env" "$STATE/env-before"
echo graphical-session.target >"$STATE/active"
run_session "$WORK/bin/lumen-session" -l >"$STATE/out" 2>&1
rc=$?
if (( rc == 2 )) && diff -q "$STATE/env" "$STATE/env-before" >/dev/null &&
   ! grep -q 'lumen-compositor.service' "$STATE/log"; then
  pass "a second graphical session is refused"
else
  fail "a second graphical session is refused" "rc=$rc"
fi

# 4. An inactive console session: refused.
reset_state
run_session FAKE_ACTIVE=no "$WORK/bin/lumen-session" -l >"$STATE/out" 2>&1
(( $? == 2 )) && pass "an inactive session (VT in the background) is refused" ||
  fail "an inactive session is refused"

# 5. --cleanup undoes a record left by a session that died with the script.
reset_state
mkdir -p "$WORK/runtime/lumen"
printf '%s\n' "FOO=restored" "-WAYLAND_DISPLAY" >"$WORK/runtime/lumen/session-env"
run_session "$WORK/bin/lumen-session" --cleanup >"$STATE/out" 2>&1
if grep -qx 'FOO=restored' "$STATE/env" && ! grep -q '^WAYLAND_DISPLAY=' "$STATE/env" &&
   [[ ! -e "$WORK/runtime/lumen/session-env" ]]; then
  pass "--cleanup restores a left-over record"
else
  fail "--cleanup restores a left-over record" "$(tr '\n' ' ' <"$STATE/env")"
fi

# 6. No user manager: the compositor and the shell are run directly, and the autostart
#    entries meant for this desktop are started.
reset_state
touch "$STATE/no-manager"
mkdir -p "$WORK/config/autostart" "$WORK/xdg/autostart"
cat >"$WORK/bin/autostart-probe" <<'EOS'
#!/usr/bin/env bash
echo "autostart $1" >>"$FAKE_STATE/log"
EOS
chmod +x "$WORK/bin/autostart-probe"
printf '[Desktop Entry]\nType=Application\nName=A\nExec=%s one %%U\n' "$WORK/bin/autostart-probe" \
  >"$WORK/config/autostart/a.desktop"
printf '[Desktop Entry]\nType=Application\nName=B\nExec=%s gnome-only\nOnlyShowIn=GNOME;\n' \
  "$WORK/bin/autostart-probe" >"$WORK/xdg/autostart/b.desktop"
printf '[Desktop Entry]\nType=Application\nName=C\nExec=%s hidden\nHidden=true\n' \
  "$WORK/bin/autostart-probe" >"$WORK/xdg/autostart/c.desktop"
# The lists match any desktop of XDG_CURRENT_DESKTOP, and their last ';' is optional.
printf '[Desktop Entry]\nType=Application\nName=D\nExec=%s lumen-only\nOnlyShowIn=Lumen\n' \
  "$WORK/bin/autostart-probe" >"$WORK/xdg/autostart/d.desktop"
printf '[Desktop Entry]\nType=Application\nName=E\nExec=%s not-lumen\nNotShowIn=KDE;Lumen\n' \
  "$WORK/bin/autostart-probe" >"$WORK/xdg/autostart/e.desktop"
printf '[Desktop Entry]\nType=Application\nName=F\nExec=%s second-desktop\nOnlyShowIn=Test;\n' \
  "$WORK/bin/autostart-probe" >"$WORK/xdg/autostart/f.desktop"
# Overridden by the user's own a.desktop: the system copy never runs.
printf '[Desktop Entry]\nType=Application\nName=A\nExec=%s system-a\n' \
  "$WORK/bin/autostart-probe" >"$WORK/xdg/autostart/a.desktop"
run_session XDG_CURRENT_DESKTOP=Lumen:Test timeout 10 "$WORK/bin/lumen-session" -l \
  >"$STATE/out" 2>&1
rc=$?
if grep -q '^compositor lumen-7' "$STATE/log" && grep -q '^shell WAYLAND_DISPLAY=lumen-7' "$STATE/log" &&
   (( rc != 124 )); then
  pass "fallback without a user manager runs compositor and shell, ends with the compositor"
else
  fail "fallback" "rc=$rc $(tr '\n' '|' <"$STATE/log")"
fi
sleep 0.3
if grep -qx 'autostart one' "$STATE/log" && grep -qx 'autostart lumen-only' "$STATE/log" &&
   grep -qx 'autostart second-desktop' "$STATE/log" &&
   ! grep -q 'autostart gnome-only\|autostart hidden\|autostart system-a\|autostart not-lumen' "$STATE/log"; then
  pass "fallback autostart: this desktop's entries, user over system, Hidden, OnlyShowIn and NotShowIn honoured"
else
  fail "fallback autostart" "$(grep autostart "$STATE/log" | tr '\n' '|')"
fi

# 7. The login shell re-exec: without -l it goes through $SHELL -l once.
reset_state
cat >"$WORK/bin/fake-login-shell" <<'EOF'
#!/usr/bin/env bash
echo "login-shell $*" >>"$FAKE_STATE/log"
[[ "$1" == -l && "$2" == -c ]] || exit 9
shift 2
exec bash -c "$@"
EOF
chmod +x "$WORK/bin/fake-login-shell"
run_session SHELL="$WORK/bin/fake-login-shell" "$WORK/bin/lumen-session" >"$STATE/out" 2>&1
rc=$?
if (( rc == 0 )) && [[ $(grep -c '^login-shell' "$STATE/log") == 1 ]]; then
  pass "re-executed once through the login shell"
else
  fail "login shell re-exec" "rc=$rc $(tr '\n' '|' <"$STATE/log")"
fi

echo "$fails failed"
(( fails == 0 ))
