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

struct layer_surface_state {

    struct wl_resource *resource;

    /*
     * Current layer-shell state.
     *
     * Trierarch mengikuti model Phoc:
     * tidak memiliki pending/desire state sendiri.
     * Nilai request client langsung disimpan di sini,
     * sedangkan ukuran configure selalu dihitung oleh
     * compositor saat send_layer_surface_configure().
     */
    uint32_t layer;
    uint32_t anchor;

    uint32_t keyboard_interactive;

    int32_t exclusive_zone;

    /*
     * Margin dari client.
     */
    int32_t margin_top;
    int32_t margin_right;
    int32_t margin_bottom;
    int32_t margin_left;

    /*
     * Configure tracking.
     */
    uint32_t last_configure_serial;
    uint32_t acked_serial;

    bool configured;

    char namespace_name[128];
};


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
    free(surf->layer_surface);
   surf->layer_surface = NULL;
   surf->role = SURFACE_ROLE_NONE;
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

    struct compositor_surface *surf =
        wl_resource_get_user_data(resource);

    if (!surf || !surf->layer_surface)
        return;

    /*
     * Phoc-compatible:
     * ukuran request client tidak disimpan.
     * Nilai ini hanya menjadi hint dan akan
     * diputuskan kembali ketika configure
     * dikirim compositor.
     */
   surf->layer_surface->width = width;
   surf->layer_surface->height = height;
}

static void layer_surface_set_anchor(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t anchor)
{
    (void)client;
    (void)resource;
    (void)client;

    struct compositor_surface *surf =
            wl_resource_get_user_data(resource);

    if (!surf || !surf->layer_surface)
            return;

    surf->layer_surface->anchor = anchor;
}

static void layer_surface_set_exclusive_zone(
        struct wl_client *client,
        struct wl_resource *resource,
        int32_t zone)
{
    (void)client;
    (void)resource;
    (void)zone;
    struct compositor_surface *surf =
            wl_resource_get_user_data(resource);

    if (!surf || !surf->layer_surface)
            return;

    surf->layer_surface->exclusive_zone = zone;
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
    struct compositor_surface *surf =
            wl_resource_get_user_data(resource);

    if (!surf || !surf->layer_surface)
            return;

    surf->layer_surface->margin_top = top;
    surf->layer_surface->margin_right = right;
    surf->layer_surface->margin_bottom = bottom;
    surf->layer_surface->margin_left = left;
}

static void layer_surface_set_keyboard_interactivity(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t mode)
{
    (void)client;
    (void)resource;
    (void)mode;
    struct compositor_surface *surf =
            wl_resource_get_user_data(resource);

    if (!surf || !surf->layer_surface)
            return;

    if (mode >
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND) {
        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_KEYBOARD_INTERACTIVITY,
                "invalid keyboard interactivity");
        return;
    }

    surf->layer_surface->keyboard_interactive = mode;
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

    struct compositor_surface *surf =
            wl_resource_get_user_data(resource);

    if (!surf || !surf->layer_surface)
        return;

    /*
     * Ikuti pola xdg-shell/Phoc:
     * ack hanya diterima untuk configure terakhir.
     */
    if (serial != surf->layer_surface->last_configure_serial)
        return;

    surf->layer_surface->acked_serial = serial;
    surf->layer_surface->configured = true;
}

static void layer_surface_set_layer(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t layer)
{
    (void)client;
    (void)resource;
    (void)layer;
    struct compositor_surface *surf =
            wl_resource_get_user_data(resource);

    if (!surf || !surf->layer_surface)
            return;
    if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
                "invalid layer");
        return;
    }

    surf->layer_surface->layer = layer;

    /*
     * Sinkronkan z-order internal.
     * Layout engine nantinya tetap boleh
     * mengubah urutan akhir.
     */
    switch (layer) {

    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        surf->z_order = 0;
        break;

    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        surf->z_order = 100;
        break;

    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        surf->z_order = 5000;
        break;

    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
    default:
        surf->z_order = 10000;
        break;
    }
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

/*
 * Stage 2
 * --------
 *
 * Create zwlr_layer_surface_v1 and attach it to compositor_surface.
 *
 * Design follows xdg_shell.c:
 *
 *  - one Wayland role per wl_surface
 *  - compositor_surface owns protocol state
 *  - initial configure is compositor driven
 *
 * Trierarch host policy:
 *
 *  - output argument is currently ignored
 *    (single logical output)
 *
 *  - namespace is stored for future shell policy
 *
 *  - actual layout/configure handled later by
 *    send_layer_surface_configure()
 */
static void layer_shell_get_layer_surface(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id,
        struct wl_resource *surface_res,
        struct wl_resource *output,
        uint32_t layer,
        const char *namespace)
{
    (void)output;

    struct wayland_server *srv =
            wl_resource_get_user_data(resource);

    struct compositor_surface *surf =
            wl_resource_get_user_data(surface_res);

    if (!srv || !surf || surf->srv != srv) {
        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
                "invalid surface");
        return;
    }

    /*
     * Same rule as xdg-shell:
     * one role per wl_surface.
     */
    if (surf->role != SURFACE_ROLE_NONE) {
        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
                "surface already has role");
        return;
    }

    if (surf->layer_surface) {
        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
                "layer_surface already exists");
        return;
    }

    struct layer_surface_state *state =
            calloc(1, sizeof(*state));

    if (!state) {
        wl_client_post_no_memory(client);
        return;
    }

    struct wl_resource *layer_res =
            wl_resource_create(
                    client,
                    &zwlr_layer_surface_v1_interface,
                    wl_resource_get_version(resource),
                    id);

    if (!layer_res) {
        free(state);
        wl_client_post_no_memory(client);
        return;
    }

    state->resource = layer_res;
    
    if (layer > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                "invalid layer %u",
                layer);
        free(state);
        wl_resource_destroy(layer_res);
        return;
    }

    state->layer = layer;

    if (namespace) {
        strncpy(state->namespace_name,
                namespace,
                sizeof(state->namespace_name) - 1);
    }

    surf->role = SURFACE_ROLE_LAYER;
    surf->layer_surface = state;
    surf->layer_surface_res = layer_res;

    /*
     * Layer surfaces live above normal windows by default.
     * Later layout policy may adjust this depending on layer.
     */
    switch (layer) {

    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        surf->z_order = 0;
        break;

    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        surf->z_order = 100;
        break;

    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        surf->z_order = 5000;
        break;

    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
    default:
        surf->z_order = 10000;
        break;
    }

    wl_resource_set_implementation(
            layer_res,
            &layer_surface_impl,
            surf,
            layer_surface_resource_destroy);

    LOGI(
        "new layer_surface surf=%p layer=%u ns=%s",
        (void *)surf,
        layer,
        namespace ? namespace : "");

    /*
     * Initial configure.
     *
     * Like xdg-shell, the client should not attach
     * its first buffer until configure has been sent.
     */
    send_layer_surface_configure(surf);
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


void send_layer_surface_configure(struct compositor_surface *surf)
{
    if (!surf ||
        !surf->layer_surface ||
        !surf->layer_surface_res ||
        !surf->srv)
        return;

    /*
     * Phoc model:
     * configure selalu berasal dari layout compositor,
     * bukan dari state yang disimpan oleh request
     * set_size().
     */
    uint32_t width = surf->srv->output_width;
    uint32_t height = surf->srv->output_height;
    uint32_t serial =
        wl_display_next_serial(surf->srv->display);

    surf->layer_surface->last_configure_serial =
            serial;

    /*
     * Menunggu ack_configure().
     */
    surf->layer_surface->configured = false;

    zwlr_layer_surface_v1_send_configure(
            surf->layer_surface_res,
            serial,
            width,
            height);
}


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
