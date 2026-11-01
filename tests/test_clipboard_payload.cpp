// The drag payload's uri-list helpers (lx.ui:clipboard): RFC 2483 formatting (a scheme-
// less absolute path goes out as file://), and parsing (CRLF and LF ends, comments).

#include "lumen_test.hpp"

import std;

import lx.foundation;
import lx.ui;

LUMEN_TEST(uri_list_format_appends_file_scheme_to_paths) {
    const std::vector<std::string_view> uris{"file:///already/a/uri", "/home/u/my file.txt",
                                             "https://example.com/x"};
    const std::string body = lx::ui::format_uri_list(uris);
    LUMEN_CHECK(body ==
                "file:///already/a/uri\r\nfile:///home/u/my%20file.txt\r\nhttps://example.com/x\r\n" ||
                body == "file:///already/a/uri\r\nfile:///home/u/my file.txt\r\nhttps://example.com/x\r\n");
}

LUMEN_TEST(uri_list_parse_skips_comments_and_blank_lines) {
    const std::vector<std::string> uris =
        lx::ui::parse_uri_list("# comment\r\nfile:///a\r\n\r\nfile:///b\r\n");
    LUMEN_CHECK(uris.size() == 2);
    LUMEN_CHECK(uris[0] == "file:///a" && uris[1] == "file:///b");
}

LUMEN_TEST(uri_list_round_trip_through_parse) {
    const std::vector<std::string_view> uris{"file:///x", "file:///y"};
    const auto parsed = lx::ui::parse_uri_list(lx::ui::format_uri_list(uris));
    LUMEN_CHECK(parsed.size() == 2 && parsed[0] == "file:///x" && parsed[1] == "file:///y");
}

LUMEN_TEST(uri_list_empty_input_gives_empty_output) {
    LUMEN_CHECK(lx::ui::format_uri_list({}).empty());
    LUMEN_CHECK(lx::ui::parse_uri_list("").empty());
}

int main(int argc, char** argv) {
    return lumen_test::run_all(argc, argv);
}
