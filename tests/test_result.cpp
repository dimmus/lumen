#include "lumen_test.hpp"

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

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
