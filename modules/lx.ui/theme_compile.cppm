module;

import lx.foundation;

export module lx.ui:theme.compile;

import :style;

export namespace lx::ui {

struct theme_source {
    const char* input_path = "";
    const char* output_path = "";
    const char* theme_name = "default";
};

struct compile_options {
    bool strip_comments = true;
    bool embed_fonts = false;
    float default_scale = 1.f;
};

struct compile_report {
    unsigned rule_count = 0;
    unsigned widget_type_count = 0;
    const char* output_path = "";
    bool ok = false;
};

/// Compile-time theme compiler API (invoked by `lumen-theme` tool).
class theme_compiler {
public:
    explicit theme_compiler(compile_options options = {});

    [[nodiscard]] lx::result<compile_report> compile(const theme_source& source) const;
    [[nodiscard]] lx::result<theme> load_compiled(const char* compiled_path) const;

private:
    compile_options options_{};
};

} // namespace lx::ui


lx::ui::theme_compiler::theme_compiler(compile_options options) : options_{options} {}

lx::result<lx::ui::compile_report>
lx::ui::theme_compiler::compile(const theme_source& source) const {
    (void)options_;
    (void)source;
    return lx::not_implemented("lx::ui::theme_compiler::compile");
}

lx::result<lx::ui::theme> lx::ui::theme_compiler::load_compiled(const char* compiled_path) const {
    return lx::ui::theme::load(compiled_path);
}
