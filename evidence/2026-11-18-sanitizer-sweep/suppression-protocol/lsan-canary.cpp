// Deliberate first-party leak: run with lsan.supp active — LSan must STILL report it.
#include <cstdlib>
#include <cstdio>
int main() {
    void* leak = std::malloc(12345);
    std::printf("canary leaked %p\n", leak);
    return 0;
}
