module;

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

export module lx.foundation:handles;

export namespace lx {

template<typename Tag>
class handle {
public:
    using id_type = unsigned long long;

    constexpr handle() = default;
    explicit constexpr handle(id_type id) : id_{id} {}

    [[nodiscard]] constexpr id_type id() const { return id_; }
    [[nodiscard]] constexpr explicit operator bool() const { return id_ != 0; }
    [[nodiscard]] constexpr bool operator==(const handle&) const = default;

private:
    id_type id_ = 0;
};

struct surface_tag;
struct toplevel_tag;
struct output_tag;
struct seat_tag;
struct buffer_tag;
struct workspace_tag;
struct client_tag;
struct texture_tag;
struct cursor_tag;
struct image_description_tag;

using surface_id = handle<surface_tag>;
using toplevel_id = handle<toplevel_tag>;
using output_id = handle<output_tag>;
using seat_id = handle<seat_tag>;
using buffer_id = handle<buffer_tag>;
using workspace_id = handle<workspace_tag>;
using client_id = handle<client_tag>;
using texture_id = handle<texture_tag>;
using cursor_id = handle<cursor_tag>;
using image_description_id = handle<image_description_tag>;

class unique_fd {
public:
    unique_fd() = default;
    explicit unique_fd(int fd);
    ~unique_fd();

    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;
    unique_fd(unique_fd&& other) noexcept;
    unique_fd& operator=(unique_fd&& other) noexcept;

    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] int release();
    void reset(int fd = -1);

private:
    void close_fd();
    int fd_ = -1;
};

} // namespace lx


lx::unique_fd::unique_fd(int fd) : fd_{fd} {}
lx::unique_fd::~unique_fd() { reset(); }
lx::unique_fd::unique_fd(unique_fd&& other) noexcept : fd_{other.fd_} { other.fd_ = -1; }
lx::unique_fd& lx::unique_fd::operator=(unique_fd&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}
int lx::unique_fd::release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
}
void lx::unique_fd::reset(int fd) {
    close_fd();
    fd_ = fd;
}
void lx::unique_fd::close_fd() {
#if defined(_WIN32)
    if (fd_ >= 0) {
        _close(fd_);
        fd_ = -1;
    }
#else
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
}
