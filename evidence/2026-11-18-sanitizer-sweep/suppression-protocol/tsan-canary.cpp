// Deliberate data race, compiled with -fsanitize=thread: run with the suppression
// file active — TSan must STILL report it (the files only match third-party objects).
#include <thread>
#include <cstdio>
static long counter = 0;
int main() {
    std::thread a([] { for (int i = 0; i < 2000000; ++i) { ++counter; std::this_thread::yield(); } });
    std::thread b([] { for (int i = 0; i < 2000000; ++i) { ++counter; std::this_thread::yield(); } });
    a.join(); b.join();
    std::printf("canary done %ld\n", counter);
    return 0;
}
