/*
 * wlr-layer-shell (zwlr_layer_shell_v1)
 * note contract:layershell.c berjalan di trierarch compositor jni sebagai compositor display host
 * note contract:buat agar agar mengikuti phoc alih alih wlroots.
 *
 *
 *
 * Stage 1
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
/*
 * CONTEXT:
 * Keyboard focus policy dimiliki oleh compositor/server.
 *
 * layer-shell tidak mengubah keyboard focus secara langsung.
 * Setelah layer surface benar-benar aktif, layer-shell meminta
 * compositor untuk mengubah focus melalui API external ini.
 */
extern void keyboard_focus_update(
        struct wayland_server *srv,
        struct compositor_surface *surface);
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
     * CONTEXT:
     * Resource layer-shell sudah dihancurkan.
     * Surface tidak lagi memiliki layer role.
     *
     * Geometry lama tidak boleh dianggap sebagai geometry
     * aktif jika surface nanti dipakai kembali oleh role lain.
     */
    surf->layer_surface_res = NULL;

    if (surf->layer_surface) {
        surf->layer_surface->popup_res = NULL;
        free(surf->layer_surface);
        surf->layer_surface = NULL;
    }
    /*
     * layer_surface/layer_surface_res are cleared below.
     * Trierarch has no generic surface role enum.
     */
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

    /*
     * CONTEXT:
     * keyboard_interactivity adalah state request dari client.
     *
     * Jangan langsung mengubah keyboard focus di sini.
     *
     * Layer surface belum tentu:
     *   - menerima configure
     *   - ack configure
     *   - memiliki buffer
     *   - sudah mapped
     *
     * Focus aktual dilakukan setelah commit buffer di
     * surface_commit(), sehingga focus hanya diberikan
     * kepada layer surface yang benar-benar aktif.
     */
    surf->layer_surface->keyboard_interactive = mode;

    /*
     * Jika interactivity dimatikan pada surface yang sedang
     * menjadi keyboard focus, lepaskan focus sekarang.
     *
     * Jangan memilih surface pengganti di sini.
     * keyboard_focus_update() hanya diberi NULL; policy
     * pemilihan target tetap berada di layer/seat focus logic.
     */
    if (mode == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE &&
        surf->srv &&
        surf->srv->keyboard_focus == surf) {

        keyboard_focus_update(
                surf->srv,
                NULL);
    }

    LOGI(
        "layer keyboard interactivity surf=%p mode=%u",
        (void *)surf,
        mode);
}

Android/app/src/main/jni/xdg_shell.c

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
     * CONTEXT:
     * Hanya configure terbaru yang valid.
     *
     * Configure lama dapat masih berada di client event queue.
     * Jangan membuka gate buffer berdasarkan serial lama.
     */
    if (serial != surf->layer_surface->last_configure_serial) {
        LOGI(
            "layer stale ack ignored surf=%p serial=%u expected=%u",
            (void *)surf,
            serial,
            surf->layer_surface->last_configure_serial);
        return;
    }

    surf->layer_surface->acked_serial = serial;
    surf->layer_surface->configured = true;

    /*
     * Client sekarang sudah meng-ack geometry terbaru.
     *
     * surface_commit() berikutnya boleh mempromosikan
     * pending_buffer menjadi current_buffer.
     */
    surf->layer_surface->first_buffer_allowed = true;

    LOGI(
        "layer configure acked surf=%p serial=%u",
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
     * One layer-surface role per wl_surface.
     *
     * Trierarch does not have a generic `surf->role` field.
     * Existing protocol resources/state are the role markers.
     *
     * layer_surface itself prevents creating a second
     * zwlr_layer_surface_v1 for the same wl_surface.
     */
    if (surf->layer_surface ||
        surf->layer_surface_res) {

        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
                "surface already has a layer-shell role");
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
/*
 * CONTEXT:
 * Public compositor-side API.
 *
 * Mengembalikan true hanya jika compositor_surface sedang
 * memiliki layer-shell role aktif.
 */
bool layer_surface_is_active(
        struct compositor_surface *surf)
{
    return surf &&
           surf->layer_surface &&
           surf->layer_surface_res;
}
/*
 * CONTEXT:
 * Read-only compositor API.
 *
 * Dipakai focus policy untuk mengetahui apakah layer
 * meminta keyboard interaction.
 */
bool layer_surface_wants_keyboard(
        struct compositor_surface *surf)
{
    if (!surf || !surf->layer_surface)
        return false;

    return surf->layer_surface->keyboard_interactive !=
           ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
}

/*
 * CONTEXT:
 * Configure gate API.
 *
 * Buffer pertama layer-shell hanya boleh dipromosikan
 * setelah configure terbaru di-ack.
 */
bool layer_surface_buffer_allowed(
        struct compositor_surface *surf)
{
    if (!surf || !surf->layer_surface)
        return false;

    return surf->layer_surface->first_buffer_allowed;
}

bool layer_surface_configured(
        struct compositor_surface *surf)
{
    if (!surf || !surf->layer_surface)
        return false;

    return surf->layer_surface->configured;
}

/*
 * CONTEXT:
 * Geometry API.
 *
 * Mengambil geometry hasil policy layer-shell yang sudah
 * ada sekarang. Tidak melakukan recalculation tambahan.
 */
bool layer_surface_get_geometry(
        struct compositor_surface *surf,
        uint32_t *width,
        uint32_t *height,
        int32_t *x,
        int32_t *y)
{
    if (!surf ||
        !surf->layer_surface ||
        !width ||
        !height ||
        !x ||
        !y)
        return false;

    layer_surface_calculate_size(
            surf,
            width,
            height);

    *x = surf->wm_x;
    *y = surf->wm_y;

    return true;
}

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
