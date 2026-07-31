module;

import lx.foundation;
import lx.gfx;
import lx.scene;
import lx.wayland.server;

export module lx.compositor:surface;

export namespace lx::compositor {

/// Content hint — reserved for future scanout / compositing priority.
enum class content_hint { normal, video, game, subsurface_ui };

/// How a client buffer's storage reaches the compositor. shm is not a dmabuf and must
/// not be pushed through the dmabuf import path.
enum class buffer_kind { none, dmabuf, shm };

struct surface_commit {
    lx::surface_id surface{};
    lx::buffer_id buffer{};
    lx::client_id client{};
    lx::rect2i damage{};
    lx::gfx::syncobj_timeline sync{};
    content_hint hint = content_hint::normal;
    bool fullscreen = false;
    bool opaque = true;
};

class surface_manager {
public:
    void set_import_cache(lx::gfx::dmabuf_import_cache* cache);
    void set_scene(lx::scene::scene_graph* graph);

    /// Register a dmabuf description for a buffer (client attach path). Takes ownership
    /// of the plane FDs in `desc`.
    void register_dmabuf(lx::buffer_id buffer, lx::gfx::dmabuf_desc desc);

    /// Register a shared-memory buffer. Its pixels are uploaded by the render backend
    /// against the texture id returned by `texture_for`, not imported as a dmabuf.
    void register_shm(lx::buffer_id buffer, unsigned width, unsigned height,
                      lx::fourcc format);

    /// Stable texture id assigned to a registered shm buffer, or 0.
    [[nodiscard]] unsigned texture_for(lx::buffer_id buffer) const;

    [[nodiscard]] lx::result<void> on_commit(surface_commit commit);

    /// Metadata-only copy for callers that must not take plane FD ownership. The import
    /// path must not use this — an FD-less desc cannot be imported.
    [[nodiscard]] lx::gfx::dmabuf_desc extract_dmabuf_desc(lx::buffer_id buffer) const;
    void attach_imported(lx::surface_id surface, lx::gfx::imported_image image);
    void destroy(lx::surface_id surface);
    void set_override_redirect(lx::surface_id surface, bool value);
    void set_content_hint(lx::surface_id surface, content_hint hint);

    [[nodiscard]] lx::gfx::imported_image lookup(lx::surface_id surface) const;
    [[nodiscard]] content_hint content_hint_for(lx::surface_id surface) const;

private:
    static constexpr unsigned k_max_surfaces = 128;
    static constexpr unsigned k_max_buffers = 256;

    struct surface_entry {
        lx::surface_id id{};
        lx::buffer_id buffer{};
        lx::gfx::imported_image image{};
        content_hint hint = content_hint::normal;
        lx::scene::surface_node* node = nullptr;
        bool override_redirect = false;
        bool used = false;
    };

    struct buffer_entry {
        lx::buffer_id id{};
        lx::gfx::dmabuf_desc desc{};
        buffer_kind kind = buffer_kind::none;
        unsigned texture_id = 0;
        bool used = false;
    };

    [[nodiscard]] int find_surface(lx::surface_id id) const;
    [[nodiscard]] int find_or_alloc_surface(lx::surface_id id);
    [[nodiscard]] int find_buffer(lx::buffer_id id) const;

    lx::gfx::dmabuf_import_cache* import_cache_ = nullptr;
    lx::scene::scene_graph* scene_ = nullptr;
    surface_entry surfaces_[k_max_surfaces]{};
    buffer_entry buffers_[k_max_buffers]{};
    /// shm texture ids live in a space disjoint from dmabuf import ids.
    unsigned next_shm_texture_id_ = 0x4000'0000u;
};

} // namespace lx::compositor


void lx::compositor::surface_manager::set_import_cache(lx::gfx::dmabuf_import_cache* cache) {
    import_cache_ = cache;
}

void lx::compositor::surface_manager::set_scene(lx::scene::scene_graph* graph) {
    scene_ = graph;
}

void lx::compositor::surface_manager::register_dmabuf(lx::buffer_id buffer,
                                                      lx::gfx::dmabuf_desc desc) {
    if (!buffer)
        return;
    if (const int existing = find_buffer(buffer); existing >= 0) {
        auto& entry = buffers_[static_cast<unsigned>(existing)];
        entry.desc = static_cast<lx::gfx::dmabuf_desc&&>(desc);
        entry.kind = buffer_kind::dmabuf;
        return;
    }
    for (unsigned i = 0; i < k_max_buffers; ++i) {
        if (buffers_[i].used)
            continue;
        buffers_[i].used = true;
        buffers_[i].id = buffer;
        buffers_[i].desc = static_cast<lx::gfx::dmabuf_desc&&>(desc);
        buffers_[i].kind = buffer_kind::dmabuf;
        return;
    }
}

void lx::compositor::surface_manager::register_shm(lx::buffer_id buffer, unsigned width,
                                                   unsigned height, lx::fourcc format) {
    if (!buffer || width == 0 || height == 0)
        return;

    int slot = find_buffer(buffer);
    if (slot < 0) {
        for (unsigned i = 0; i < k_max_buffers; ++i) {
            if (buffers_[i].used)
                continue;
            buffers_[i].used = true;
            buffers_[i].id = buffer;
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0)
        return;

    auto& entry = buffers_[static_cast<unsigned>(slot)];
    entry.kind = buffer_kind::shm;
    entry.desc = {};
    entry.desc.width = width;
    entry.desc.height = height;
    entry.desc.format = format;
    if (entry.texture_id == 0)
        entry.texture_id = next_shm_texture_id_++;
}

unsigned lx::compositor::surface_manager::texture_for(lx::buffer_id buffer) const {
    if (const int slot = find_buffer(buffer); slot >= 0)
        return buffers_[static_cast<unsigned>(slot)].texture_id;
    return 0;
}

int lx::compositor::surface_manager::find_surface(lx::surface_id id) const {
    for (unsigned i = 0; i < k_max_surfaces; ++i)
        if (surfaces_[i].used && surfaces_[i].id == id)
            return static_cast<int>(i);
    return -1;
}

int lx::compositor::surface_manager::find_or_alloc_surface(lx::surface_id id) {
    if (const int existing = find_surface(id); existing >= 0)
        return existing;
    for (unsigned i = 0; i < k_max_surfaces; ++i) {
        if (surfaces_[i].used)
            continue;
        surfaces_[i].used = true;
        surfaces_[i].id = id;
        return static_cast<int>(i);
    }
    return -1;
}

int lx::compositor::surface_manager::find_buffer(lx::buffer_id id) const {
    for (unsigned i = 0; i < k_max_buffers; ++i)
        if (buffers_[i].used && buffers_[i].id == id)
            return static_cast<int>(i);
    return -1;
}

lx::result<void> lx::compositor::surface_manager::on_commit(surface_commit commit) {
    if (!commit.surface) {
        return lx::make_error(lx::error_domain::invalid_argument, 0, "null surface on commit");
    }

    const int buffer_slot = find_buffer(commit.buffer);
    if (buffer_slot < 0 || buffers_[static_cast<unsigned>(buffer_slot)].kind == buffer_kind::none) {
        // Buffer-less commit is valid Wayland (state-only). Do not invent a placeholder
        // buffer — that masked missing protocol wiring as a successful import.
        if (!commit.buffer)
            return {};
        return lx::make_error(lx::error_domain::wayland,
                              static_cast<int>(lx::wayland_err::protocol_violation),
                              "surface commit with unknown buffer");
    }

    // Import from the registered buffer itself, not a metadata copy: dmabuf_desc owns its
    // plane FDs, and a copy leaves them at -1 so nothing would ever reach Vulkan.
    const auto& buffer_entry_ref = buffers_[static_cast<unsigned>(buffer_slot)];
    const lx::gfx::dmabuf_desc& desc = buffer_entry_ref.desc;
    if (!desc.width || !desc.height) {
        return lx::make_error(lx::error_domain::wayland,
                              static_cast<int>(lx::wayland_err::protocol_violation),
                              "surface commit with zero-sized buffer");
    }

    lx::gfx::imported_image image{};
    if (buffer_entry_ref.kind == buffer_kind::shm) {
        // shm pixels are uploaded by the render backend against a stable texture id;
        // there is no dmabuf to import.
        image.image_id = buffer_entry_ref.texture_id;
        image.memory_id = image.image_id;
        image.width = desc.width;
        image.height = desc.height;
        image.format = desc.format;
    } else if (import_cache_) {
        if (!desc.plane_count) {
            return lx::make_error(lx::error_domain::wayland,
                                  static_cast<int>(lx::wayland_err::protocol_violation),
                                  "dmabuf buffer with no planes");
        }
        lx::gfx::import_cache_key key{};
        key.client = commit.client ? commit.client
                                   : lx::client_id{static_cast<lx::client_id::id_type>(commit.surface.id())};
        key.buffer = commit.buffer ? commit.buffer
                                   : lx::buffer_id{static_cast<lx::buffer_id::id_type>(commit.surface.id())};
        key.modifier = desc.planes[0].modifier;
        auto acquired = import_cache_->acquire(key, desc);
        if (!acquired)
            return acquired.get_error();
        image = static_cast<decltype(acquired)&&>(acquired).value();
    } else {
        lx::gfx::dmabuf_importer soft{};
        auto imported = soft.import(desc);
        if (!imported)
            return imported.get_error();
        image = static_cast<decltype(imported)&&>(imported).value();
    }

    attach_imported(commit.surface, image);

    const int slot = find_or_alloc_surface(commit.surface);
    if (slot < 0) {
        return lx::make_error(lx::error_domain::wayland, 0, "surface table full");
    }
    surfaces_[static_cast<unsigned>(slot)].buffer = commit.buffer;
    surfaces_[static_cast<unsigned>(slot)].hint = commit.hint;
    surfaces_[static_cast<unsigned>(slot)].image = image;

    if (scene_) {
        auto& entry = surfaces_[static_cast<unsigned>(slot)];
        if (!entry.node) {
            entry.node = new lx::scene::surface_node{commit.surface};
            if (!scene_->root().add(entry.node)) {
                delete entry.node;
                entry.node = nullptr;
                return lx::make_error(lx::error_domain::wayland, 0, "scene child overflow");
            }
        }
        entry.node->attach(image);
        if (commit.damage.width > 0 && commit.damage.height > 0)
            entry.node->set_bounds(commit.damage);
        else if (desc.width > 0 && desc.height > 0)
            entry.node->set_bounds({0, 0, static_cast<int>(desc.width), static_cast<int>(desc.height)});
        if (commit.damage.width > 0 && commit.damage.height > 0)
            scene_->note_damage(commit.damage);
    }

    return {};
}

lx::gfx::dmabuf_desc lx::compositor::surface_manager::extract_dmabuf_desc(lx::buffer_id buffer) const {
    // Metadata-only copy — plane FDs stay with the registered buffer entry (move-only).
    lx::gfx::dmabuf_desc out{};
    if (const int slot = find_buffer(buffer); slot >= 0) {
        const auto& src = buffers_[static_cast<unsigned>(slot)].desc;
        out.width = src.width;
        out.height = src.height;
        out.format = src.format;
        out.plane_count = src.plane_count;
        for (unsigned i = 0; i < src.plane_count && i < 4; ++i) {
            out.planes[i].offset = src.planes[i].offset;
            out.planes[i].stride = src.planes[i].stride;
            out.planes[i].modifier = src.planes[i].modifier;
        }
    }
    return out;
}

void lx::compositor::surface_manager::attach_imported(lx::surface_id surface,
                                                      lx::gfx::imported_image image) {
    const int slot = find_or_alloc_surface(surface);
    if (slot < 0)
        return;
    surfaces_[static_cast<unsigned>(slot)].image = image;
    if (surfaces_[static_cast<unsigned>(slot)].node)
        surfaces_[static_cast<unsigned>(slot)].node->attach(image);
}

void lx::compositor::surface_manager::destroy(lx::surface_id surface) {
    const int slot = find_surface(surface);
    if (slot < 0)
        return;
    auto& entry = surfaces_[static_cast<unsigned>(slot)];
    if (entry.node) {
        if (scene_)
            (void)scene_->root().remove(entry.node);
        delete entry.node;
    }
    entry = {};
}

lx::gfx::imported_image lx::compositor::surface_manager::lookup(lx::surface_id surface) const {
    if (const int slot = find_surface(surface); slot >= 0)
        return surfaces_[static_cast<unsigned>(slot)].image;
    return {};
}

lx::compositor::content_hint lx::compositor::surface_manager::content_hint_for(lx::surface_id surface) const {
    if (const int slot = find_surface(surface); slot >= 0)
        return surfaces_[static_cast<unsigned>(slot)].hint;
    return content_hint::normal;
}

void lx::compositor::surface_manager::set_override_redirect(lx::surface_id surface, bool value) {
    if (const int slot = find_or_alloc_surface(surface); slot >= 0)
        surfaces_[static_cast<unsigned>(slot)].override_redirect = value;
}

void lx::compositor::surface_manager::set_content_hint(lx::surface_id surface,
                                                         content_hint hint) {
    if (const int slot = find_or_alloc_surface(surface); slot >= 0)
        surfaces_[static_cast<unsigned>(slot)].hint = hint;
}
