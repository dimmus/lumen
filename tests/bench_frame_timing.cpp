#include <chrono>
#include <cstdio>

import lx.foundation;
import lx.gfx;

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    lx::gfx::headless_backend backend;
    if (auto r = backend.create(64, 64); !r) {
        std::fprintf(stderr, "bench_frame: create failed\n");
        return 1;
    }

    constexpr int k_iters = 100;
    double total_ms = 0.0;
    const char* dump_path = "bench_frame_last.ppm";

    for (int i = 0; i < k_iters; ++i) {
        if (auto r = backend.begin_frame(); !r) return 1;
        if (auto r = backend.clear(lx::color::rgb(0.1f, 0.2f, 0.3f)); !r) return 1;
        // Dump only the last iteration to keep CI I/O light.
        const char* path = (i + 1 == k_iters) ? dump_path : nullptr;
        if (auto r = backend.end_frame_and_dump(path); !r) return 1;
        total_ms += backend.last_frame_ms();
    }

    const double avg = total_ms / static_cast<double>(k_iters);
    std::printf("bench_frame: %d clear cycles, avg %.3f ms (dump=%s)\n", k_iters, avg,
                dump_path);
    // Never fail on timing — CTest only checks exit status.
    return 0;
}
