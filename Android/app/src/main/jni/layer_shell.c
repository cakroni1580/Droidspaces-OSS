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

/*
 * Trierarch render-list contract:
 *
 * layer-shell tidak memiliki render list sendiri.
 * Semua compositor_surface tetap berada di srv->surfaces
 * dan dirender oleh compositor_foreach_surface() di server.c.
 *
 * z_order hanya menentukan posisi surface tersebut
 * ketika server.c melakukan qsort().
 *
 * Normal xdg surfaces menggunakan z_order mulai dari
 * srv->next_z_order (nilai positif kecil), sehingga
 * range layer-shell dibuat jauh di atas/bawah range normal.
 */
#define TRIERARCH_LAYER_Z_BACKGROUND   (-30000)
#define TRIERARCH_LAYER_Z_BOTTOM       (-20000)
#define TRIERARCH_LAYER_Z_TOP           (20000)
#define TRIERARCH_LAYER_Z_OVERLAY      (30000)

struct layer_surface_state {

    struct wl_resource *resource;
    /*
     * Client requested size.
     *
     * Bukan final size.
     * Hanya dipakai compositor saat
     * menghitung configure.
     */
    uint32_t requested_width;
    uint32_t requested_height;
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
    /*
     * True setelah client menerima
     * configure dan boleh commit buffer.
     */
    bool first_buffer_allowed;

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


    /*
     * Resource sudah tidak punya owner surface.
     * Tidak ada cleanup lagi.
     */
    if (!surf)
        return;


    if (surf->srv &&
        surf->srv->keyboard_focus == surf) {

        keyboard_focus_update(
                surf->srv,
                NULL);
    }


    /*
     * Detach resource dulu.
     */
    surf->layer_surface_res = NULL;


    /*
     * Free layer state hanya sekali.
     */
    if (surf->layer_surface) {

        free(surf->layer_surface);

        surf->layer_surface = NULL;
    }


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
     * Phoc style:
     *
     * Simpan sebagai hint.
     * Layout compositor tetap menentukan
     * configure final.
     */
    surf->layer_surface->requested_width = width;
    surf->layer_surface->requested_height = height;
}

static void layer_surface_set_anchor(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t anchor)
{
    (void)client;
    (void)resource;

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

    struct compositor_surface *surf =
            wl_resource_get_user_data(resource);

    if (!surf || !surf->layer_surface)
        return;

    surf->layer_surface->keyboard_interactive = mode;

    /*
     * Phoc style:
     *
     * Layer surface dengan keyboard interactivity
     * langsung menjadi keyboard target.
     *
     * Tidak membuat pending focus state.
     * Focus diupdate melalui seat keyboard.
     */
    if (mode != 0 && surf->srv) {

        keyboard_focus_update(
                surf->srv,
                surf);
    }
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
    /*
     * Setelah ack configure,
     * client boleh commit buffer.
     */
    surf->layer_surface->first_buffer_allowed = true;
    LOGI(
         "layer ack surf=%p serial=%u",
         (void *)surf,
         serial);
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
     * Trierarch render-list contract:
     *
     * Jangan membuat list layer sendiri.
     * compositor_foreach_surface() di server.c
     * mengambil surface dari srv->surfaces lalu
     * mengurutkannya berdasarkan surf->z_order.
     */
    switch (layer) {

    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        surf->z_order = TRIERARCH_LAYER_Z_BACKGROUND;
        break;

    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        surf->z_order = TRIERARCH_LAYER_Z_BOTTOM;
        break;

    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        surf->z_order = TRIERARCH_LAYER_Z_TOP;
        break;

    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
    default:
        surf->z_order = TRIERARCH_LAYER_Z_OVERLAY;
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
    
    state->configured = false;
    state->first_buffer_allowed = false;
    surf->role = SURFACE_ROLE_LAYER;
    surf->layer_surface = state;
    surf->layer_surface_res = layer_res;

    /*
     * Trierarch render-list contract:
     *
     * Jangan membuat list layer sendiri.
     * compositor_foreach_surface() di server.c
     * mengambil surface dari srv->surfaces lalu
     * mengurutkannya berdasarkan surf->z_order.
     */
    switch (layer) {

    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        surf->z_order = TRIERARCH_LAYER_Z_BACKGROUND;
        break;

    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        surf->z_order = TRIERARCH_LAYER_Z_BOTTOM;
        break;

    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        surf->z_order = TRIERARCH_LAYER_Z_TOP;
        break;

    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
    default:
        surf->z_order = TRIERARCH_LAYER_Z_OVERLAY;
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

/*
 * Calculate layer-shell geometry.
 *
 * Trierarch policy follows the same conceptual model as Phoc:
 *
 *   - requested size is a size hint;
 *   - anchor determines whether an axis is stretched;
 *   - margins reduce the available geometry;
 *   - exclusive_zone reserves output space but does NOT become
 *     the surface size itself;
 *   - wm_x/wm_y contain the final output position used by
 *     compositor_foreach_surface() in DIRECT mode.
 *
 * IMPORTANT:
 * layer-shell does not have its own render list.
 */
static void layer_surface_calculate_size(
        struct compositor_surface *surf,
        uint32_t *width,
        uint32_t *height)
{
    struct layer_surface_state *ls =
            surf->layer_surface;

    struct wayland_server *srv =
            surf->srv;

    uint32_t ow =
            srv->output_width > 0 ?
            srv->output_width : 1;

    uint32_t oh =
            srv->output_height > 0 ?
            srv->output_height : 1;

    if (!ls) {
        *width = ow;
        *height = oh;
        surf->wm_x = 0;
        surf->wm_y = 0;
        return;
    }

    const bool left =
        (ls->anchor &
         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) != 0;

    const bool right =
        (ls->anchor &
         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) != 0;

    const bool top =
        (ls->anchor &
         ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) != 0;

    const bool bottom =
        (ls->anchor &
         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) != 0;

    const bool stretch_x = left && right;
    const bool stretch_y = top && bottom;

    /*
     * ------------------------------------------------------------------
     * Width
     * ------------------------------------------------------------------
     *
     * Both horizontal anchors:
     *
     *     output width - left margin - right margin
     *
     * Otherwise use the client's requested width.
     *
     * Trierarch host policy:
     * if no width was requested, use the output width.
     */
    if (stretch_x) {

        int64_t w =
            (int64_t)ow -
            ls->margin_left -
            ls->margin_right;

        *width = w > 0 ? (uint32_t)w : 1;

    } else if (ls->requested_width > 0) {

        *width = ls->requested_width;

    } else {

        *width = ow;
    }

    /*
     * ------------------------------------------------------------------
     * Height
     * ------------------------------------------------------------------
     */
    if (stretch_y) {

        int64_t h =
            (int64_t)oh -
            ls->margin_top -
            ls->margin_bottom;

        *height = h > 0 ? (uint32_t)h : 1;

    } else if (ls->requested_height > 0) {

        *height = ls->requested_height;

    } else {

        *height = oh;
    }

    /*
     * ------------------------------------------------------------------
     * Position X
     * ------------------------------------------------------------------
     *
     * If both anchors are present, the surface fills the horizontal
     * available area.
     *
     * If only LEFT is anchored:
     *
     *     x = left margin
     *
     * If only RIGHT is anchored:
     *
     *     x = output_width - width - right margin
     *
     * If neither is anchored:
     *
     *     center the requested surface.
     */
    if (left) {

        surf->wm_x =
            ls->margin_left;

    } else if (right) {

        surf->wm_x =
            (int32_t)ow -
            (int32_t)*width -
            ls->margin_right;

    } else {

        surf->wm_x =
            ((int32_t)ow -
             (int32_t)*width) / 2;
    }

    /*
     * ------------------------------------------------------------------
     * Position Y
     * ------------------------------------------------------------------
     */
    if (top) {

        surf->wm_y =
            ls->margin_top;

    } else if (bottom) {

        surf->wm_y =
            (int32_t)oh -
            (int32_t)*height -
            ls->margin_bottom;

    } else {

        surf->wm_y =
            ((int32_t)oh -
             (int32_t)*height) / 2;
    }

    /*
     * ------------------------------------------------------------------
     * Exclusive zone
     * ------------------------------------------------------------------
     *
     * DO NOT replace surface height with exclusive_zone.
     *
     * exclusive_zone means:
     *
     *     "reserve this amount of output space"
     *
     * It is therefore relevant to layout of other surfaces.
     *
     * The layer surface itself keeps its calculated width/height.
     *
     * For Trierarch's single-fullscreen-host model we keep the
     * layer surface anchored at the requested edge.
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


void send_layer_surface_configure(struct compositor_surface *surf)
{
    if (!surf ||
        !surf->layer_surface ||
        !surf->layer_surface_res ||
        !surf->srv)
        return;

     uint32_t width = 0;
     uint32_t height = 0;


    layer_surface_calculate_size(
            surf,
            &width,
            &height);
    
    uint32_t serial =
        wl_display_next_serial(surf->srv->display);

    surf->layer_surface->last_configure_serial =
            serial;

    /*
     * Menunggu ack_configure().
     */
    surf->layer_surface->configured = false;
    /*
     * Tunggu ack_configure sebelum
     * menerima buffer pertama.
     */
    surf->layer_surface->first_buffer_allowed = false;

    zwlr_layer_surface_v1_send_configure(
            surf->layer_surface_res,
            serial,
            width,
            height);
}

void layer_surface_notify_output_change(
        struct wayland_server *srv)
{
    if (!srv)
        return;


    pthread_mutex_lock(
            &srv->surfaces_mutex);


    struct compositor_surface *surf;


    /*
     * NOTE...!!!
     * Tidak ada render traversal di layer-shell.c.
     *
     * Rendering seluruh compositor_surface dilakukan
     * oleh compositor_foreach_surface() di server.c.
     *
     * Layer-shell hanya mengubah:
     *   - surf->layer_surface
     *   - surf->z_order
     *   - configure geometry
     *   - keyboard focus
     *  Ini bukan render traversal.
     */
    
    wl_list_for_each(
            surf,
            &srv->surfaces,
            link) {


        if (!surf->layer_surface)
            continue;

        send_layer_surface_configure(surf);
    }

    


    pthread_mutex_unlock(
            &srv->surfaces_mutex);
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
