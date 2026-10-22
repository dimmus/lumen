module;

// xdg-desktop-portal-lumen (design-app-compat-capture Part E, ADR 009): the portal backend
// of a Lumen session — org.freedesktop.impl.portal.Screenshot (Screenshot, PickColor) over
// ext-image-copy-capture-v1, and the source chooser xdg-desktop-portal-wlr runs for
// ScreenCast. ScreenCast itself is xdg-desktop-portal-wlr's (its PipeWire producer on the
// same capture protocol); deploy/portal/lumen-portals.conf routes each interface.
//
// A client of the compositor like any other (it runs outside it, unsandboxed, and uses the
// capture globals the compositor hides from sandboxed clients); the old Flatpak-token stub
// this module held is gone — sandboxed apps reach capture only through the frontend's
// permission checks and this backend.
//
//   :compose  frames turned upright, composed in layout space (pure)
//   :png      libpng encoding
//   :files    where screenshots go, URIs (pure parts)
//   :chooser  xdpw's dmenu lines (pure)
//   :desktop  the Wayland connection: outputs, toplevels, shm captures
//   :overlay  layer-shell surfaces: area/output selection, colour picking, lists
//   :service  the D-Bus service and the chooser entry point

export module lx.portal;

export import :compose;
export import :png;
export import :files;
export import :chooser;
export import :desktop;
export import :overlay;
export import :service;
