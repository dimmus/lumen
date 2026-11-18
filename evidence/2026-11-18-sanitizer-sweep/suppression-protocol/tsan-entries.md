# TSan suppressions — 12-run / load-bearing / canary protocol (2026-11-18, notebook iritur)
# Carriers: window_protocols (hasvk), ui_golden under the validation layer (khronos), vk_capture (libgallium).
# Each section: 12 runs with the full tsan.supp (all pass), one run without THAT entry (fails — load-bearing;
# the unsuppressed report's top frames are quoted and resolve to the third-party object only), and the canary
# (a deliberate two-thread race in a standalone program) still reported with the full file active.

/usr/bin/bash: line 11: [: =: unary operator expected
## entry race:...(hasvk) — carrier ./test_window_protocols 
/usr/bin/bash: line 13: [: =: unary operator expected
run1 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run2 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run3 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run4 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run5 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run6 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run7 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run8 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run9 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run10 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run11 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run12 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 21: [: =: unary operator expected
load-bearing run without THIS entry: rc=66
SUMMARY: ThreadSanitizer: data race (/home/dimmus/lumen-wt/sanitizer-sweep-green/build/tsan/test_window_protocols+0xb7377) (BuildId: 250375b5602fccb12475dab4999ac7bb59a4c11b) in pthread_barrier_destroy
top racing frames of the unsuppressed report:
    #0 pthread_barrier_destroy <null> (test_window_protocols+0xb7377) (BuildId: 250375b5602fccb12475dab4999ac7bb59a4c11b)
    #1 util_queue_thread_func /usr/src/debug/mesa/build/../mesa-26.2.7/src/util/u_queue.c:300:10 (libvulkan_intel_hasvk.so+0x1914f0) (BuildId: a8394109debaa01ce9904fe63113c443e7c155b3)

## entry race:...(khronos) — carrier ./test_ui_golden with LUMEN_VULKAN_VALIDATION=1
run1 with-supp rc=0 tsan-reports=0
run2 with-supp rc=0 tsan-reports=0
run3 with-supp rc=0 tsan-reports=0
run4 with-supp rc=0 tsan-reports=0
run5 with-supp rc=0 tsan-reports=0
run6 with-supp rc=0 tsan-reports=0
run7 with-supp rc=0 tsan-reports=0
run8 with-supp rc=0 tsan-reports=0
run9 with-supp rc=0 tsan-reports=0
run10 with-supp rc=0 tsan-reports=0
run11 with-supp rc=0 tsan-reports=0
run12 with-supp rc=0 tsan-reports=0
load-bearing run without THIS entry: rc=66
SUMMARY: ThreadSanitizer: data race (/home/dimmus/lumen-wt/sanitizer-sweep-green/build/tsan/test_ui_golden+0x13f1ef) (BuildId: 78277601fac48bd012b5fc5e56255a4e502a345e) in operator delete(void*, unsigned long)
top racing frames of the unsuppressed report:
    #0 operator delete(void*, unsigned long) <null> (test_ui_golden+0x13f1ef) (BuildId: 78277601fac48bd012b5fc5e56255a4e502a345e)
    #1 vulkan_layer_chassis::DestroyFence(VkDevice_T*, VkFence_T*, VkAllocationCallbacks const*) /usr/src/debug/vulkan-validation-layers/Vulkan-ValidationLayers/layers/vulkan/generated/chassis.cpp:1367:42 (libVkLayer_khronos_validation.so+0x6c19e5) (BuildId: bf0c22058b08d3429bec2ab3f6c8921fbca18e65)

/usr/bin/bash: line 11: [: =: unary operator expected
## entry race:...(gallium) — carrier ./test_vk_capture 
/usr/bin/bash: line 13: [: =: unary operator expected
run1 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run2 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run3 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run4 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run5 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run6 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run7 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run8 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run9 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run10 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run11 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 13: [: =: unary operator expected
run12 with-supp rc=0 tsan-reports=0
/usr/bin/bash: line 21: [: =: unary operator expected
load-bearing run without THIS entry: rc=66
SUMMARY: ThreadSanitizer: data race (/home/dimmus/lumen-wt/sanitizer-sweep-green/build/tsan/test_vk_capture+0xac742) (BuildId: 28f9e4ca368edb7d51d90b1379c108a7ad05c6d7) in free
top racing frames of the unsuppressed report:
    #0 free <null> (test_vk_capture+0xac742) (BuildId: 28f9e4ca368edb7d51d90b1379c108a7ad05c6d7)
    #1 unsafe_free /usr/src/debug/mesa/build/../mesa-26.2.7/src/util/ralloc.c:319:7 (libgallium-26.2.7-arch1.1.so+0x5cfc0d) (BuildId: 8f8fcbbdd56b776996a59a4be286d8d00a5cdedc)

## canary (deliberate race, full tsan.supp active)
canary rc=66
SUMMARY: ThreadSanitizer: data race /tmp/tsan-canary.cpp:8:60 in main::$_1::operator()() const
