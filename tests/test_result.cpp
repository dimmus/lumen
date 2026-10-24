#include "lumen_test.hpp"

import std;
import lx.foundation;

LUMEN_TEST(result_ok_int) {
    lx::result<int> r{42};
    LUMEN_CHECK(r.ok());
    LUMEN_CHECK(static_cast<bool>(r));
    LUMEN_CHECK(r.value() == 42);
}

LUMEN_TEST(result_error_path) {
    lx::result<int> r{lx::not_implemented("test")};
    LUMEN_CHECK(!r.ok());
    LUMEN_CHECK(r.get_error().domain == lx::error_domain::not_implemented);
}

struct no_default {
    explicit no_default(int v) : value(v) {}
    int value;
};

LUMEN_TEST(result_non_default_constructible) {
    lx::result<no_default> ok{no_default{7}};
    LUMEN_CHECK(ok.ok());
    LUMEN_CHECK(ok.value().value == 7);

    lx::result<no_default> err{lx::make_error(lx::error_domain::invalid_argument, 1, "bad")};
    LUMEN_CHECK(!err.ok());
}

LUMEN_TEST(result_void_ok) {
    lx::result<void> r{};
    LUMEN_CHECK(r.ok());
}

LUMEN_TEST(result_void_error) {
    lx::result<void> r{lx::not_implemented("void")};
    LUMEN_CHECK(!r.ok());
}

LUMEN_TEST(result_monadic) {
    const lx::result<int> ok{20};
    const lx::result<int> doubled = ok.transform([](int v) { return v * 2; });
    LUMEN_CHECK(doubled.ok() && *doubled == 40);

    const auto half = [](int v) -> std::expected<int, lx::error> {
        if (v % 2)
            return std::unexpected(lx::invalid_argument("odd"));
        return v / 2;
    };
    const lx::result<int> chained = ok.and_then(half).and_then(half);
    LUMEN_CHECK(chained.ok() && chained.value() == 5);
    const lx::result<int> failed = chained.and_then(half);
    LUMEN_CHECK(!failed.ok());
    LUMEN_CHECK(failed.get_error().domain == lx::error_domain::invalid_argument);
    LUMEN_CHECK(failed.value_or(-1) == -1);
}

LUMEN_TEST(result_domain_from_code) {
    const lx::error e = lx::make_error(lx::drm_err::page_flip_failed, "flip");
    LUMEN_CHECK(e.domain == lx::error_domain::drm);
    LUMEN_CHECK(e.code == std::to_underlying(lx::drm_err::page_flip_failed));
    lx::result<void> r{lx::make_error(lx::wayland_err::bind_failed, "bind")};
    LUMEN_CHECK(r.get_error().domain == lx::error_domain::wayland);
}

// ── lx::sparse_slots ──────────────────────────────────────────────────────────

namespace {
struct rec {
    int id = -1; // a non-zero default, like the tables it holds
    bool used = false;
};
} // namespace

LUMEN_TEST(sparse_slots_allocates_a_block_on_first_write_only) {
    lx::sparse_slots<rec, 64> t;
    LUMEN_CHECK(t.blocks_allocated() == 0);
    const auto& c = t;
    LUMEN_CHECK(c[40].id == -1 && !c[40].used); // an untouched block reads as defaults
    LUMEN_CHECK(t.blocks_allocated() == 0);
    t[40].used = true; // block 2 (slots 32..47)
    t[40].id = 7;
    LUMEN_CHECK(t.blocks_allocated() == 1);
    LUMEN_CHECK(c[40].id == 7 && c[41].id == -1);
}

LUMEN_TEST(sparse_slots_iterates_the_allocated_blocks) {
    lx::sparse_slots<rec, 64> t;
    t[3].used = true;
    t[50].used = true;
    unsigned seen = 0;
    unsigned used = 0;
    for (auto& r : t) {
        ++seen;
        used += r.used ? 1u : 0u;
    }
    LUMEN_CHECK(seen == 32); // two blocks of 16
    LUMEN_CHECK(used == 2);
    unsigned none = 0;
    const lx::sparse_slots<rec, 64> empty;
    for (const auto& r : empty)
        none += r.used ? 1u : 1u;
    LUMEN_CHECK(none == 0);
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
