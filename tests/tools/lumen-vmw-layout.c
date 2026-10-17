// lumen-vmw-layout: drive the virtual monitors of a VMware guest for live hotplug tests
// (ADR 008 §3). It sets the vmwgfx preferred layout with DRM_VMW_UPDATE_LAYOUT — what
// open-vm-tools' resolutionKMS does when the host window or topology changes: connectors
// 0..n-1 become connected with the given size as their preferred mode, the rest
// disconnected, `suggested X/Y` take the rect origins, and one `change` uevent with
// HOTPLUG=1 (no CONNECTOR=) goes out. The kernel allows it to DRM master or CAP_SYS_ADMIN,
// so a test runs it with sudo while the compositor holds master.
//
//   lumen-vmw-layout [-d CARD] WxH[+X+Y] [WxH[+X+Y] ...]   set the layout (X defaults to
//                                                          the running sum, Y to 0)
//   lumen-vmw-layout --show      status and first mode of every Virtual-N (sysfs; reading
//                                status runs a detect, which can raise a uevent of its own)
//   lumen-vmw-layout --probe     GETCONNECTOR on this fd (a non-master gets the kernel's
//                                cache: "demoting to read-only probe")
//
// A test tool: built only where <libdrm/vmwgfx_drm.h> exists, never installed.
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <libdrm/drm.h>
#include <libdrm/vmwgfx_drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef DRM_IOCTL_VMW_UPDATE_LAYOUT
#define DRM_IOCTL_VMW_UPDATE_LAYOUT \
    DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_UPDATE_LAYOUT, struct drm_vmw_update_layout_arg)
#endif

static int probe(int fd) {
    printf("master=%d\n", drmIsMaster(fd));
    drmModeRes* res = drmModeGetResources(fd);
    if (!res)
        return 1;
    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c)
            continue;
        const char* pref = "-";
        char buf[32];
        for (int m = 0; m < c->count_modes; ++m)
            if (c->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
                snprintf(buf, sizeof buf, "%ux%u", c->modes[m].hdisplay, c->modes[m].vdisplay);
                pref = buf;
                break;
            }
        printf("connector %u %s modes=%d preferred=%s\n", c->connector_id,
               c->connection == DRM_MODE_CONNECTED ? "connected" : "disconnected", c->count_modes,
               pref);
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    return 0;
}

static int show(void) {
    glob_t g;
    if (glob("/sys/class/drm/card*-Virtual-*", 0, NULL, &g) != 0)
        return 1;
    for (size_t i = 0; i < g.gl_pathc; ++i) {
        char p[512], st[32] = "?", mode[64] = "-";
        snprintf(p, sizeof p, "%s/status", g.gl_pathv[i]);
        FILE* f = fopen(p, "r");
        if (f) {
            if (fscanf(f, "%31s", st) != 1)
                strcpy(st, "?");
            fclose(f);
        }
        snprintf(p, sizeof p, "%s/modes", g.gl_pathv[i]);
        f = fopen(p, "r");
        if (f) {
            if (fscanf(f, "%63s", mode) != 1)
                strcpy(mode, "-");
            fclose(f);
        }
        printf("%s %s %s\n", strrchr(g.gl_pathv[i], '/') + 1, st, mode);
    }
    globfree(&g);
    return 0;
}

int main(int argc, char** argv) {
    const char* dev = "/dev/dri/card0";
    int a = 1;
    if (argc > 1 && strcmp(argv[1], "--show") == 0)
        return show();
    if (argc > 2 && strcmp(argv[1], "-d") == 0) {
        dev = argv[2];
        a = 3;
    }
    if (a < argc && strcmp(argv[a], "--probe") == 0) {
        int fd = open(dev, O_RDWR | O_CLOEXEC);
        return fd < 0 ? 1 : probe(fd);
    }
    struct drm_vmw_rect rects[8];
    unsigned n = 0;
    int x_auto = 0;
    for (; a < argc && n < 8; ++a) {
        unsigned w, h;
        int x = x_auto, y = 0;
        int k = sscanf(argv[a], "%ux%u+%d+%d", &w, &h, &x, &y);
        if (k < 2 || w == 0 || h == 0) {
            fprintf(stderr, "bad rect %s (WxH[+X+Y])\n", argv[a]);
            return 2;
        }
        if (k < 4) {
            x = x_auto;
            y = 0;
        }
        rects[n].x = x;
        rects[n].y = y;
        rects[n].w = w;
        rects[n].h = h;
        x_auto = x + (int)w;
        ++n;
    }
    if (n == 0) {
        fprintf(stderr, "usage: lumen-vmw-layout [-d CARD] WxH[+X+Y] ... | --show | --probe\n");
        return 2;
    }
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(dev);
        return 1;
    }
    struct drm_vmw_update_layout_arg arg = {.num_outputs = n, .rects = (uint64_t)(uintptr_t)rects};
    if (ioctl(fd, DRM_IOCTL_VMW_UPDATE_LAYOUT, &arg) != 0) {
        fprintf(stderr, "DRM_IOCTL_VMW_UPDATE_LAYOUT: %s (needs DRM master or CAP_SYS_ADMIN)\n",
                strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}
