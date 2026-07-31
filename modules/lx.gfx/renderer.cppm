module;

import lx.foundation;

export module lx.gfx:renderer;

import :headless;

export namespace lx::gfx {

struct blit_command {
    unsigned texture_id = 0;
    lx::rect2i dst{};
    float opacity = 1.f;
    lx::blend_mode blend = lx::blend_mode::premultiplied;
};

class pipeline_cache {
public:
    [[nodiscard]] unsigned acquire(unsigned blend_key, unsigned format_key);
    void clear();

private:
    unsigned next_id_ = 1;
    static constexpr unsigned k_capacity = 64;
    unsigned keys_[64]{};
    unsigned ids_[64]{};
    unsigned count_ = 0;
};

class frame_renderer {
public:
    void set_pipeline_cache(pipeline_cache* cache);

    [[nodiscard]] lx::result<void> draw_to_headless(const blit_command* cmds, unsigned count,
                                                     headless_backend& target);

private:
    pipeline_cache* pipelines_ = nullptr;
};

} // namespace lx::gfx


unsigned lx::gfx::pipeline_cache::acquire(unsigned blend_key, unsigned format_key) {
    const unsigned key = (blend_key << 16u) | (format_key & 0xFFFFu);
    for (unsigned i = 0; i < count_; ++i) {
        if (keys_[i] == key)
            return ids_[i];
    }
    if (count_ < k_capacity) {
        keys_[count_] = key;
        ids_[count_] = next_id_++;
        return ids_[count_++];
    }
    return next_id_++;
}

void lx::gfx::pipeline_cache::clear() { count_ = 0; }

void lx::gfx::frame_renderer::set_pipeline_cache(pipeline_cache* cache) { pipelines_ = cache; }

lx::result<void> lx::gfx::frame_renderer::draw_to_headless(const blit_command* cmds,
                                                              unsigned count,
                                                              headless_backend& target) {
    if (!cmds)
        return {};
    // Draw everything that resolves, but still report that something did not: an
    // unregistered texture means the commit and render paths disagree.
    lx::result<void> outcome{};
    for (unsigned i = 0; i < count; ++i) {
        const auto& cmd = cmds[i];
        if (pipelines_) {
            (void)pipelines_->acquire(static_cast<unsigned>(cmd.blend), 0);
        }
        if (auto blitted = target.blit_texture(cmd.texture_id, cmd.dst); !blitted)
            outcome = blitted.get_error();
    }
    return outcome;
}
