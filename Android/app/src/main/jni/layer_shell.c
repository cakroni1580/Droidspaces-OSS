/*
 * wlr-layer-shell (zwlr_layer_shell_v1)
 * note contract:layershell.c berjalan di trierarch compositor jni sebagai compositor display host
 * note contract:buat agar agar mengikuti phoc alih alih wlroots.
 *
 *
 *
 * Stage 1
 * --------
 * Skeleton only.
 *
 * Implemented:
 *   - global bind
 *   - layer shell global
 *   - layer surface resource
 *   - destroy callback
 *
 * Not implemented yet:
 *   - configure
 *   - layout
 *   - popup
 *   - keyboard focus
 *   - exclusive zone
 */

#include "server_internal.h"
#include "wlr-layer-shell-unstable-v1-server-protocol.h"

#include <android/log.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "TrierarchLayerShell"

#define LOGI(...) \
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

#define LOGE(...) \
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)


/* ------------------------------------------------------------------------- */
/* layer_surface resource                                                    */
/* ------------------------------------------------------------------------- */

static void layer_surface_resource_destroy(
        struct wl_resource *resource)
{
    struct compositor_surface *surf =
            wl_resource_get_user_data(resource);

    if (!surf)
        return;

    surf->layer_surface_res = NULL;
}


/* ------------------------------------------------------------------------- */
/* layer_surface requests                                                    */
/* ------------------------------------------------------------------------- */

static void layer_surface_destroy(
        struct wl_client *client,
        struct wl_resource *resource)
{
    (void)client;

    wl_resource_destroy(resource);
}

static void layer_surface_set_size(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t width,
        uint32_t height)
{
    (void)client;
    (void)resource;
    (void)width;
    (void)height;
}

static void layer_surface_set_anchor(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t anchor)
{
    (void)client;
    (void)resource;
    (void)anchor;
}

static void layer_surface_set_exclusive_zone(
        struct wl_client *client,
        struct wl_resource *resource,
        int32_t zone)
{
    (void)client;
    (void)resource;
    (void)zone;
}

static void layer_surface_set_margin(
        struct wl_client *client,
        struct wl_resource *resource,
        int32_t top,
        int32_t right,
        int32_t bottom,
        int32_t left)
{
    (void)client;
    (void)resource;
    (void)top;
    (void)right;
    (void)bottom;
    (void)left;
}

static void layer_surface_set_keyboard_interactivity(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t mode)
{
    (void)client;
    (void)resource;
    (void)mode;
}

static void layer_surface_get_popup(
        struct wl_client *client,
        struct wl_resource *resource,
        struct wl_resource *popup)
{
    (void)client;
    (void)resource;
    (void)popup;
}

static void layer_surface_ack_configure(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t serial)
{
    (void)client;
    (void)resource;
    (void)serial;
}

static void layer_surface_set_layer(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t layer)
{
    (void)client;
    (void)resource;
    (void)layer;
}


/* ------------------------------------------------------------------------- */
/* layer_surface interface                                                   */
/* ------------------------------------------------------------------------- */

static const struct zwlr_layer_surface_v1_interface
layer_surface_impl = {

    .destroy = layer_surface_destroy,

    .set_size = layer_surface_set_size,

    .set_anchor = layer_surface_set_anchor,

    .set_exclusive_zone =
        layer_surface_set_exclusive_zone,

    .set_margin =
        layer_surface_set_margin,

    .set_keyboard_interactivity =
        layer_surface_set_keyboard_interactivity,

    .get_popup =
        layer_surface_get_popup,

    .ack_configure =
        layer_surface_ack_configure,

    .set_layer =
        layer_surface_set_layer,
};


/* ------------------------------------------------------------------------- */
/* layer_shell requests                                                      */
/* ------------------------------------------------------------------------- */

static void layer_shell_destroy(
        struct wl_client *client,
        struct wl_resource *resource)
{
    (void)client;

    wl_resource_destroy(resource);
}

static void layer_shell_get_layer_surface(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id,
        struct wl_resource *surface,
        struct wl_resource *output,
        uint32_t layer,
        const char *namespace)
{
    (void)client;
    (void)resource;
    (void)id;
    (void)surface;
    (void)output;
    (void)layer;
    (void)namespace;

    /*
     * Stage 2:
     *
     * - obtain compositor_surface
     * - validate role
     * - create layer_surface resource
     * - attach implementation
     * - send initial configure
     */
}


/* ------------------------------------------------------------------------- */
/* layer_shell interface                                                     */
/* ------------------------------------------------------------------------- */

static const struct zwlr_layer_shell_v1_interface
layer_shell_impl = {

    .destroy = layer_shell_destroy,

    .get_layer_surface =
        layer_shell_get_layer_surface,
};


/* ------------------------------------------------------------------------- */
/* global bind                                                               */
/* ------------------------------------------------------------------------- */

void layer_shell_bind(
        struct wl_client *client,
        void *data,
        uint32_t version,
        uint32_t id)
{
    struct wayland_server *srv = data;

    struct wl_resource *resource =
            wl_resource_create(
                    client,
                    &zwlr_layer_shell_v1_interface,
                    version,
                    id);

    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(
            resource,
            &layer_shell_impl,
            srv,
            NULL);

    LOGI("bind zwlr_layer_shell_v1 version=%u",
            version);
}
