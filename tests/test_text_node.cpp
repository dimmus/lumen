// lx.ui:text_node off-thread shaping (ADR-010 §4.6, the boundary measured by S4): text of
// `k_text_async_min_bytes` or more is shaped and wrapped on the worker affinity into a
// staging layout, handed back through the UI strand and swapped in, the old layout shown
// meanwhile; smaller text stays inline. A worker thread drains the executor's worker strand;
// the test thread is the UI thread and drains the UI strand.
#include "lumen_test.hpp"

#include <stdlib.h>
#include <unistd.h>

import std;
import lx.foundation;
import lx.gfx;
import lx.runtime;
import lx.scene;
import lx.text;
import lx.ui;

namespace {
/// Every allocation through the replaced operator new (G5: a reconcile of unchanged props
/// allocates nothing).
std::atomic<unsigned long long> g_allocations{0};
} // namespace

void* operator new(std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

namespace lt = lx::text;

/// `lines` lines of Latin, Cyrillic and Arabic words (deterministic).
std::string document(unsigned lines, unsigned seed) {
    static constexpr const char* k_words[] = {
        "the",    "quick", "brown", "fox",    "jumps", "over",   "lazy",  "dog",
        "съешь",  "же",    "ещё",   "этих",   "мягких", "булок", "окно",  "текст",
        "العدد", "هنا",   "والنص", "العربي", "كتاب",  "مدرسة", "glyph", "atlas",
    };
    std::uint64_t state = 0x9E3779B97F4A7C15ull ^ seed;
    std::string out;
    for (unsigned l = 0; l < lines; ++l) {
        for (unsigned w = 0; w < 9; ++w) {
            state ^= state << 13u;
            state ^= state >> 7u;
            state ^= state << 17u;
            if (w)
                out += ' ';
            out += k_words[state % std::size(k_words)];
        }
        out += ' ';
        out += std::to_string(l);
        if (l + 1 < lines)
            out += '\n';
    }
    return out;
}

/// The caches of one test: the block's engine and the worker's, over one face cache.
struct env {
    lt::face_cache faces{};
    lt::shape_cache ui_shapes{};
    lt::itemizer ui_items{};
    lt::shape_cache worker_shapes{
        lt::shape_cache_config{.entries = 16384, .glyph_capacity = 1u << 20}};
    lt::itemizer worker_items{};
    lx::gfx::atlas atlas{};
    lt::glyph_cache glyphs{atlas};
    lx::ui::text_async_shaping async{};

    env() { async.engine = {&faces, &worker_shapes, &worker_items}; }
    [[nodiscard]] lt::text_engine engine() { return {&faces, &ui_shapes, &ui_items}; }
    [[nodiscard]] bool has_fonts() { return faces.resolve(lt::font_desc{}).valid(); }

    [[nodiscard]] lx::ui::text_block_props props(const char* text) {
        lx::ui::text_block_props p{};
        p.text = text;
        p.font = {lt::k_ui_family, 10.5f};
        p.wrap = lt::text_wrap::word;
        p.context = {engine(), &glyphs, &async};
        return p;
    }
};

/// The worker affinity: a thread draining the executor's worker strand until stopped.
class worker_thread {
public:
    worker_thread()
        : thread_{[this] {
              while (!stop_.load(std::memory_order_acquire)) {
                  lx::runtime::executor::global().drain(lx::runtime::affinity::worker);
                  ::usleep(200);
              }
              lx::runtime::executor::global().drain(lx::runtime::affinity::worker);
          }} {}
    ~worker_thread() {
        stop_.store(true, std::memory_order_release);
        thread_.join();
    }

private:
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

/// Drains the UI strand until `done` (or 20 s).
template<typename F>
bool pump(F done) {
    for (int i = 0; i < 20000; ++i) {
        lx::runtime::executor::global().drain(lx::runtime::affinity::ui);
        if (done())
            return true;
        ::usleep(1000);
    }
    return false;
}

/// Lays the node out at `width` px and emits it; the sprites it drew.
unsigned draw(lx::ui::text_block_node& node, env& e, int width) {
    const lx::size2i size = node.measure({{0, 0}, {width, 1 << 28}}, lx::ui::size_hint::preferred);
    node.arrange({0, 0, width, size.height});
    e.atlas.begin_frame(e.atlas.frame() + 1);
    auto list = std::make_unique<lx::scene::draw_list>();
    list->begin({width, 600});
    lx::ui::draw_scope scope{*list, {0, 0, width, 600}};
    node.emit(scope);
    return node.last_emit().sprites;
}

bool skip(env& e, const char* name) {
    if (e.has_fonts())
        return false;
    std::printf("    SKIP %s: no font face on this host\n", name);
    return true;
}

} // namespace

LUMEN_TEST(large_text_is_shaped_on_the_worker) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    auto e = std::make_unique<env>();
    if (skip(*e, "large_text_is_shaped_on_the_worker"))
        return;
    worker_thread worker{};
    const std::string text = document(300, 1);
    LUMEN_CHECK(text.size() >= lx::ui::k_text_async_min_bytes);

    lx::ui::text_block_node node{};
    node.apply(e->props(text.c_str()));
    // Nothing laid out on this thread: the job is with the worker, the block shows nothing.
    lx::ui::text_async_stats st = node.async_stats();
    LUMEN_CHECK(st.active);
    LUMEN_CHECK(st.posted == 1);
    LUMEN_CHECK(node.layout().text().empty());
    (void)draw(node, *e, 700);

    LUMEN_CHECK(pump([&] { return !node.async_stats().pending; }));
    st = node.async_stats();
    LUMEN_CHECK(st.landed == 1);
    LUMEN_CHECK(node.layout().text() == text);
    // The worker's caches shaped it; the block's own shaped nothing of it.
    LUMEN_CHECK(e->worker_shapes.counters().shaping_calls > 0);
    LUMEN_CHECK(e->ui_shapes.counters().shaping_calls == 0);

    // The same lines as an inline layout of the same text.
    lt::shape_cache ref_shapes{
        lt::shape_cache_config{.entries = 16384, .glyph_capacity = 1u << 20}};
    lt::itemizer ref_items{};
    lt::text_layout ref{lt::text_engine{&e->faces, &ref_shapes, &ref_items}};
    ref.set_text(text);
    ref.set_font({lt::k_ui_family, 10.5f});
    ref.set_max_width(700.f);
    LUMEN_CHECK(ref.update().ok());
    const unsigned sprites = draw(node, *e, 700);
    LUMEN_CHECK(node.layout().lines().size() == ref.lines().size());
    LUMEN_CHECK(node.layout().lines().size() >= 300); // a paragraph a line, or more
    LUMEN_CHECK(sprites > 100);
    // Re-wrapping and drawing on the UI thread shapes nothing.
    LUMEN_CHECK(e->ui_shapes.counters().shaping_calls == 0);
}

LUMEN_TEST(old_layout_stays_until_the_new_one_lands) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    auto e = std::make_unique<env>();
    if (skip(*e, "old_layout_stays_until_the_new_one_lands"))
        return;
    worker_thread worker{};
    const std::string a = document(200, 2);
    const std::string b = document(220, 3);
    lx::ui::text_block_node node{};
    node.apply(e->props(a.c_str()));
    LUMEN_CHECK(pump([&] { return !node.async_stats().pending; }));
    LUMEN_CHECK(node.layout().text() == a);
    const std::size_t lines_a = node.layout().lines().size();

    // A new text: until it lands, the block lays out and draws the old one.
    node.apply(e->props(b.c_str()));
    LUMEN_CHECK(node.async_stats().pending);
    LUMEN_CHECK(node.layout().text() == a);
    LUMEN_CHECK(draw(node, *e, 700) > 0);
    LUMEN_CHECK(node.layout().lines().size() == lines_a);
    // Re-applying the same props posts nothing new.
    node.apply(e->props(b.c_str()));
    LUMEN_CHECK(node.async_stats().posted == 2);
    LUMEN_CHECK(pump([&] { return !node.async_stats().pending; }));
    LUMEN_CHECK(node.layout().text() == b);
    LUMEN_CHECK(node.async_stats().landed == 2);

    // Changes while a job runs: the newest inputs win, at most one job in flight.
    const std::string c = document(240, 4);
    node.apply(e->props(a.c_str()));
    node.apply(e->props(c.c_str()));
    LUMEN_CHECK(pump([&] {
        const lx::ui::text_async_stats s = node.async_stats();
        return !s.pending && node.layout().text() == c;
    }));
    LUMEN_CHECK(node.layout().text() == c);
}

LUMEN_TEST(small_text_stays_inline_and_discards_a_running_job) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    auto e = std::make_unique<env>();
    if (skip(*e, "small_text_stays_inline_and_discards_a_running_job"))
        return;
    worker_thread worker{};
    const std::string big = document(200, 5);
    lx::ui::text_block_node node{};
    node.apply(e->props("a label"));
    LUMEN_CHECK(!node.async_stats().active);
    LUMEN_CHECK(node.async_stats().posted == 0);
    LUMEN_CHECK(draw(node, *e, 700) > 0);
    LUMEN_CHECK(node.layout().text() == "a label");

    node.apply(e->props(big.c_str()));
    LUMEN_CHECK(node.async_stats().pending);
    node.apply(e->props("back to a label"));
    // Inline at once, whatever the worker is doing.
    LUMEN_CHECK(node.layout().text() == "back to a label");
    LUMEN_CHECK(pump([&] { return !node.async_stats().pending; }));
    LUMEN_CHECK(node.async_stats().discarded == 1);
    LUMEN_CHECK(node.async_stats().landed == 0);
    LUMEN_CHECK(node.layout().text() == "back to a label");
    LUMEN_CHECK(draw(node, *e, 700) > 0);
}

LUMEN_TEST(node_destroyed_with_a_job_in_flight) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    auto e = std::make_unique<env>();
    if (skip(*e, "node_destroyed_with_a_job_in_flight"))
        return;
    worker_thread worker{};
    const std::string big = document(300, 6);
    auto node = std::make_unique<lx::ui::text_block_node>();
    node->apply(e->props(big.c_str()));
    LUMEN_CHECK(node->async_stats().pending);
    node.reset();
    // The job finishes and lands into nothing: `land_job` frees it.
    lx::runtime::executor& ex = lx::runtime::executor::global();
    LUMEN_CHECK(pump([&] {
        return ex.worker_strand().pending() == 0 && ex.ui_strand().pending() == 0;
    }));
}

LUMEN_TEST(ui_thread_gets_the_face_lock_while_the_worker_shapes) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    auto e = std::make_unique<env>();
    if (skip(*e, "ui_thread_gets_the_face_lock_while_the_worker_shapes"))
        return;
    worker_thread worker{};
    const std::string big = document(2000, 7); // ~130 KB: tens of ms of shaping
    lx::ui::text_block_node node{};
    node.apply(e->props(big.c_str()));
    // What a UI frame does when a glyph misses: take the face lock.
    std::uint64_t worst_ns = 0;
    unsigned attempts = 0;
    while (node.async_stats().pending) {
        const auto t0 = std::chrono::steady_clock::now();
        {
            auto held = e->faces.lock();
        }
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());
        worst_ns = ns > worst_ns ? ns : worst_ns;
        ++attempts;
        lx::runtime::executor::global().drain(lx::runtime::affinity::ui);
        ::usleep(500);
    }
    LUMEN_CHECK(node.layout().text() == big);
    const lt::text_layout_counters c = node.layout().counters();
    std::printf("    %u lock attempts while the worker shaped %zu bytes; worst wait %.2f ms; "
                "the pass yielded %llu times\n",
                attempts, big.size(), static_cast<double>(worst_ns) / 1e6,
                static_cast<unsigned long long>(c.lock_yields));
    // The pass handed the lock over rather than holding it to the end.
    LUMEN_CHECK(attempts < 3 || c.lock_yields > 0);
}

LUMEN_TEST(reapplying_identical_props_allocates_nothing) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    auto e = std::make_unique<env>();
    if (skip(*e, "reapplying_identical_props_allocates_nothing"))
        return;
    const lt::text_attr attrs[] = {
        {.begin = 6, .end = 10, .weight = lt::weight::bold},
        {.begin = 11, .end = 15, .color = 0x3366FFFFu, .underline = lt::text_underline::single},
    };
    lx::ui::text_block_props p = e->props("plain bold link");
    p.context.async = nullptr;
    p.attrs = attrs;
    lx::ui::text_block_node node{};
    node.apply(p);
    LUMEN_CHECK(draw(node, *e, 400) > 0);
    const std::uint64_t generation = node.layout().shape_generation();
    const unsigned long long before = g_allocations.load(std::memory_order_relaxed);
    for (int i = 0; i < 100; ++i)
        node.apply(p);
    const unsigned long long allocated = g_allocations.load(std::memory_order_relaxed) - before;
    LUMEN_CHECK(allocated == 0);
    LUMEN_CHECK(node.layout().shape_generation() == generation && !node.layout().dirty());
}

LUMEN_TEST(a_glyph_that_fails_is_not_retried_every_frame) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    auto e = std::make_unique<env>();
    if (skip(*e, "a_glyph_that_fails_is_not_retried_every_frame"))
        return;
    // Every glyph is larger than this cache's bitmaps: rasterising fails.
    lt::glyph_cache_config tiny_config{};
    tiny_config.max_glyph_px = 2;
    lt::glyph_cache tiny{e->atlas, tiny_config};
    lx::ui::text_block_props p = e->props("Lumen");
    p.context.async = nullptr;
    p.context.glyphs = &tiny;
    lx::ui::text_block_node node{};
    node.apply(p);
    LUMEN_CHECK(draw(node, *e, 400) == 0);
    const unsigned failed = node.last_emit().failures;
    const std::uint64_t tried = tiny.counters().oversize;
    LUMEN_CHECK(failed == 5 && tried == 5);
    // The next frames draw nothing and ask FreeType nothing.
    for (int i = 0; i < 10; ++i) {
        (void)draw(node, *e, 400);
        LUMEN_CHECK(node.last_emit().failures == failed && node.last_emit().rasterized == 0);
    }
    LUMEN_CHECK(tiny.counters().oversize == tried);
    // A stale age later they are tried once more.
    e->atlas.begin_frame(e->atlas.frame() + lx::gfx::k_atlas_stale_frames);
    (void)draw(node, *e, 400);
    LUMEN_CHECK(tiny.counters().oversize == tried + 5);
}

LUMEN_TEST(a_block_whose_face_was_evicted_reshapes) {
    lx::runtime::set_current_affinity(lx::runtime::affinity::ui);
    auto e = std::make_unique<env>();
    if (skip(*e, "a_block_whose_face_was_evicted_reshapes"))
        return;
    lx::ui::text_block_props p = e->props("Retained text");
    p.context.async = nullptr;
    lx::ui::text_block_node node{};
    node.apply(p);
    const unsigned sprites = draw(node, *e, 400);
    LUMEN_CHECK(sprites > 0 && node.last_emit().failures == 0);
    const std::uint64_t passes = node.layout().counters().shape_passes;
    // Fill the face cache's LRU with other faces: the block's face is evicted (its slot
    // reused under a new id), and its glyphs leave the glyph cache.
    const std::uint64_t evicted = e->faces.stats().evictions;
    for (unsigned i = 0; i < lt::k_face_cache_size; ++i) {
        char family[32];
        std::snprintf(family, sizeof family, "Lumen Evict %u", i);
        (void)e->faces.resolve(family, lt::weight::normal, lt::style::normal);
    }
    LUMEN_CHECK(e->faces.stats().evictions >= evicted + 1);
    e->glyphs.clear();
    // This frame finds the face gone (and asks for a relayout); the next one reshapes and
    // draws everything again.
    lx::ui::ui_damage().clear();
    (void)draw(node, *e, 400);
    LUMEN_CHECK(node.last_emit().failures > 0);
    LUMEN_CHECK(lx::ui::ui_damage().needs_layout());
    lx::ui::ui_damage().clear();
    LUMEN_CHECK(draw(node, *e, 400) == sprites);
    LUMEN_CHECK(node.last_emit().failures == 0);
    LUMEN_CHECK(node.layout().counters().shape_passes == passes + 1);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
