module;

import lx.foundation;

export module lx.a11y;

export namespace lx::a11y {

enum class role {
    unknown,
    button,
    label,
    text_field,
    window,
    group,
};

enum class state_flag : unsigned {
    none = 0,
    focused = 1u << 0,
    disabled = 1u << 1,
    expanded = 1u << 2,
};

struct node {
    role role_ = role::unknown;
    const char* name = "";
    const char* description = "";
    const char* value = "";
    unsigned state = 0;
    void* widget = nullptr;
};

class tree {
public:
    void set_root(node root);
    [[nodiscard]] const node& root() const;
    void sync_to_atspi();

private:
    node root_{};
};

class atspi_bridge {
public:
    [[nodiscard]] lx::result<void> connect();
    void publish(const tree& accessibility_tree);
    void disconnect();

private:
    bool connected_ = false;
};

} // namespace lx::a11y


void lx::a11y::tree::set_root(node root) { root_ = root; }
const lx::a11y::node& lx::a11y::tree::root() const { return root_; }

void lx::a11y::tree::sync_to_atspi() {
    atspi_bridge bridge{};
    if (auto connected = bridge.connect(); connected)
        bridge.publish(*this);
}

lx::result<void> lx::a11y::atspi_bridge::connect() {
    return lx::not_implemented("lx::a11y::atspi_bridge::connect");
}

void lx::a11y::atspi_bridge::publish(const tree& t) {
    (void)t;
}

void lx::a11y::atspi_bridge::disconnect() { connected_ = false; }
