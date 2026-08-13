# Droidspaces - Wayland compositor CMake build
#
# Builds libwayland-compositor.so from the trierarch compositor sources
# (via git submodule) plus our own JNI bridge (ds_wayland_bridge.c).
#
# renderer.c and jni_bridge.c are intentionally excluded:
#   - renderer.c  requires ANativeWindow + EGL surface (we run headless)
#   - jni_bridge.c exports symbols under the app.trierarch package namespace
#
# Prebuilt dependencies (arm64-v8a only):
#   ../jniLibs/arm64-v8a/libwayland-server.so
#   ../jniLibs/arm64-v8a/libffi.so
#
# Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.22.1)
project(droidspaces-wayland C)

# ---------------------------------------------------------------------------
# Paths into the submodule
# ---------------------------------------------------------------------------
set(TRIERARCH_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party/trierarch/trierarch-wayland")

set(COMPOSITOR_SRC  "${TRIERARCH_ROOT}/src")
set(PROTOCOL_SRC    "${TRIERARCH_ROOT}/protocol")
set(WAYLAND_INCLUDE "${TRIERARCH_ROOT}/libs/include")
set(JNILIBS_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}")

# ---------------------------------------------------------------------------
# Prebuilt shared libraries
# ---------------------------------------------------------------------------
add_library(wayland-server SHARED IMPORTED)
set_target_properties(wayland-server PROPERTIES
    IMPORTED_LOCATION "${JNILIBS_DIR}/libwayland-server.so")

add_library(ffi SHARED IMPORTED)
set_target_properties(ffi PROPERTIES
    IMPORTED_LOCATION "${JNILIBS_DIR}/libffi.so")

# ---------------------------------------------------------------------------
# libwayland-compositor
# ---------------------------------------------------------------------------
add_library(wayland-compositor SHARED

    # Our JNI bridge (com.droidspaces.app.wayland.WaylandManager + WaylandSurface)
    ds_wayland_bridge.c
    gtk-shell.c
    surface.c
    compositor.c
    layer_shell.c
    pointer_input.c
    xdg_shell.c
    subcompositor.c

    # Compositor core — identical set to Android.mk, minus jni_bridge.c
    # renderer.c is included here because WaylandSurface uses EGL rendering
    #ime_keyboard.c
    ${COMPOSITOR_SRC}/keyboard_input.c
    ${COMPOSITOR_SRC}/renderer.c
    ${COMPOSITOR_SRC}/buffer.c
    ${COMPOSITOR_SRC}/linux_dmabuf.c
    ${COMPOSITOR_SRC}/android_wlegl.c
    ${COMPOSITOR_SRC}/output.c
    ${COMPOSITOR_SRC}/seat.c
    ${COMPOSITOR_SRC}/data_device.c
    ${COMPOSITOR_SRC}/single_pixel_buffer.c
    ${COMPOSITOR_SRC}/viewporter.c
    ${COMPOSITOR_SRC}/xdg_decoration.c
    ${COMPOSITOR_SRC}/xdg_output.c
    ${COMPOSITOR_SRC}/fractional_scale.c
    ${COMPOSITOR_SRC}/pointer_constraints.c
    ${COMPOSITOR_SRC}/relative_pointer.c
    ${COMPOSITOR_SRC}/presentation.c
    ${COMPOSITOR_SRC}/keycode_map.c

    # Generated protocol stubs — exact set from Android.mk
    src/protocol/gtk-shell-protocol.c
    src/protocol/wlr-layer-shell-unstable-v1-protocol.c
    ${PROTOCOL_SRC}/xdg-shell-server-protocol.c
    ${PROTOCOL_SRC}/single-pixel-buffer-v1-server-protocol.c
    ${PROTOCOL_SRC}/viewporter-server-protocol.c
    ${PROTOCOL_SRC}/xdg-decoration-unstable-v1-server-protocol.c
    ${PROTOCOL_SRC}/xdg-output-unstable-v1-server-protocol.c
    ${PROTOCOL_SRC}/fractional-scale-v1-server-protocol.c
    ${PROTOCOL_SRC}/pointer-constraints-unstable-v1-server-protocol.c
    ${PROTOCOL_SRC}/relative-pointer-unstable-v1-server-protocol.c
    ${PROTOCOL_SRC}/presentation-time-server-protocol.c
    ${PROTOCOL_SRC}/linux-dmabuf-v1-server-protocol.c
    ${PROTOCOL_SRC}/android-wlegl-server-protocol.c
    ${PROTOCOL_SRC}/input-method-unstable-v1-server-protocol.c
    ${PROTOCOL_SRC}/text-input-unstable-v3-server-protocol.c
)

target_include_directories(wayland-compositor PRIVATE
    ${COMPOSITOR_SRC}
    ${PROTOCOL_SRC}
    ${WAYLAND_INCLUDE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/protocol
)

# memfd_create syscall numbers for every ABI we might encounter.
# aarch64 (279) and arm (385) are already defined in linux_dmabuf.c;
# x86_64 and x86 fall through to the #else / ENOSYS path without these.
target_compile_definitions(wayland-compositor PRIVATE
    $<$<STREQUAL:${ANDROID_ABI},x86_64>:SYS_memfd_create=319>
    $<$<STREQUAL:${ANDROID_ABI},x86>:SYS_memfd_create=356>
)

target_link_libraries(wayland-compositor
    wayland-server
    ffi
    EGL
    GLESv2
    log
    android
)
