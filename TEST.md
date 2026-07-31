# Unit tests
cd build && ctest --output-on-failure

# vkms Vulkan scanout (needs modprobe vkms + DRM master)
LUMEN_TEST_VKMS=1 ./test_kms_vulkan_scanout

# TTY (needs DRM master, not nested GNOME)
chmod +x scripts/tty-smoke.sh && ./scripts/tty-smoke.sh lumen-smoke