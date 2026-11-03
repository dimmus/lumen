---
name: lumen-vulkan-13-floor
description: Vulkan 1.3 floor + dynamic rendering/sync2 + shared pipeline cache + host_image_copy/memory_budget/memory_priority, 2026-10-28 merged to main as PR #57 (bf1cdc0); user benchmarks on real HW
metadata:
  type: project
---

2026-10-28, branch claude/exciting-jones-02795b, merged to main as PR #57 (merge bf1cdc0; the merged tree with newer main was not built or tested, at the user's request): ADR-004 D4 carried out — instances ask for 1.3, devices below 1.3 are filtered out in `lx_gfx_enumerate` (→ automatic falls to GL), Vulkan12/13 features enabled (timeline, dynamicRendering, synchronization2), no VkRenderPass/VkFramebuffer left in vk_renderer/ui_vk/swapchain_vk, all barriers vkCmdPipelineBarrier2, compositor creates ui_vk first and passes `ui_vk_renderer::pipeline_cache()` to `vulkan_compositor::initialize`. 107/107 ctest on both baseline and change; G10 goldens untouched.

Step 4 followed the same day at the user's request ("implement without vm test — I will do it myself on real hardware"): `host_image_copier` (gated on optimalDeviceAccess + SHADER_READ_ONLY_OPTIMAL copy layout) for `upload_rgba` and fresh ui_vk atlas pages (`page_fresh_`), staging ring now lazy, `memory_pool::gpu` fed from render at 1 Hz via `device::memory_budget`, memory priority 1.0 on scanout targets + linear intermediates. `bench_composite --vk` prints `-- Vulkan shm upload --` both ways. Push descriptors not done.

**Why:** the user measures on RADV/ANV themselves; no lavapipe number decides these, see [[lumen-no-degradation-rule]].
**How to apply:** if the user reports hardware numbers, keep or revert host copy per path on them; lavapipe caches nothing in VkPipelineCache so the shared-cache gain is also unmeasured. Related: [[lumen-host-build-run-env]].
