// Minimal Catch2-style single-header test runner for Lumen unit tests.
// Kept dependency-free so tests build without network during CI bootstrap.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace lumen_test {

struct test_case {
    const char* name;
    void (*fn)();
};

inline std::vector<test_case>& registry() {
    static std::vector<test_case> cases;
    return cases;
}

struct registrar {
    registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int run_all(int argc, char** argv) {
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], "--filter=", 9) == 0)
            filter = argv[i] + 9;
    }

    int failed = 0;
    int ran = 0;
    for (const auto& tc : registry()) {
        if (filter && std::strstr(tc.name, filter) == nullptr)
            continue;
        ++ran;
        try {
            tc.fn();
            std::printf("  PASS  %s\n", tc.name);
        } catch (const std::exception& ex) {
            std::printf("  FAIL  %s — %s\n", tc.name, ex.what());
            ++failed;
        } catch (...) {
            std::printf("  FAIL  %s — unknown exception\n", tc.name);
            ++failed;
        }
    }
    std::printf("%d tests, %d failed\n", ran, failed);
    return failed == 0 ? 0 : 1;
}

struct assertion_error : std::exception {
    explicit assertion_error(std::string msg) : message(std::move(msg)) {}
    const char* what() const noexcept override { return message.c_str(); }
    std::string message;
};

inline void check(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s:%d: CHECK(%s) failed", file, line, expr);
        throw assertion_error(buf);
    }
}

} // namespace lumen_test

#define LUMEN_TEST(name)                                                                           \
    static void lumen_test_fn_##name();                                                            \
    static lumen_test::registrar lumen_test_reg_##name{#name, &lumen_test_fn_##name};              \
    static void lumen_test_fn_##name()

#define LUMEN_CHECK(expr) lumen_test::check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
