# Unit tests
cd build && ctest --output-on-failure

# vkms Vulkan scanout (needs modprobe vkms + DRM master)
LUMEN_TEST_VKMS=1 ./test_kms_vulkan_scanout

# TTY (needs DRM master, not nested GNOME)
chmod +x scripts/tty-smoke.sh && ./scripts/tty-smoke.sh lumen-smoke

# Sanitizers
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan

All four configurations must be green. A sanitizer suite that starts red cannot gate
anything — the first real finding gets waved through as "the usual noise".

## Sanitizer suppressions

Rule: **suppressions are scoped to third-party shared objects only.** Never suppress by
symbol name. `race_top:pthread_mutex_destroy` would silence the driver, but Lumen's own
`std::mutex` lowers to exactly that symbol, so a real "destroyed while another thread held
it" bug would disappear with it.

Both suppression files apply to every test (`LUMEN_SANITIZER_ENV` in `CMakeLists.txt`).
That is safe precisely because they match on object name: a test that never loads a
graphics driver matches nothing, and a test that later grows a compositor gets the right
behavior without anyone having to remember to add it to a list.

Only the blunt instrument is opt-in — `LUMEN_SANITIZER_ENV_VULKAN_ICD`, which turns the
leak check off outright and is applied to just `test_dmabuf_composite` and
`test_kms_vulkan_scanout`.

### TSan — `tests/sanitizers/tsan.supp`

Mesa lavapipe (`libvulkan_lvp.so`, the software Vulkan ICD used on hosts without a GPU)
calls `pthread_mutex_destroy` / `pthread_cond_destroy` on primitives its own llvmpipe
worker threads are still locking and broadcasting on. Real race — Mesa's, in Mesa's
threads, on Mesa's objects.

Verified before suppressing: across 12 runs, every racing access frame resolved to
`libvulkan_lvp.so` (`#0` an intercepted libc call, `#1` the driver). Zero Lumen frames in
any racing access; Lumen appears only at `#2`+ in the thread-creation stack, because
`gfx::device_selector::select_best()` is what makes the driver spawn those threads.

`race:libvulkan_lvp.so` is the narrowest form that works. `called_from_lib:` does not
suppress these — the reports come from TSan's pthread interceptors rather than from
instrumented driver code (verified: 4/4 runs still failed).

Confirm the suppression is still doing exactly its job, and nothing more:

```
# Should print "Matched 4 suppressions" and pass
TSAN_OPTIONS="suppressions=$PWD/tests/sanitizers/tsan.supp:print_suppressions=1" \
    ./build/tsan/test_dmabuf_composite

# Should fail — proves the suppression is load-bearing, not decorative
TSAN_OPTIONS= ./build/tsan/test_dmabuf_composite
```

To check it has not started hiding Lumen races, add a deliberate one to the test (two
threads incrementing a `static long`, with a `std::this_thread::yield()` in the loop so
`RelWithDebInfo` cannot collapse it to a single store) and confirm TSan still reports it
with the suppression active.

### LSan — `tests/sanitizers/lsan.supp` for Mesa EGL/GL

Bringing up an EGL display allocates Mesa driver-global state that lives until process
exit, and creating a GL context allocates gallium state with the same lifetime. LSan
reports both at exit. These resolve to real shared objects (`libEGL_mesa.so`,
`libgallium-*.so`), so they get a proper object-scoped suppression. `libgallium` carries
its Mesa version in the filename, so that pattern is deliberately a prefix.

Any test that starts a compositor can reach this through
`compositor_impl::setup_gl_present`, which is the reason the file is applied suite-wide
rather than to a hand-maintained list of GL tests.

### LSan — `detect_leaks=0` for the Vulkan ICD

lavapipe leaks 224 bytes in 4 allocations of per-thread state, because the driver is
`dlclose`d while its worker threads are still running.

Unlike the EGL leaks above, this one cannot be suppressed. The ICD is unloaded before
LSan's exit check, so its frames
resolve to `<unknown module>` and no `leak:` pattern can match them — LSan matches on
symbolized text. Two rejected alternatives:

- `LD_PRELOAD` of the ICD keeps it mapped and does make the leak disappear, but it
  disappears with *or without* a suppression file, i.e. preloading changes the program
  rather than the report. It would also perturb driver selection on hosts with a real GPU.
- `__lsan_disable()` around device creation does not help: LSan's disable is per-thread,
  and the leaked allocations happen on the driver's own worker threads.

So these two tests run with `LSAN_OPTIONS=detect_leaks=0`. That disables the exit leak
check for them only — every other ASan check (use-after-free, overflow, out-of-bounds)
stays active, and leak detection stays on for all other tests. Reproduce the underlying
report with `LSAN_OPTIONS= ./build/asan/test_dmabuf_composite`.
