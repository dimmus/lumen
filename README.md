# Lumen

Linux windowing system, desktop environment, and UI library — greenfield C++26, Clang, Wayland-only, Vulkan-first.

## Quick start (Linux)

```bash
./scripts/fetch-protocols.sh
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```
or

```bash
# Configure + build (once)
cmake --preset debug          # or: cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset debug  # or: cmake --build build

# Run all tests
ctest --preset release        # if you used a preset
# or, for a plain build dir:
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp}"
ctest --test-dir build --output-on-failure

# One test
ctest --test-dir build -R test_protocol_bind --output-on-failure

# Direct binary
./build/test_kms_device

# Opt-in vkms KMS test
LUMEN_TEST_VKMS=1 ctest --test-dir build -R test_kms_vkms.

# Sanitizer suites
cmake --preset asan && cmake --build --preset asan && ctest --preset asan (same for tsan)
```

## Architecture

**Full architecture guide:** [docs/architecture.md](docs/architecture.md) — processes, module layers, workflows, protocols.

**AI agent instructions:** [AGENTS.md](AGENTS.md) — coding principles, decision framework, anti-patterns.

| Layer | Modules |
|-------|---------|
| Foundation | `lx.foundation`, `lx.sync`, `lx.trace` |
| Runtime | `lx.runtime`, `lx.runtime.executor`, `lx.scheduler` |
| Platform | `lx.wayland.*`, `lx.input`, `lx.drm`, `lx.session` |
| Graphics | `lx.gfx`, `lx.text`, `lx.layout`, `lx.scene` |
| Desktop | `lx.compositor`, `lx.compositor.*`, `lx.shell`, `lx.shell.*`, `lx.ui` |
| Apps | `lx.app`, `lx.ui.builder` |
| Optional | `lx.portal` (Flatpak builds) |

- **Multi-process:** compositor, shell, apps
- **Stacking-first WM** with optional tiling plugin (`PolicyRegistry`)
- **State sync:** compositor → shell snapshot + delta (`zlm_policy_bridge_v1`)
- **First-class threading:** `lx.sync`, `lx.runtime.executor`, immutable double-buffered frames ([docs/subsystems/threading.md](docs/subsystems/threading.md))
- **Errors & logging:** `lx.foundation.error`, `lx.trace` ([docs/subsystems/errors-and-logging.md](docs/subsystems/errors-and-logging.md))
- **Zero-copy:** dmabuf → Vulkan import ([docs/subsystems/dmabuf-vulkan-import.md](docs/subsystems/dmabuf-vulkan-import.md))
- **Performance pipeline:** snapshot v2, scanout, import cache, budgets ([docs/subsystems/rendering-performance.md](docs/subsystems/rendering-performance.md))

UML index: [docs/uml/README.md](docs/uml/README.md)

**Public API reference:** [docs/api/README.md](docs/api/README.md)

## Build options

| Option | Default | Description |
|--------|---------|-------------|
| `LUMEN_PROTOCOLS_P1` | ON | Generate P1 upstream protocols |
| `LUMEN_PROTOCOLS_P2` | OFF | Generate P2 upstream protocols |
| `LUMEN_ENABLE_TILING_PLUGIN` | ON | Tiling plugin compile flag |
| `LUMEN_VULKAN_VALIDATION` | OFF | Vulkan validation layers |
| `LUMEN_BUILD_FLATPAK_PORTAL` | OFF | Flatpak portal adapter |

## systemd (user session)

Install unit files from `deploy/systemd/`. Socket: `$XDG_RUNTIME_DIR/lumen-0`.

## License

MIT
