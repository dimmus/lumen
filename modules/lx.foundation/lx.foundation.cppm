module;

export module lx.foundation;

export import :types;
export import :result;
export import :handles;
export import :error;
export import :wm_types;

export namespace lx {

struct version {
    static constexpr int major = 0;
    static constexpr int minor = 3;
    static constexpr int patch = 0;
};

template<typename T>
[[nodiscard]] constexpr T clamp(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace lx
