---
name: lumen-no-degradation-rule
description: "User rule for Lumen changes — no degradation, and no feature/functionality that does not push Lumen forward; gate every change on measurement"
metadata:
  node_type: memory
  type: feedback
  originSessionId: b66679e2-b37c-4f8d-a20e-a65f3602e64b
  modified: 2026-10-28T12:38:46.194Z
---

"Do not let degradation. I do not want features and functionality that does not push Lumen forward." (user, 2026-10-28, when asking for items 1-5 of the umbriel/scenefx review to be implemented).

**Why:** the user wants every change to earn its place; speculative features, knobs or marginal optimisations are unwanted even if they work.
**How to apply:** for each change, measure before/after, require all baseline-passing tests to stay green and pixels to match, and revert or abandon anything whose gain is marginal or unproven rather than shipping it. No new config keys/env vars/user-facing features unless asked. Refinement (user, 2026-10-28, on the kernel-features scheduling measurement): "as we are vulkan first you can save changes that do vulkan backend better, but others like software can become worse" — the gate is the Vulkan backend; a change that helps Vulkan may be kept even if the GL/CPU paths regress. This host has no hardware Vulkan (vmwgfx runs the GL path; lavapipe is software), so a Vulkan gain usually cannot be measured here — say so rather than claim one. Refinement (user, 2026-10-29, at the start of ADR-010 Phase 5): "do not allow regressions, check and ask me if see one" — when a measurement, test, smoke or pixel check shows a regression, stop and ask the user before keeping, working around or accepting it; do not decide alone that a regression is acceptable or "just noise" without showing the A/B evidence. Related: [[lumen-best-in-class-bar]], [[lumen-umbriel-scenefx-review]].
