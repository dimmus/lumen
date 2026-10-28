module;

// GPU memory from DRM fdinfo (Documentation/gpu/drm-usage-stats.rst, Linux 5.19+): a driver
// that implements it prints, in /proc/<pid>/fdinfo/<fd> of every open DRM file, the driver
// name, a client id unique to that open file, and per memory region the bytes its buffers
// take — `drm-total-<region>`, `drm-shared-`, `drm-resident-`, `drm-purgeable-`,
// `drm-active-` (6.6+) or the legacy `drm-memory-<region>` (resident). That is the one source
// that sees every composite backend's memory: Vulkan, GL and Mesa's own render-node fd alike,
// where VK_EXT_memory_budget sees only Vulkan. gputop, nvtop and intel_gpu_top read the same.
//
// It sits in lx.drm because it is DRM uAPI and nothing else: no graphics API, no libdrm, read
// from /proc. Cold path: read when the health report is asked for, never per frame.
// A driver that prints none of it (vmwgfx today) leaves the report empty.

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

export module lx.drm:fdinfo;

import std;

export namespace lx::drm {

/// One memory region of one DRM client, or summed over several. Bytes; a figure the driver
/// does not print is absent (its `has_` flag clear).
struct gpu_region {
    /// The driver's region name ("vram", "gtt", "system0", "memory"); qualified as
    /// "<driver>:<region>" when a second driver's client is summed in.
    char name[32]{};
    std::uint64_t total = 0;
    std::uint64_t shared = 0;
    std::uint64_t resident = 0;
    std::uint64_t purgeable = 0;
    std::uint64_t active = 0;
    bool has_total = false;
    bool has_shared = false;
    bool has_resident = false;
    bool has_purgeable = false;
    bool has_active = false;
};

inline constexpr unsigned k_max_gpu_regions = 8;

/// What one fdinfo text says. `driver` empty: not a DRM file, or one without usage stats.
struct fdinfo_client {
    char driver[32]{};
    std::uint64_t client_id = 0;
    bool has_client_id = false;
    std::inplace_vector<gpu_region, k_max_gpu_regions> regions{};
    /// Regions past `k_max_gpu_regions`, not kept.
    unsigned regions_dropped = 0;
};

/// Parses one /proc/<pid>/fdinfo/<fd> text. Pure. Values take an optional KiB / MiB (GiB)
/// unit; `drm-memory-<region>` counts as resident when `drm-resident-` is not printed;
/// lines it does not know (engine times, `pos:`, junk) are skipped.
[[nodiscard]] fdinfo_client parse_fdinfo(std::string_view text);

/// GPU memory summed over the DRM clients of a process.
struct gpu_memory {
    /// The first client's driver ("" : no DRM file reported usage stats).
    char driver[32]{};
    /// Distinct clients summed (by drm-client-id).
    unsigned clients = 0;
    std::inplace_vector<gpu_region, k_max_gpu_regions> regions{};
    unsigned regions_dropped = 0;
    /// The client ids already summed: an fd dup'd (or passed to Mesa) is the same client.
    static constexpr unsigned k_max_clients = 32;
    std::uint64_t seen[k_max_clients]{};
    unsigned seen_count = 0;
};

/// Adds `c` to `into` per region. False, and nothing added, for a file without usage stats
/// or a client already counted. A client without drm-client-id cannot be matched and is
/// always added.
bool add_fdinfo_client(gpu_memory& into, const fdinfo_client& c);

/// Walks /proc/self/fdinfo, keeps the DRM character devices (major 226) whose fdinfo has
/// drm-driver, and sums them. Empty when /proc is unreadable or no driver reports. A few
/// syscalls per open DRM file plus one per fd for the type check: for a request only.
[[nodiscard]] gpu_memory read_gpu_memory();

} // namespace lx::drm

namespace {

constexpr unsigned k_drm_major = 226;

[[nodiscard]] std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

void copy_name(char (&dst)[32], std::string_view src) {
    const std::size_t n = src.size() < sizeof(dst) - 1 ? src.size() : sizeof(dst) - 1;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

/// "<digits>[ KiB| MiB| GiB]" → bytes. False for anything else (or an overflow).
[[nodiscard]] bool parse_size(std::string_view v, std::uint64_t& out) {
    std::uint64_t n = 0;
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
    if (ec != std::errc{} || end == v.data())
        return false;
    const std::string_view unit = trim(v.substr(static_cast<std::size_t>(end - v.data())));
    unsigned shift = 0;
    if (unit.empty())
        shift = 0;
    else if (unit == "KiB")
        shift = 10;
    else if (unit == "MiB")
        shift = 20;
    else if (unit == "GiB")
        shift = 30;
    else
        return false;
    if (shift > 0 && n > (std::numeric_limits<std::uint64_t>::max() >> shift))
        return false;
    out = n << shift;
    return true;
}

[[nodiscard]] lx::drm::gpu_region* find_region(
    std::inplace_vector<lx::drm::gpu_region, lx::drm::k_max_gpu_regions>& regions,
    std::string_view name, unsigned& dropped) {
    for (auto& r : regions)
        if (name == r.name)
            return &r;
    if (regions.size() == regions.capacity()) {
        ++dropped;
        return nullptr;
    }
    lx::drm::gpu_region r{};
    copy_name(r.name, name);
    regions.push_back(r);
    return &regions.back();
}

void add_figure(std::uint64_t& into, bool& into_has, std::uint64_t v, bool has) {
    if (!has)
        return;
    into += v;
    into_has = true;
}

} // namespace

lx::drm::fdinfo_client lx::drm::parse_fdinfo(std::string_view text) {
    fdinfo_client c{};
    // drm-memory-<region> seen per region; folded into resident at the end.
    std::uint64_t legacy[k_max_gpu_regions]{};
    bool has_legacy[k_max_gpu_regions]{};
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        const std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos)
            continue;
        const std::string_view key = trim(line.substr(0, colon));
        const std::string_view value = trim(line.substr(colon + 1));
        if (!key.starts_with("drm-"))
            continue;
        if (key == "drm-driver") {
            if (!value.empty())
                copy_name(c.driver, value);
            continue;
        }
        if (key == "drm-client-id") {
            std::uint64_t id = 0;
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), id);
            if (ec == std::errc{} && end == value.data() + value.size()) {
                c.client_id = id;
                c.has_client_id = true;
            }
            continue;
        }
        struct prefix {
            std::string_view text;
            int figure;
        };
        static constexpr prefix k_prefixes[] = {
            {"drm-total-", 0},     {"drm-shared-", 1}, {"drm-resident-", 2},
            {"drm-purgeable-", 3}, {"drm-active-", 4}, {"drm-memory-", 5},
        };
        for (const prefix& p : k_prefixes) {
            if (!key.starts_with(p.text))
                continue;
            const std::string_view region = key.substr(p.text.size());
            std::uint64_t bytes = 0;
            if (region.empty() || !parse_size(value, bytes))
                break;
            gpu_region* r = find_region(c.regions, region, c.regions_dropped);
            if (!r)
                break;
            switch (p.figure) {
            case 0:
                r->total = bytes;
                r->has_total = true;
                break;
            case 1:
                r->shared = bytes;
                r->has_shared = true;
                break;
            case 2:
                r->resident = bytes;
                r->has_resident = true;
                break;
            case 3:
                r->purgeable = bytes;
                r->has_purgeable = true;
                break;
            case 4:
                r->active = bytes;
                r->has_active = true;
                break;
            default: {
                const auto i = static_cast<std::size_t>(r - c.regions.data());
                legacy[i] = bytes;
                has_legacy[i] = true;
            }
            }
            break;
        }
    }
    // The two spellings are aliases; a driver prints one of them.
    for (std::size_t i = 0; i < c.regions.size(); ++i)
        if (has_legacy[i] && !c.regions[i].has_resident) {
            c.regions[i].resident = legacy[i];
            c.regions[i].has_resident = true;
        }
    return c;
}

bool lx::drm::add_fdinfo_client(gpu_memory& into, const fdinfo_client& c) {
    if (c.driver[0] == '\0')
        return false;
    if (c.has_client_id) {
        for (unsigned i = 0; i < into.seen_count; ++i)
            if (into.seen[i] == c.client_id)
                return false;
        if (into.seen_count < gpu_memory::k_max_clients)
            into.seen[into.seen_count++] = c.client_id;
    }
    if (into.driver[0] == '\0')
        copy_name(into.driver, c.driver);
    const bool other_driver = std::string_view{into.driver} != c.driver;
    ++into.clients;
    into.regions_dropped += c.regions_dropped;
    for (const gpu_region& r : c.regions) {
        // Two GPUs both name a region "vram": keep them apart.
        char qualified[32];
        std::string_view name = r.name;
        if (other_driver) {
            std::snprintf(qualified, sizeof(qualified), "%s:%s", c.driver, r.name);
            name = qualified;
        }
        gpu_region* sum = find_region(into.regions, name, into.regions_dropped);
        if (!sum)
            continue;
        add_figure(sum->total, sum->has_total, r.total, r.has_total);
        add_figure(sum->shared, sum->has_shared, r.shared, r.has_shared);
        add_figure(sum->resident, sum->has_resident, r.resident, r.has_resident);
        add_figure(sum->purgeable, sum->has_purgeable, r.purgeable, r.has_purgeable);
        add_figure(sum->active, sum->has_active, r.active, r.has_active);
    }
    return true;
}

lx::drm::gpu_memory lx::drm::read_gpu_memory() {
    gpu_memory out{};
    DIR* dir = ::opendir("/proc/self/fdinfo");
    if (!dir)
        return out;
    const int dfd = ::dirfd(dir);
    // amdgpu and xe print a line per engine as well; 8 KiB holds every driver's text today.
    char buf[8192];
    while (const dirent* e = ::readdir(dir)) {
        int fd = 0;
        const std::string_view name = e->d_name;
        const auto [end, ec] = std::from_chars(name.data(), name.data() + name.size(), fd);
        if (ec != std::errc{} || end != name.data() + name.size() || fd == dfd)
            continue;
        // Our own fd: a dmabuf, a socket, a memfd is no DRM client. Skip them without
        // reading their fdinfo (a compositor holds hundreds).
        struct stat st{};
        if (::fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode) || major(st.st_rdev) != k_drm_major)
            continue;
        const int info = ::openat(dfd, e->d_name, O_RDONLY | O_CLOEXEC);
        if (info < 0)
            continue;
        std::size_t len = 0;
        while (len < sizeof(buf)) {
            const ssize_t n = ::read(info, buf + len, sizeof(buf) - len);
            if (n <= 0)
                break;
            len += static_cast<std::size_t>(n);
        }
        ::close(info);
        std::string_view text{buf, len};
        // Cut short: drop the partial last line rather than read a truncated number.
        if (len == sizeof(buf))
            text = text.substr(0, text.rfind('\n') + 1);
        (void)add_fdinfo_client(out, parse_fdinfo(text));
    }
    ::closedir(dir);
    return out;
}
