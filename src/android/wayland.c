/*
 * Droidspaces v6 - Wayland compositor socket bridge
 *
 * Stages the host-side compositor socket into /run/droidspaces/wayland-1,
 * which is immune to user-runtime-dir@0 overmounts. The rootfs-side systemd
 * service re-binds it into the correct /run/user/<uid>/ per user after boot.
 *
 * Host side:  /.old_root/data/data/com.droidspaces.app/files/usr/tmp/wayland-1
 * Staged at:  /run/droidspaces/wayland-1
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include "droidspace.h"
#include <sys/stat.h>

int ds_setup_wayland_socket(struct ds_config *cfg) {
  if (!is_android())
    return 0;

  if (!cfg->wayland)
    return 0;

  if (access(DS_WL_HOST_SOCKET_OLDROOT, F_OK) != 0) {
    ds_warn("[Wayland] compositor socket not found at %s",
            DS_WL_HOST_SOCKET_OLDROOT);
    ds_warn("[Wayland] Is the Wayland compositor running in the Droidspaces app?");
    return -1;
  }

  /* Stage under /run/droidspaces - immune to user-runtime-dir@0 overmounts.
   * XDG_RUNTIME_DIR and WAYLAND_DISPLAY are set per-user by the rootfs side. */
  if (ds_bind_mount_socket(DS_WL_HOST_SOCKET_OLDROOT, DS_WL_CONTAINER_SOCKET,
                           0, "Wayland") < 0)
    return -1;
  ds_log("[Wayland] socket staged: %s -> %s",
         DS_WL_HOST_SOCKET_OLDROOT,
         DS_WL_CONTAINER_SOCKET);

  /*
   * Publish the staged compositor socket into the root user's
   * standard XDG runtime directory.
   *
   * /run/droidspaces/wayland-1
   *          ↓ bind mount
   * /run/user/0/wayland-1
   */
  if (mkdir("/run/user", 0755) < 0 && errno != EEXIST) {
    ds_warn("[Wayland] failed to create /run/user: %s", strerror(errno));
    return -1;
}

if (mkdir("/run/user/0", 0700) < 0 && errno != EEXIST) {
    ds_warn("[Wayland] failed to create /run/user/0: %s", strerror(errno));
    return -1;
}

/*
 * Publish staged compositor socket into the standard root
 * XDG runtime directory.
 */
if (ds_bind_mount_socket(DS_WL_CONTAINER_SOCKET,
                         "/run/user/0/wayland-1",
                         0, "Wayland root runtime") < 0)
    return -1;

ds_log("[Wayland] socket published: %s -> /run/user/0/wayland-1",
       DS_WL_CONTAINER_SOCKET);

  setenv("QT_QPA_PLATFORM", "wayland", 1);
  setenv("XDG_SESSION_TYPE", "wayland", 1);
  setenv("XDG_RUNTIME_DIR", "/run/user/0", 1);
  setenv("WAYLAND_DISPLAY", "wayland-1", 1);
  setenv("KWIN_COMPOSE", "Q", 1);
  setenv("KWIN_OPENGL_INTERFACE", "egl", 1);
  setenv("LIBGL_ALWAYS_SOFTWARE", "0", 1);
  setenv("WLR_BACKENDS", "wayland", 1);
  setenv("WLR_RENDERER", "pixman", 1);

  ds_log("[Wayland]   XDG_RUNTIME_DIR=/run/user/0");
  ds_log("[Wayland]   WAYLAND_DISPLAY=wayland-1");

  return 0;
}
