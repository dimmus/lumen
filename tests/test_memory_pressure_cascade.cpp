// The memory-pressure cascade (AGENTS.md §5: "Memory pressure: `memory_budget_coordinator`
// → evict import cache, flush buffer lifecycle, evict surface pool"; docs/invariants.md, the
// "Memory-pressure cascade" row). One signal, three consumers: the compositor's handler
// (compositor_impl.cpp:2642-2649) answers the coordinator's level by evicting from the
// import cache, releasing from the buffer lifecycle, and trimming the staging pool.
//
// What makes this the cascade and not three unit tests is who triggers it. Pressure is
// raised only through the compositor's own coordinator (`memory()`, lx.compositor.cppm:208),
// by the two ways anything raises it: a pool's own usage (`report_usage`) and an outside
// source's level (`on_external_pressure`, the kernel's PSI). The test never calls a
// consumer's `on_memory_pressure`, never builds a coordinator, a cache or a tracker of its
// own, and reaches the consumers only through the facade: the compositor's import cache
// (`import_cache()`, :198) and its buffer lifecycle (`buffer_lifecycle()`, :207) — which the
// compositor built on that very cache (compositor_impl.cpp:1296-1297).
//
// The compositor is started in-process, as the other compositor tests start one, with one
// difference that keeps a card out of it: virtual outputs ("no card is opened when set" —
// no DRM master, no KMS). Without a card the present path asked for below has nothing to
// run on (compositor_impl.cpp:5406-5408), no Vulkan device is created, and the compositor
// stays on its default device, whose importer is the bookkeeping one (dmabuf_import.cppm:
// 303-312): its own import cache takes plain descriptors. The lifecycle's records carry no
// wl_buffer, so the tracker never calls the compositor's release callback
// (buffer_lifecycle.cppm:193-195). The compositor's loop is never dispatched here, so
// nothing else reports to the coordinator — not the PSI.
//
// What `compositor::start()` does to the session is `start()`'s to do, and this test does
// not make it smaller: a compositor attaches to the logind session it finds for the user
// and opens the libinput seat (compositor_impl.cpp:9592, 9744), virtual outputs or not.
// This is unit evidence about the wiring on one started compositor — one start per process,
// since a third `compositor::start()` in a process fails ("no event source slot") — and it
// is not a claim about a seat.
//
// Not driven: the third leg, `shm_pool_.trim()` (compositor_impl.cpp:2648-2649). The pool is
// private (:1006) and nothing on the facade exposes it. Its bytes are readable only as
// `health_inputs::staging_bytes` (:4114), built by `answer_health` (:4077) for the control
// socket, and filling its cache takes a client committing shm buffers. The report at the
// end says so in the output.
#include "lumen_test.hpp"

import std;
import lx.foundation;
import lx.runtime;
import lx.gfx;
import lx.compositor;

namespace {

namespace rt = lx::runtime;
namespace cmp = lx::compositor;
namespace gfx = lx::gfx;

/// `LUMEN_CHECK` repeats its expression; these checks say which step and which buffer.
void expect(bool ok, const std::string& what) {
    if (!ok)
        throw lumen_test::assertion_error(what);
}

// ── The compositor ─────────────────────────────────────────────────────────────────────

/// A started compositor on one virtual output, with the GL present path asked for (the file
/// comment says why). `start()` makes the calling thread the UI thread; the affinity it had
/// is given back once the compositor is gone.
struct started_compositor {
    rt::affinity saved = rt::current_affinity();
    std::unique_ptr<cmp::compositor> comp;

    explicit started_compositor(const char* socket) {
        cmp::config cfg{};
        cfg.socket_name = socket;
        cfg.target_fps = 30.0;
        cfg.present = cmp::present_backend::gl;
        cfg.virtual_outputs = "640x480";
        comp = std::make_unique<cmp::compositor>(cfg);
        const auto started = comp->start();
        expect(started.has_value(),
               std::format("the compositor must start: {}",
                           started.has_value() ? "" : started.get_error().message));
        // No card behind the outputs.
        const cmp::output_manager& outputs = comp->outputs();
        expect(outputs.count() == 1, "one virtual output must be up");
        expect(outputs.state(outputs.nth(0)).virtual_output,
               "the output must be a virtual one: no card is to be opened");
        // The handler is registered by `start()` (compositor_impl.cpp:10280): the cascade
        // does not exist on a compositor that was only constructed.
        expect(comp->memory().current_pressure() == rt::memory_pressure::normal,
               "a started compositor must begin under no pressure");
        // What lets plain descriptors into the compositor's own cache: no Vulkan device.
        expect(comp->graphics().vk_device() == nullptr,
               "this fixture needs the compositor on its default device (a Vulkan device was "
               "created all the same)");
        expect(!comp->graphics().dmabuf().is_hardware(),
               "the importer must be the bookkeeping one");
    }
    ~started_compositor() {
        comp.reset();
        rt::set_current_affinity(saved);
    }
    started_compositor(const started_compositor&) = delete;
    started_compositor& operator=(const started_compositor&) = delete;
};

// ── Filling the compositor's cache and tracker through the facade ──────────────────────

gfx::dmabuf_desc plain_desc() {
    gfx::dmabuf_desc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.plane_count = 1;
    desc.planes[0].stride = 64 * 4;
    return desc;
}

gfx::import_cache_key key_of(lx::client_id client, unsigned buffer) {
    gfx::import_cache_key key{};
    key.client = client;
    key.buffer = lx::buffer_id{buffer};
    return key;
}

/// The consumers as a scenario sees them, with the eviction count it began from.
struct consumers {
    lx::gfx::dmabuf_import_cache& cache;
    cmp::buffer_lifecycle_tracker& life;
    unsigned evicted_from;

    explicit consumers(started_compositor& c)
        : cache(c.comp->import_cache()),
          life(c.comp->buffer_lifecycle()),
          evicted_from(cache.evictions()) {}
    /// Buffers pressure evicted since the scenario began.
    unsigned evicted() const { return cache.evictions() - evicted_from; }
};

/// What the compositor holds once a scenario has filled it, in the order it was used.
struct filled {
    static constexpr unsigned k_spare = 8;
    /// Imported and let go: the cache's spare capacity, what an eviction can take.
    gfx::import_cache_key spare[k_spare]{};
    /// Surface 1's last buffer. Its frame is done and a newer commit superseded it: nothing
    /// samples it, so it waits to go back to the client.
    gfx::import_cache_key done{};
    /// Surface 1's buffer now: on screen, never released.
    gfx::import_cache_key shown{};
    /// Surface 2's last buffer. A submitted frame still samples it, and a newer commit
    /// superseded it: in flight.
    gfx::import_cache_key flying{};
    /// Surface 2's buffer now: on screen, never released.
    gfx::import_cache_key shown2{};

    /// The last frame the cache was ticked for. Every buffer is used on a frame of its own,
    /// in this order, so among the spare buffers the cache's LRU order is the order of this
    /// list. The four still held were all in use at the last tick, so they are all as recent
    /// as each other.
    unsigned frame = 0;
};

/// Fills the compositor's own import cache and its own buffer lifecycle for `client`, the
/// way commits do: an import from the cache, a record on the tracker. `cache.tick` is what
/// stamps a buffer's last use (the frame's), so it is called for each.
filled fill(started_compositor& c, lx::client_id client, unsigned frame_from) {
    auto& cache = c.comp->import_cache();
    auto& life = c.comp->buffer_lifecycle();
    const auto desc = plain_desc();
    const lx::surface_id one{1};
    const lx::surface_id two{2};
    filled f;
    f.frame = frame_from;
    f.done = key_of(client, 101);
    f.shown = key_of(client, 102);
    f.flying = key_of(client, 201);
    f.shown2 = key_of(client, 202);
    for (unsigned k = 0; k < f.k_spare; ++k) {
        f.spare[k] = key_of(client, 1 + k);
        expect(cache.acquire(f.spare[k], desc).has_value(), "the cache must take a spare buffer");
        cache.tick(++f.frame);
        cache.release(f.spare[k]);
    }
    // Surface 1: `done` shown, its frame presented and rendered; then `shown` committed.
    expect(cache.acquire(f.done, desc).has_value(), "the cache must take surface 1's first buffer");
    expect(life.on_import(client, one, f.done.buffer, f.done, nullptr).ok(),
           "the tracker must take a record");
    life.on_attach(one);
    life.on_present(one, f.frame);
    life.on_render_done(one, f.frame);
    cache.tick(++f.frame);
    expect(cache.acquire(f.shown, desc).has_value(), "the cache must take surface 1's next buffer");
    expect(life.on_import(client, one, f.shown.buffer, f.shown, nullptr).ok(),
           "the tracker must take a record");
    cache.tick(++f.frame);
    // Surface 2: `flying` tagged on a submitted frame; then `shown2` committed.
    expect(cache.acquire(f.flying, desc).has_value(),
           "the cache must take surface 2's first buffer");
    expect(life.on_import(client, two, f.flying.buffer, f.flying, nullptr).ok(),
           "the tracker must take a record");
    life.on_attach(two);
    life.on_frame_submitted(f.frame + 1);
    cache.tick(++f.frame);
    expect(cache.acquire(f.shown2, desc).has_value(),
           "the cache must take surface 2's next buffer");
    expect(life.on_import(client, two, f.shown2.buffer, f.shown2, nullptr).ok(),
           "the tracker must take a record");
    cache.tick(++f.frame);

    // What was built, before any pressure.
    expect(cache.size() == f.k_spare + 4, "eight spare and four shown buffers must be cached");
    expect(life.pending_release_count() == 1, "surface 1's superseded buffer must wait to go back");
    expect(life.has_unreleased_frames(), "surface 2's superseded buffer must be in flight");
    expect(f.frame < cache.config().stale_frame_threshold, "no buffer may be stale yet");
    return f;
}

/// Raises the level the way a pool's own usage does: the UI arena at `pct` percent.
void report_ui_usage(started_compositor& c, unsigned pct) {
    c.comp->memory().report_usage(rt::memory_pool::ui, pct, 100u);
}

/// Whether the compositor's cache still holds `key`. It takes the buffer again to find out,
/// so each buffer is asked once, at the end of a scenario.
bool cached(lx::gfx::dmabuf_import_cache& cache, const gfx::import_cache_key& key) {
    const unsigned hits = cache.hits();
    const unsigned misses = cache.misses();
    (void)cache.acquire(key, plain_desc());
    return cache.hits() == hits + 1u && cache.misses() == misses;
}

// ── The scenarios ──────────────────────────────────────────────────────────────────────

// Moderate (import_cache.cppm:227-233: a quarter of the count and one, least recently used
// first and only what nothing holds; buffer_lifecycle.cppm:404-417: what no frame samples).
// Raised by a pool's own usage, the way the compositor's arenas do it.
void moderate_by_a_pools_usage(started_compositor& c, unsigned frame_from) {
    const filled f = fill(c, lx::client_id{7}, frame_from);
    consumers u{c};

    report_ui_usage(c, c.comp->memory().config().pressure_moderate_used_pct);
    expect(c.comp->memory().current_pressure() == rt::memory_pressure::moderate,
           "a pool at the moderate threshold must put the coordinator at moderate");

    // The cache: 12 buffers, a quarter of the count and one — the four used longest ago, all
    // of them spare.
    expect(u.evicted() == 4, "moderate must evict a quarter of 12 buffers and one: 4");
    expect(u.cache.size() == 8, "moderate must leave 8 of 12 buffers cached");
    // The lifecycle: the buffer nothing samples goes back; the one a frame still samples stays.
    expect(u.life.pending_release_count() == 0,
           "moderate must release the superseded, rendered buffer");
    expect(u.life.has_unreleased_frames(),
           "moderate must keep the superseded buffer a frame samples");
    expect(u.life.state_for(lx::surface_id{2}) == cmp::buffer_lifecycle_state::in_flight,
           "surface 2's superseded buffer must still be in flight under moderate pressure");

    expect(!cached(u.cache, f.spare[0]) && !cached(u.cache, f.spare[1]) &&
               !cached(u.cache, f.spare[2]) && !cached(u.cache, f.spare[3]),
           "moderate must evict the four spare buffers used longest ago");
    expect(cached(u.cache, f.spare[4]) && cached(u.cache, f.spare[7]),
           "moderate must keep the spare buffers used more recently");
    expect(cached(u.cache, f.done), "moderate must keep the released buffer's import cached");
    expect(cached(u.cache, f.shown) && cached(u.cache, f.flying) && cached(u.cache, f.shown2),
           "moderate must not evict a buffer the compositor still holds");
}

// Critical (import_cache.cppm:227-233: half the count and one; buffer_lifecycle.cppm:
// 412-414: what no frame samples and what a submitted frame still samples). Raised by an
// outside source's level, the way the kernel's PSI does it.
void critical_by_an_outside_source(started_compositor& c, unsigned frame_from) {
    const filled f = fill(c, lx::client_id{8}, frame_from);
    consumers u{c};

    c.comp->memory().on_external_pressure(rt::memory_pressure::critical);
    expect(c.comp->memory().current_pressure() == rt::memory_pressure::critical,
           "an outside source's critical level must put the coordinator at critical");

    // The cache: 12 buffers, half the count and one — the seven used longest ago, all spare.
    expect(u.evicted() == 7, "critical must evict half of 12 buffers and one: 7");
    expect(u.cache.size() == 5, "critical must leave 5 of 12 buffers cached");
    // The lifecycle: both superseded buffers go back.
    expect(u.life.pending_release_count() == 0,
           "critical must release the superseded, rendered buffer");
    expect(!u.life.has_unreleased_frames(),
           "critical must release the superseded buffer a frame samples");
    expect(u.life.state_for(lx::surface_id{2}) == cmp::buffer_lifecycle_state::imported,
           "surface 2's superseded buffer must be gone under critical pressure, its current one "
           "kept");

    expect(!cached(u.cache, f.spare[0]) && !cached(u.cache, f.spare[6]),
           "critical must evict the spare buffers used longest ago");
    expect(cached(u.cache, f.spare[7]), "critical must keep the spare buffer used most recently");
    expect(cached(u.cache, f.done) && cached(u.cache, f.flying),
           "a buffer the lifecycle released is no longer held, but its import stays cached");
    expect(cached(u.cache, f.shown) && cached(u.cache, f.shown2),
           "critical must not evict a buffer the compositor still holds");
}

// No pressure: nothing is evicted, and a signal that leaves the level where it is reaches
// no consumer at all (memory.cppm:412: the handler fires only on a change). The way back to
// normal from moderate changes nothing more.
void no_pressure_and_the_way_back(started_compositor& c, unsigned frame_from) {
    fill(c, lx::client_id{9}, frame_from);
    consumers u{c};

    // The level is normal, and stays normal: by usage, and by an outside source.
    report_ui_usage(c, 10);
    c.comp->memory().on_external_pressure(rt::memory_pressure::normal);
    expect(c.comp->memory().current_pressure() == rt::memory_pressure::normal,
           "a pool at a tenth of its budget must leave the coordinator at normal");
    expect(u.evicted() == 0 && u.cache.size() == 12, "no pressure must evict nothing");
    expect(u.life.pending_release_count() == 1 && u.life.has_unreleased_frames(),
           "no pressure must release nothing");

    // Moderate, then back to normal: the way back changes nothing more.
    report_ui_usage(c, c.comp->memory().config().pressure_moderate_used_pct);
    expect(u.evicted() == 4, "moderate must evict 4");
    expect(u.life.pending_release_count() == 0,
           "moderate must release the superseded, rendered buffer");
    const auto state_of_two = u.life.state_for(lx::surface_id{2});
    report_ui_usage(c, 10);
    expect(c.comp->memory().current_pressure() == rt::memory_pressure::normal,
           "a pool back under the thresholds must put the coordinator back at normal");
    expect(u.evicted() == 4 && u.cache.size() == 8,
           "the way back to normal must evict nothing more");
    expect(u.life.has_unreleased_frames() && u.life.state_for(lx::surface_id{2}) == state_of_two,
           "the way back to normal must release nothing more");
}

// The handler fires once per change of level, however the level is signalled again
// (memory.cppm:407-413) — and what the lifecycle released is what the cache can then evict:
// the two legs of the cascade meet in the import cache.
void once_per_level_change_and_the_lifecycle_feeds_the_cache(started_compositor& c,
                                                             unsigned frame_from) {
    const filled f = fill(c, lx::client_id{10}, frame_from);
    consumers u{c};
    auto& memory = c.comp->memory();

    report_ui_usage(c, memory.config().pressure_critical_used_pct);
    expect(memory.current_pressure() == rt::memory_pressure::critical, "critical by usage");
    expect(u.evicted() == 7 && u.cache.size() == 5, "the first critical must evict 7");
    expect(u.life.pending_release_count() == 0 && !u.life.has_unreleased_frames(),
           "the first critical must release both superseded buffers");

    // Critical again, by both ways, and from a little further over the threshold: no change.
    for (unsigned again = 0; again < 8; ++again) {
        report_ui_usage(c, memory.config().pressure_critical_used_pct + 1 + again);
        memory.on_external_pressure(rt::memory_pressure::critical);
    }
    expect(memory.current_pressure() == rt::memory_pressure::critical, "still critical");
    expect(u.evicted() == 7 && u.cache.size() == 5,
           "a level that did not change must not reach the cache again");
    expect(!u.life.has_unreleased_frames(),
           "a level that did not change must not reach the tracker again");

    // A real change, down to moderate: the handler runs, on a cache of 5. What it can take:
    // the last spare buffer, and the first of the two imports the lifecycle let go of.
    memory.on_external_pressure(rt::memory_pressure::normal);
    report_ui_usage(c, memory.config().pressure_moderate_used_pct);
    expect(memory.current_pressure() == rt::memory_pressure::moderate,
           "an outside source's normal level must let the pools' level show");
    expect(u.evicted() == 7 + 2 && u.cache.size() == 3,
           "the change to moderate must evict a quarter of the 5 left and one: 2");

    // Critical once more: of the 3 left, the only one an eviction can take is the other
    // import the lifecycle released. The two on screen are never taken.
    report_ui_usage(c, memory.config().pressure_critical_used_pct);
    expect(u.evicted() == 7 + 2 + 1 && u.cache.size() == 2,
           "critical must now evict the other import the lifecycle released, and nothing else");
    expect(cached(u.cache, f.shown) && cached(u.cache, f.shown2),
           "the two buffers on screen must never be evicted");
    expect(!cached(u.cache, f.done) && !cached(u.cache, f.flying),
           "the two imports the lifecycle released must be the ones evicted");
}

/// A scenario: on the compositor, for one client, filling from frame `frame_from`.
using scenario_fn = void (*)(started_compositor&, unsigned frame_from);

struct scenario {
    const char* name;
    lx::client_id client;
    scenario_fn run;
};

const scenario k_scenarios[]{
    {"moderate, by a pool's own usage: a quarter of the cache and what no frame samples",
     lx::client_id{7}, &moderate_by_a_pools_usage},
    {"critical, by an outside source: half the cache and what a frame still samples",
     lx::client_id{8}, &critical_by_an_outside_source},
    {"no pressure evicts nothing; the way back to normal changes nothing more", lx::client_id{9},
     &no_pressure_and_the_way_back},
    {"once per level change; what the lifecycle released is what the cache then evicts",
     lx::client_id{10}, &once_per_level_change_and_the_lifecycle_feeds_the_cache},
};

} // namespace

// The whole cascade, scenario by scenario, on one started compositor.
LUMEN_TEST(cascade_from_the_coordinator_to_the_compositor_s_own_consumers) {
    started_compositor c{"lumen-test-cascade"};
    auto& memory = c.comp->memory();
    auto& cache = c.comp->import_cache();
    auto& life = c.comp->buffer_lifecycle();
    std::println("    compositor started on one virtual output, on its default device; the");
    std::println("    pressure handler is the one start() registered (compositor_impl.cpp:10280)");

    unsigned frame = 0;
    unsigned failed = 0;
    for (unsigned i = 0; i < std::size(k_scenarios); ++i) {
        const scenario& s = k_scenarios[i];
        // The scenario before left its buffers with the compositor: its client is gone, the
        // way a disconnect cleans up — every import of its own evicted.
        if (i > 0)
            life.on_client_disconnect(k_scenarios[i - 1].client);
        expect(cache.size() == 0, "a disconnect must leave nothing of its client's in the cache");
        memory.on_external_pressure(rt::memory_pressure::normal);
        report_ui_usage(c, 10);
        expect(memory.current_pressure() == rt::memory_pressure::normal,
               "each scenario must begin under no pressure");
        try {
            s.run(c, frame);
            std::println("    PASS  {}", s.name);
        } catch (const lumen_test::assertion_error& e) {
            ++failed;
            std::println("    FAIL  {} — {}", s.name, e.what());
        }
        // The frames the scenario used, and some air: no buffer may go stale on the next.
        frame += 32;
    }
    std::println("  driven through the compositor's coordinator, on a started compositor:");
    std::println(
        "    import cache eviction    compositor_impl.cpp:2644 -> import_cache.cppm:227-233");
    std::println(
        "    buffer lifecycle release  compositor_impl.cpp:2645 -> buffer_lifecycle.cppm:404-417");
    std::println(
        "  not driven: shm_pool_.trim() (compositor_impl.cpp:2648-2649) — the pool is private");
    std::println(
        "    (:1006), nothing on the facade exposes it, and its bytes are readable only as");
    std::println("    health_inputs::staging_bytes (:4114) over the control socket");
    expect(failed == 0, std::format("{} of {} scenarios failed", failed, std::size(k_scenarios)));
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
