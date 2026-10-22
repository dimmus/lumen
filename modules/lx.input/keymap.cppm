module;

#include <stdlib.h>

#if defined(LUMEN_HAS_INPUT)
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#endif

import lx.foundation;

export module lx.input:keymap;

import std;

export namespace lx::input {

/// Modifier state in the form `wl_keyboard.modifiers` wants it. These are serialized xkb
/// masks, not a fixed enum: which bit means Shift depends on the compiled keymap, so the
/// compositor forwards them verbatim and lets the client's own xkb resolve them.
struct modifier_state {
    unsigned depressed = 0;
    unsigned latched = 0;
    unsigned locked = 0;
    unsigned group = 0;

    [[nodiscard]] bool operator==(const modifier_state&) const = default;
};

/// Named modifiers, resolved against the active keymap. For compositor-side keybindings,
/// which need to ask "is Super held" without knowing the keymap's bit layout.
struct named_modifiers {
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
    bool super = false;
    bool caps_lock = false;
    bool num_lock = false;
};

/// Keyboard layout configuration. Empty fields mean "the system default", which xkb
/// resolves from its own rules — not hardcoded to US.
struct keymap_config {
    const char* rules = nullptr;
    const char* model = nullptr;
    const char* layout = nullptr;
    const char* variant = nullptr;
    const char* options = nullptr;
};

/// Compiled keymap plus the live state of the keys held on it.
///
/// A compositor needs both halves and for different reasons. The *keymap* is what clients
/// need — it is sent to them once, as a file descriptor, and their own libxkbcommon
/// interprets every keycode against it. The *state* is what the compositor needs, to know
/// which modifiers are active for its own keybindings and to tell clients when they change.
///
/// Previously the compositor sent clients a hardcoded US keymap string and kept no state at
/// all, so no layout other than US worked and nothing could answer "is Ctrl held".
class keyboard_keymap {
public:
    keyboard_keymap() = default;
    ~keyboard_keymap();

    keyboard_keymap(const keyboard_keymap&) = delete;
    keyboard_keymap& operator=(const keyboard_keymap&) = delete;
    keyboard_keymap(keyboard_keymap&& other) noexcept;
    keyboard_keymap& operator=(keyboard_keymap&& other) noexcept;

    /// Compiles a keymap. Falls back to the system default when a field is null.
    [[nodiscard]] lx::result<void> compile(keymap_config config = {});

    [[nodiscard]] bool valid() const { return state_ != nullptr; }

    /// Feeds a key transition. `keycode` is evdev — the +8 offset xkb expects is applied
    /// here, once, rather than at every call site where it is easy to forget.
    /// Returns true when the modifier state changed as a result.
    [[nodiscard]] bool update_key(unsigned evdev_keycode, bool pressed);

    /// Applies modifier state pushed from elsewhere (a session leader, or a client that
    /// owns the seat). Returns true when anything changed.
    [[nodiscard]] bool update_mask(modifier_state mods);

    [[nodiscard]] modifier_state modifiers() const;
    [[nodiscard]] named_modifiers named() const;

    /// The keysym a keycode currently produces, given layout and modifiers. 0 when none.
    [[nodiscard]] unsigned keysym(unsigned evdev_keycode) const;

    /// UTF-8 for a keycode in the current state; empty when the key produces no text.
    /// `out` must have room for at least 8 bytes.
    [[nodiscard]] unsigned utf8(unsigned evdev_keycode, char* out, unsigned capacity) const;

    /// The keysym a keycode produces at its first shift level in `layout`, regardless of the
    /// modifiers held — what a key binding names ("Super+Return" is Return, not whatever
    /// Shift would make of it). 0 when the key has none in that layout.
    [[nodiscard]] unsigned level1_keysym(unsigned evdev_keycode, unsigned layout) const;
    /// The effective layout (group) index.
    [[nodiscard]] unsigned active_layout() const;
    /// Some key produces `keysym` at its first level in `layout`. Scans the keymap's
    /// keycodes; for the rare binding lookup that falls back to layout 0, never per event.
    [[nodiscard]] bool layout_has_keysym(unsigned keysym, unsigned layout) const;
    /// Forgets every held key: a fresh state keeping only the locked modifiers (Caps Lock,
    /// Num Lock) and the locked layout. For a session pause, after which no key is down.
    /// Returns true when the serialized modifiers changed.
    [[nodiscard]] bool reset_depressed();
    /// The keyboard LEDs the state lights, as libinput's `libinput_led` bits (num 1, caps
    /// 2, scroll 4), for `libinput_device_led_update`.
    [[nodiscard]] unsigned led_mask() const;

    /// Whether this key repeats when held, per the keymap. Clients do their own repeating
    /// from `wl_keyboard.repeat_info`, but the compositor needs this for its own bindings.
    [[nodiscard]] bool repeats(unsigned evdev_keycode) const;

    /// The keymap as a string in xkb v1 text format, for handing to clients.
    /// Caller owns the returned buffer and frees it with `free_keymap_string`.
    [[nodiscard]] char* keymap_string() const;
    static void free_keymap_string(char* s);

    /// A read-only, sealed fd holding the keymap text, ready for `wl_keyboard.keymap`.
    /// Returns the size in `size_out`. Caller owns the fd.
    [[nodiscard]] lx::result<lx::unique_fd> keymap_fd(unsigned& size_out) const;

private:
    void destroy();

    void* context_ = nullptr; // xkb_context*
    void* keymap_ = nullptr;  // xkb_keymap*
    void* state_ = nullptr;   // xkb_state*
    modifier_state last_{};
};

/// A keymap a client supplied (zwp_virtual_keyboard_v1.keymap), compiled — one that does not
/// compile is refused — and serialized again into a sealed, read-only memfd: the form of every
/// keymap the compositor hands to clients. Serialization is canonical (the seat's own keymap
/// text comes back byte for byte), so "is this the seat's keymap" is a memcmp of the two.
/// `text` need not be NUL-terminated; trailing NULs are ignored. One compile per request:
/// never on a hot path. `size_out` includes the terminating NUL, as `keymap_fd`'s does.
[[nodiscard]] lx::result<lx::unique_fd> normalize_keymap(const char* text, unsigned long len,
                                                        unsigned& size_out);

/// Largest keymap a client may hand over (a full keymap is ~60 KiB).
inline constexpr unsigned long k_max_client_keymap = 4ul << 20;

/// `normalize_keymap` of the `size` bytes a client's keymap fd holds. Read with pread, never
/// mapped: the file can be shorter than the client says, or be truncated while it is read,
/// and touching a mapping past its end raises SIGBUS in the compositor (no handler covers a
/// client's keymap; libwayland's covers wl_shm pools only). A file shorter than `size`, or a
/// `size` of 0 or above `k_max_client_keymap`, is refused. `fd` stays the caller's.
[[nodiscard]] lx::result<lx::unique_fd> normalize_keymap_fd(int fd, unsigned long size,
                                                           unsigned& size_out);

} // namespace lx::input


#if defined(LUMEN_HAS_INPUT)

namespace {

/// evdev keycodes are offset by 8 from xkb's. Wrong by eight is not obviously wrong — it
/// produces plausible letters — so it is converted in exactly one place.
[[nodiscard]] xkb_keycode_t to_xkb(unsigned evdev) { return static_cast<xkb_keycode_t>(evdev + 8); }

[[nodiscard]] bool mod_active(xkb_state* state, const char* name) {
    return xkb_state_mod_name_is_active(state, name, XKB_STATE_MODS_EFFECTIVE) > 0;
}

/// `len` bytes of keymap text (its NUL included) in a memfd sealed read-only. A client maps
/// it shared: without the seals a hostile or buggy one could resize it under every other
/// client and under the compositor. memfd rather than a temp file: the keymap is handed to
/// every client, and a path on disk is both a race and a leak of something that never needs
/// to exist there.
[[nodiscard]] lx::result<lx::unique_fd> sealed_keymap_fd(const char* text, unsigned len) {
    const int fd = ::memfd_create("lumen-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return lx::make_error(lx::error_domain::io, 0, "memfd_create failed for keymap");
    lx::unique_fd owned{fd};
    if (::ftruncate(fd, static_cast<off_t>(len)) != 0)
        return lx::make_error(lx::error_domain::io, 0, "ftruncate failed for keymap");
    void* map = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
        return lx::make_error(lx::error_domain::io, 0, "mmap failed for keymap");
    std::memcpy(map, text, len);
    ::munmap(map, len);
    (void)::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE);
    return owned;
}

} // namespace

#endif

lx::input::keyboard_keymap::~keyboard_keymap() { destroy(); }

void lx::input::keyboard_keymap::destroy() {
#if defined(LUMEN_HAS_INPUT)
    if (state_)
        xkb_state_unref(static_cast<xkb_state*>(state_));
    if (keymap_)
        xkb_keymap_unref(static_cast<xkb_keymap*>(keymap_));
    if (context_)
        xkb_context_unref(static_cast<xkb_context*>(context_));
#endif
    state_ = nullptr;
    keymap_ = nullptr;
    context_ = nullptr;
    last_ = {};
}

lx::input::keyboard_keymap::keyboard_keymap(keyboard_keymap&& other) noexcept
    : context_{other.context_}, keymap_{other.keymap_}, state_{other.state_},
      last_{other.last_} {
    other.context_ = nullptr;
    other.keymap_ = nullptr;
    other.state_ = nullptr;
    other.last_ = {};
}

lx::input::keyboard_keymap& lx::input::keyboard_keymap::operator=(
    keyboard_keymap&& other) noexcept {
    if (this == &other)
        return *this;
    destroy();
    context_ = other.context_;
    keymap_ = other.keymap_;
    state_ = other.state_;
    last_ = other.last_;
    other.context_ = nullptr;
    other.keymap_ = nullptr;
    other.state_ = nullptr;
    other.last_ = {};
    return *this;
}

lx::result<void> lx::input::keyboard_keymap::compile(keymap_config config) {
#if defined(LUMEN_HAS_INPUT)
    destroy();

    auto* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) {
        return lx::make_error(lx::error_domain::io, 0, "xkb_context_new failed");
    }
    context_ = ctx;

    xkb_rule_names names{};
    names.rules = config.rules;
    names.model = config.model;
    names.layout = config.layout;
    names.variant = config.variant;
    names.options = config.options;

    auto* map = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!map) {
        destroy();
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "xkb_keymap_new_from_names failed — check layout/variant");
    }
    keymap_ = map;

    auto* st = xkb_state_new(map);
    if (!st) {
        destroy();
        return lx::make_error(lx::error_domain::io, 0, "xkb_state_new failed");
    }
    state_ = st;
    last_ = modifiers();
    return {};
#else
    (void)config;
    return lx::not_implemented("lx::input::keyboard_keymap::compile");
#endif
}

bool lx::input::keyboard_keymap::update_key(unsigned evdev_keycode, bool pressed) {
#if defined(LUMEN_HAS_INPUT)
    if (!state_)
        return false;
    xkb_state_update_key(static_cast<xkb_state*>(state_), to_xkb(evdev_keycode),
                         pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    const auto now = modifiers();
    const bool changed = !(now == last_);
    last_ = now;
    return changed;
#else
    (void)evdev_keycode;
    (void)pressed;
    return false;
#endif
}

bool lx::input::keyboard_keymap::update_mask(modifier_state mods) {
#if defined(LUMEN_HAS_INPUT)
    if (!state_)
        return false;
    xkb_state_update_mask(static_cast<xkb_state*>(state_), mods.depressed, mods.latched,
                          mods.locked, 0, 0, mods.group);
    const auto now = modifiers();
    const bool changed = !(now == last_);
    last_ = now;
    return changed;
#else
    (void)mods;
    return false;
#endif
}

lx::input::modifier_state lx::input::keyboard_keymap::modifiers() const {
#if defined(LUMEN_HAS_INPUT)
    if (!state_)
        return {};
    auto* st = static_cast<xkb_state*>(state_);
    modifier_state out{};
    out.depressed = xkb_state_serialize_mods(st, XKB_STATE_MODS_DEPRESSED);
    out.latched = xkb_state_serialize_mods(st, XKB_STATE_MODS_LATCHED);
    out.locked = xkb_state_serialize_mods(st, XKB_STATE_MODS_LOCKED);
    out.group = xkb_state_serialize_layout(st, XKB_STATE_LAYOUT_EFFECTIVE);
    return out;
#else
    return {};
#endif
}

lx::input::named_modifiers lx::input::keyboard_keymap::named() const {
#if defined(LUMEN_HAS_INPUT)
    if (!state_)
        return {};
    auto* st = static_cast<xkb_state*>(state_);
    named_modifiers out{};
    out.shift = mod_active(st, XKB_MOD_NAME_SHIFT);
    out.ctrl = mod_active(st, XKB_MOD_NAME_CTRL);
    out.alt = mod_active(st, XKB_MOD_NAME_ALT);
    out.super = mod_active(st, XKB_MOD_NAME_LOGO);
    out.caps_lock = mod_active(st, XKB_MOD_NAME_CAPS);
    out.num_lock = mod_active(st, XKB_MOD_NAME_NUM);
    return out;
#else
    return {};
#endif
}

unsigned lx::input::keyboard_keymap::keysym(unsigned evdev_keycode) const {
#if defined(LUMEN_HAS_INPUT)
    if (!state_)
        return 0;
    return xkb_state_key_get_one_sym(static_cast<xkb_state*>(state_), to_xkb(evdev_keycode));
#else
    (void)evdev_keycode;
    return 0;
#endif
}

unsigned lx::input::keyboard_keymap::utf8(unsigned evdev_keycode, char* out,
                                          unsigned capacity) const {
#if defined(LUMEN_HAS_INPUT)
    if (!state_ || !out || capacity == 0)
        return 0;
    const int n = xkb_state_key_get_utf8(static_cast<xkb_state*>(state_),
                                         to_xkb(evdev_keycode), out, capacity);
    return n > 0 ? static_cast<unsigned>(n) : 0u;
#else
    (void)evdev_keycode;
    if (out && capacity > 0)
        out[0] = '\0';
    return 0;
#endif
}

unsigned lx::input::keyboard_keymap::level1_keysym(unsigned evdev_keycode,
                                                   unsigned layout) const {
#if defined(LUMEN_HAS_INPUT)
    if (!keymap_)
        return 0;
    const xkb_keysym_t* syms = nullptr;
    const int n = xkb_keymap_key_get_syms_by_level(static_cast<xkb_keymap*>(keymap_),
                                                   to_xkb(evdev_keycode), layout, 0, &syms);
    return n > 0 && syms ? syms[0] : 0u;
#else
    (void)evdev_keycode;
    (void)layout;
    return 0;
#endif
}

unsigned lx::input::keyboard_keymap::active_layout() const {
#if defined(LUMEN_HAS_INPUT)
    if (!state_)
        return 0;
    return xkb_state_serialize_layout(static_cast<xkb_state*>(state_), XKB_STATE_LAYOUT_EFFECTIVE);
#else
    return 0;
#endif
}

bool lx::input::keyboard_keymap::layout_has_keysym(unsigned keysym, unsigned layout) const {
#if defined(LUMEN_HAS_INPUT)
    if (!keymap_ || keysym == 0)
        return false;
    auto* map = static_cast<xkb_keymap*>(keymap_);
    const xkb_keycode_t lo = xkb_keymap_min_keycode(map);
    const xkb_keycode_t hi = xkb_keymap_max_keycode(map);
    for (xkb_keycode_t kc = lo; kc <= hi && kc != 0; ++kc) {
        const xkb_keysym_t* syms = nullptr;
        const int n = xkb_keymap_key_get_syms_by_level(map, kc, layout, 0, &syms);
        for (int i = 0; i < n; ++i)
            if (syms[i] == keysym)
                return true;
    }
    return false;
#else
    (void)keysym;
    (void)layout;
    return false;
#endif
}

bool lx::input::keyboard_keymap::reset_depressed() {
#if defined(LUMEN_HAS_INPUT)
    if (!state_ || !keymap_)
        return false;
    auto* old = static_cast<xkb_state*>(state_);
    const xkb_mod_mask_t locked = xkb_state_serialize_mods(old, XKB_STATE_MODS_LOCKED);
    const xkb_layout_index_t locked_layout =
        xkb_state_serialize_layout(old, XKB_STATE_LAYOUT_LOCKED);
    auto* fresh = xkb_state_new(static_cast<xkb_keymap*>(keymap_));
    if (!fresh)
        return false;
    xkb_state_update_mask(fresh, 0, 0, locked, 0, 0, locked_layout);
    xkb_state_unref(old);
    state_ = fresh;
    const auto now = modifiers();
    const bool changed = !(now == last_);
    last_ = now;
    return changed;
#else
    return false;
#endif
}

unsigned lx::input::keyboard_keymap::led_mask() const {
#if defined(LUMEN_HAS_INPUT)
    if (!state_)
        return 0;
    auto* st = static_cast<xkb_state*>(state_);
    unsigned leds = 0;
    if (xkb_state_led_name_is_active(st, XKB_LED_NAME_NUM) > 0)
        leds |= 1u;
    if (xkb_state_led_name_is_active(st, XKB_LED_NAME_CAPS) > 0)
        leds |= 2u;
    if (xkb_state_led_name_is_active(st, XKB_LED_NAME_SCROLL) > 0)
        leds |= 4u;
    return leds;
#else
    return 0;
#endif
}

bool lx::input::keyboard_keymap::repeats(unsigned evdev_keycode) const {
#if defined(LUMEN_HAS_INPUT)
    if (!keymap_)
        return false;
    return xkb_keymap_key_repeats(static_cast<xkb_keymap*>(keymap_), to_xkb(evdev_keycode)) > 0;
#else
    (void)evdev_keycode;
    return false;
#endif
}

char* lx::input::keyboard_keymap::keymap_string() const {
#if defined(LUMEN_HAS_INPUT)
    if (!keymap_)
        return nullptr;
    return xkb_keymap_get_as_string(static_cast<xkb_keymap*>(keymap_),
                                    XKB_KEYMAP_FORMAT_TEXT_V1);
#else
    return nullptr;
#endif
}

void lx::input::keyboard_keymap::free_keymap_string(char* s) {
#if defined(LUMEN_HAS_INPUT)
    if (s)
        ::free(s);
#else
    (void)s;
#endif
}

lx::result<lx::unique_fd> lx::input::keyboard_keymap::keymap_fd(unsigned& size_out) const {
#if defined(LUMEN_HAS_INPUT)
    size_out = 0;
    char* text = keymap_string();
    if (!text)
        return lx::make_error(lx::error_domain::io, 0, "keymap has not been compiled");
    const unsigned len = static_cast<unsigned>(std::strlen(text)) + 1u;
    auto fd = sealed_keymap_fd(text, len);
    free_keymap_string(text);
    if (fd)
        size_out = len;
    return fd;
#else
    size_out = 0;
    return lx::not_implemented("lx::input::keyboard_keymap::keymap_fd");
#endif
}

lx::result<lx::unique_fd> lx::input::normalize_keymap(const char* text, unsigned long len,
                                                     unsigned& size_out) {
#if defined(LUMEN_HAS_INPUT)
    size_out = 0;
    while (text && len > 0 && text[len - 1] == '\0')
        --len;
    if (!text || len == 0)
        return lx::make_error(lx::error_domain::invalid_argument, 0, "empty keymap");
    xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx)
        return lx::make_error(lx::error_domain::io, 0, "xkb_context_new failed");
    xkb_keymap* keymap = xkb_keymap_new_from_buffer(ctx, text, len, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
    xkb_context_unref(ctx);
    if (!keymap)
        return lx::make_error(lx::error_domain::invalid_argument, 0, "keymap does not compile");
    char* out = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    xkb_keymap_unref(keymap);
    if (!out)
        return lx::make_error(lx::error_domain::io, 0, "keymap serialization failed");
    const unsigned out_len = static_cast<unsigned>(std::strlen(out)) + 1u;
    auto fd = sealed_keymap_fd(out, out_len);
    ::free(out);
    if (fd)
        size_out = out_len;
    return fd;
#else
    (void)text;
    (void)len;
    size_out = 0;
    return lx::not_implemented("lx::input::normalize_keymap");
#endif
}

lx::result<lx::unique_fd> lx::input::normalize_keymap_fd(int fd, unsigned long size,
                                                        unsigned& size_out) {
#if defined(LUMEN_HAS_INPUT)
    size_out = 0;
    if (fd < 0 || size == 0 || size > k_max_client_keymap)
        return lx::make_error(lx::error_domain::invalid_argument, 0, "keymap size");
    // Once per keymap request, never per key: a heap copy is the price of not mapping a
    // file the client controls.
    char* text = new (std::nothrow) char[size];
    if (!text)
        return lx::make_error(lx::error_domain::io, 0, "keymap buffer allocation failed");
    unsigned long got = 0;
    while (got < size) {
        const ssize_t n = ::pread(fd, text + got, size - got, static_cast<off_t>(got));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += static_cast<unsigned long>(n);
    }
    if (got != size) {
        delete[] text;
        return lx::make_error(lx::error_domain::invalid_argument, 0,
                              "keymap file shorter than its size");
    }
    auto out = normalize_keymap(text, size, size_out);
    delete[] text;
    return out;
#else
    (void)fd;
    (void)size;
    size_out = 0;
    return lx::not_implemented("lx::input::normalize_keymap_fd");
#endif
}
