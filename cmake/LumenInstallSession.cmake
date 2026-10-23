# Install-time configuration of the session files (design-session-and-wm S.9): the systemd
# user units, the wayland-sessions entry, lumen-session and the portal files name absolute
# paths of the installed binaries, so they are configured here, at install time, against
# the prefix being installed to — `cmake --install build --prefix DIR` gives DIR's paths,
# and nothing is written outside DIR (plus $DESTDIR).
#
# Set by the install(CODE) before this script: LUMEN_SOURCE_DIR, LUMEN_STAGE_DIR,
# LUMEN_UNIT_DIR_OVERRIDE, LUMEN_SYSTEMD_UNIT_DIR_USR, LUMEN_PORTAL_BUILT, LUMEN_BINDIR_REL,
# LUMEN_LIBEXECDIR_REL, LUMEN_DATADIR_REL.

set(_prefix "${CMAKE_INSTALL_PREFIX}")
set(LUMEN_BINDIR "${_prefix}/${LUMEN_BINDIR_REL}")
set(LUMEN_PORTAL_EXEC "${_prefix}/${LUMEN_LIBEXECDIR_REL}/xdg-desktop-portal-lumen")
if(LUMEN_UNIT_DIR_OVERRIDE)
    set(_unit_dir "${LUMEN_UNIT_DIR_OVERRIDE}")
elseif(_prefix STREQUAL "/usr" AND LUMEN_SYSTEMD_UNIT_DIR_USR)
    set(_unit_dir "${LUMEN_SYSTEMD_UNIT_DIR_USR}")
else()
    set(_unit_dir "${_prefix}/lib/systemd/user")
endif()
if(_prefix STREQUAL "/usr")
    set(_sysconf "/etc")
else()
    set(_sysconf "${_prefix}/etc")
endif()
set(_data "${_prefix}/${LUMEN_DATADIR_REL}")

file(MAKE_DIRECTORY "${LUMEN_STAGE_DIR}")
function(lumen_stage src name)
    configure_file("${LUMEN_SOURCE_DIR}/${src}" "${LUMEN_STAGE_DIR}/${name}" @ONLY)
endfunction()

lumen_stage(deploy/systemd/lumen-compositor.service.in lumen-compositor.service)
lumen_stage(deploy/systemd/lumen-shell.service.in lumen-shell.service)
file(INSTALL DESTINATION "${_unit_dir}" TYPE FILE FILES
    "${LUMEN_STAGE_DIR}/lumen-compositor.service"
    "${LUMEN_STAGE_DIR}/lumen-shell.service"
    "${LUMEN_SOURCE_DIR}/deploy/systemd/lumen-shutdown.target")

lumen_stage(deploy/wayland-sessions/lumen.desktop.in lumen.desktop)
file(INSTALL DESTINATION "${_data}/wayland-sessions" TYPE FILE FILES
    "${LUMEN_STAGE_DIR}/lumen.desktop")

lumen_stage(scripts/lumen-session.in lumen-session)
file(INSTALL DESTINATION "${LUMEN_BINDIR}" TYPE PROGRAM FILES "${LUMEN_STAGE_DIR}/lumen-session")

if(LUMEN_PORTAL_BUILT)
    lumen_stage(deploy/systemd/xdg-desktop-portal-lumen.service.in xdg-desktop-portal-lumen.service)
    lumen_stage(deploy/portal/org.freedesktop.impl.portal.desktop.lumen.service.in
        org.freedesktop.impl.portal.desktop.lumen.service)
    lumen_stage(deploy/xdg/xdg-desktop-portal-wlr/Lumen.in Lumen)
    file(INSTALL DESTINATION "${_unit_dir}" TYPE FILE FILES
        "${LUMEN_STAGE_DIR}/xdg-desktop-portal-lumen.service")
    file(INSTALL DESTINATION "${_data}/dbus-1/services" TYPE FILE FILES
        "${LUMEN_STAGE_DIR}/org.freedesktop.impl.portal.desktop.lumen.service")
    file(INSTALL DESTINATION "${_sysconf}/xdg/xdg-desktop-portal-wlr" TYPE FILE FILES
        "${LUMEN_STAGE_DIR}/Lumen")
endif()
