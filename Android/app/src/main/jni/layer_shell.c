/*
 * wlr-layer-shell (zwlr_layer_shell_v1)
 * note contract:layershell.c berjalan di trierarch compositor jni sebagai compositor display host
 * note contract:buat agar agar mengikuti phoc alih alih wlroots.
 * ================================================================
 * TRIERARCH WM CONTRACT — WM_MODE_DIRECT
 * ================================================================
 *
 * Target desktop harus universal:
 *
 *   waybar,phosh,plasma-mobile labwc or etc.
 *   ┌──────────────────────────────┐
      esclusive zones
 *   │___________________________________│
 *   │        work-area                  │
 *   │                                   │
 *   │ ┌────────────┐ ┌───────────┐ │
 *   │ │  Firefox     │ | Calculator  │  |
 *   │ │              │ |             │  |
 *   │ └────────────┘ └───────────┘ │
 *   │___________________________________│
     |  back   | home   | window          |
 *   └──────────────────────────────┘
 *
 * NOTE CONTRACT:
 *   layershell.c berjalan di Trierarch compositor JNI sebagai
 *   compositor display host. Implementasi mengikuti policy Phoc,
 *   bukan asumsi generic wlroots.
 *
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
#define TRIERARCH_LAYER_Z_BACKGROUND   (-30000)
#define TRIERARCH_LAYER_Z_BOTTOM       (-20000)
#define TRIERARCH_LAYER_Z_TOP           (20000)
#define TRIERARCH_LAYER_Z_OVERLAY      (30000)
extern void keyboard_focus_update(
        struct wayland_server *srv,
        struct compositor_surface *surface);
static void layer_surface_calculate_size(
        struct compositor_surface *surf,
        uint32_t *width,
        uint32_t *height);
void layer_surface_notify_output_change(
        struct wayland_server *srv);
static void layer_surface_resource_destroy(
        struct wl_resource *resource)
{
    struct compositor_surface *surf =
            wl_resource_get_user_data(resource);
    if (!surf)
        return;
    if (surf->srv &&
        surf->srv->keyboard_focus == surf) {
        keyboard_focus_update(
                surf->srv,
                NULL);
    }
    surf->layer_surface_res = NULL;

    if (surf->layer_surface) {
        surf->layer_surface->popup_res = NULL;
        free(surf->layer_surface);
        surf->layer_surface = NULL;
    }
}
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
    surf->layer_surface->requested_width = width;
    surf->layer_surface->requested_height = height;

    LOGI(
        "layer set_size surf=%p requested=%ux%u",
        (void *)surf,
        width,
        height);
}
static void layer_surface_calculate_position(
        struct compositor_surface *surf,
        uint32_t width,
        uint32_t height)
{
    if (!surf ||
        !surf->srv ||
        !surf->layer_surface)
        return;
    const struct layer_surface_state *ls =
            surf->layer_surface;
    const int32_t ow =
            surf->srv->output_width > 0 ?
            surf->srv->output_width : 1;
    const int32_t oh =
            surf->srv->output_height > 0 ?
            surf->srv->output_height : 1;
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
    if (left) {
        surf->wm_x = ls->margin_left;
    } else if (right) {
        surf->wm_x =
                ow -
                (int32_t)width -
                ls->margin_right;
    } else {
        surf->wm_x =
                (ow - (int32_t)width) / 2;
    }
    if (top) {
        surf->wm_y = ls->margin_top;
    } else if (bottom) {
        surf->wm_y =
                oh -
                (int32_t)height -
                ls->margin_bottom;
    } else {
        surf->wm_y =
                (oh - (int32_t)height) / 2;
    }
}

static void layer_surface_set_anchor(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t anchor)
{
    (void)client;
    struct compositor_surface *surf =
            wl_resource_get_user_data(resource);
    if (!surf || !surf->layer_surface)
        return;
    surf->layer_surface->anchor = anchor;
    if (surf->srv)
       layer_surface_notify_output_change(surf->srv);
    LOGI(
        "layer set_anchor surf=%p anchor=0x%x",
        (void *)surf,
        anchor);
}

static void layer_surface_set_exclusive_zone(
        struct wl_client *client,
        struct wl_resource *resource,
        int32_t zone)
{
    (void)client;
    struct compositor_surface *surf =
            wl_resource_get_user_data(resource);
    if (!surf || !surf->layer_surface)
        return;
    if (zone < -1) {
        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
                "invalid exclusive zone %d",
                zone);
        return;
    }
    surf->layer_surface->exclusive_zone = zone;
    if (surf->srv)
        layer_surface_notify_output_change(surf->srv);
    const uint32_t anchor =
            surf->layer_surface->anchor;
    const bool anchor_top =
            (anchor &
             ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) != 0;
    const bool anchor_bottom =
            (anchor &
             ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) != 0;
    const bool anchor_left =
            (anchor &
             ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) != 0;
    const bool anchor_right =
            (anchor &
             ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) != 0;
    const bool valid_top =
            anchor_top &&
            !anchor_bottom &&
            (anchor_left == anchor_right);
    const bool valid_bottom =
            anchor_bottom &&
            !anchor_top &&
            (anchor_left == anchor_right);
    const bool valid_left =
            anchor_left &&
            !anchor_right &&
            (anchor_top == anchor_bottom);
    const bool valid_right =
            anchor_right &&
            !anchor_left &&
            (anchor_top == anchor_bottom);
    const char *edge = "NONE";
    if (valid_top)
        edge = "TOP";
    else if (valid_bottom)
        edge = "BOTTOM";
    else if (valid_left)
        edge = "LEFT";
    else if (valid_right)
        edge = "RIGHT";
    LOGI(
        "layer exclusive state "
        "surf=%p zone=%d anchor=0x%x edge=%s "
        "margin=%d,%d,%d,%d "
        "valid=%s",
        (void *)surf,
        zone,
        anchor,
        edge,
        surf->layer_surface->margin_top,
        surf->layer_surface->margin_right,
        surf->layer_surface->margin_bottom,
        surf->layer_surface->margin_left,
        (zone > 0 && edge[0] != 'N') ? "YES" : "NO");
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
    struct compositor_surface *surf =
            wl_resource_get_user_data(resource);
    if (!surf || !surf->layer_surface)
        return;
    surf->layer_surface->margin_top = top;
    surf->layer_surface->margin_right = right;
    surf->layer_surface->margin_bottom = bottom;
    surf->layer_surface->margin_left = left;
    if (surf->srv)
        layer_surface_notify_output_change(surf->srv);
    LOGI(
        "layer set_margin surf=%p "
        "top=%d right=%d bottom=%d left=%d",
        (void *)surf,
        top,
        right,
        bottom,
        left);
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
static void layer_surface_get_popup(
        struct wl_client *client,
        struct wl_resource *resource,
        struct wl_resource *popup)
{
    (void)client;
    struct compositor_surface *surf =
            wl_resource_get_user_data(resource);
    if (!surf ||
        !surf->layer_surface) {
        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
                "layer surface is not active");
        return;
    }
    if (surf->layer_surface->popup_res) {

        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
                "layer surface already has a popup");
        return;
    }
    if (!popup ||
        wl_resource_get_client(popup) !=
            wl_resource_get_client(resource)) {
        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
                "popup belongs to another client");
        return;
    }
    surf->layer_surface->popup_res = popup;
    LOGI(
        "layer popup attached surf=%p popup=%p",
        (void *)surf,
        (void *)popup);
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
    LOGI(
        "layer configure ack surf=%p serial=%u",
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
    if (surf->layer_surface ||
        surf->layer_surface_res) {
        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
                "surface already has a layer-shell role");
        LOGE(
            "layer role rejected: surf=%p already has "
            "layer_surface=%p layer_res=%p",
            (void *)surf,
            (void *)surf->layer_surface,
            (void *)surf->layer_surface_res);
        return;
    }
    if (!compositor_surface_set_role(
            surf,
            COMPOSITOR_SURFACE_ROLE_LAYER_SHELL)) {
        wl_resource_post_error(
            resource,
            ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
            "wl_surface already has another role");
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
    surf->layer_surface = state;
    surf->layer_surface_res = layer_res;
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
    surf->wm_x = 0;
    surf->wm_y = 0;
    wl_resource_set_implementation(
            layer_res,
            &layer_surface_impl,
            surf,
            layer_surface_resource_destroy);
    LOGI(
        "new layer_surface pending initial state "
        "surf=%p layer=%u ns=%s",
        (void *)surf,
        layer,
        namespace ? namespace : "");
}
static void layer_surface_calculate_size(
        struct compositor_surface *surf,
        uint32_t *width,
        uint32_t *height)
{
    struct layer_surface_state *ls =
        surf ? surf->layer_surface : NULL;
    struct wayland_server *srv =
        surf ? surf->srv : NULL;
    if (!srv || !width || !height) {
        if (width)
            *width = 1;
        if (height)
            *height = 1;
        return;
    }
    uint32_t ow =
        srv->output_width > 0 ?
        srv->output_width : 1;
    uint32_t oh =
        srv->output_height > 0 ?
        srv->output_height : 1;
    if (!ls) {
        *width = ow;
        *height = oh;
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
    if (left && right) {
        int64_t w =
            (int64_t)ow -
            ls->margin_left -
            ls->margin_right;
        *width = w > 0 ? (uint32_t)w : 1;
    } else if (ls->requested_width > 0) {
        *width = ls->requested_width;
    } else {
        *width = 1;
    }
    if (top && bottom) {
        int64_t h =
            (int64_t)oh -
            ls->margin_top -
            ls->margin_bottom;
        *height = h > 0 ? (uint32_t)h : 1;
    } else if (ls->requested_height > 0) {
        *height = ls->requested_height;
    } else {
        *height = 1;
    }
}

/*
 * ================================================================
 * TRIERARCH WORK-AREA CONTRACT — REALTIME
 * ================================================================
 *
 * Work-area Trierarch adalah STATE REALTIME, bukan pending state.
 * Setiap perubahan output Android (size/orientation/IME/insets)
 * yang sudah diterima dan diproses oleh JNI langsung menjadi
 * geometry authority compositor melalui srv->output_width/height
 * dan layer_surface state yang aktif. layer_shell_get_work_area()
 * wajib menghitung reservation langsung dari state aktif pada saat
 * dipanggil; tidak boleh membuat, menyimpan, atau menunggu pending
 * work-area/configuration state kedua. JNI adalah satu-satunya layer
 * yang mengelola pending lifecycle perubahan Android display, sehingga
 * layer-shell tidak boleh membuat state machine pending tambahan.
 * Dengan kontrak ini work-area selalu merefleksikan kondisi display
 * terbaru dan tidak menambah configure/resize queue yang dapat
 * memperbesar resize storm, termasuk ketika Android IME memicu
 * perubahan ukuran display.
 *
 * ================================================================
 */
void layer_shell_get_work_area(
        struct wayland_server *srv,
        struct compositor_surface *exclude,
        struct trierarch_work_area *area)
{
    if (!srv || !area)
        return;

    const uint32_t ow =
        srv->output_width > 0 ?
        srv->output_width : 1;

    const uint32_t oh =
        srv->output_height > 0 ?
        srv->output_height : 1;

    /*
     * ================================================================
     * CONTEXT:
     *
     * Work-area selalu dimulai dari full output.
     *
     * Layer-shell surface sendiri tetap menggunakan full output
     * coordinate space. Fungsi ini hanya menghitung reservation
     * yang harus dikurangi dari output untuk consumer geometry lain.
     *
     * Tidak ada dependency terhadap xdg-shell.
     * ================================================================
     */
    area->x = 0;
    area->y = 0;
    area->width = ow;
    area->height = oh;

    struct compositor_surface *surf;

    wl_list_for_each(
            surf,
            &srv->surfaces,
            link) {

        if (surf == exclude)
            continue;

        struct layer_surface_state *ls =
            surf->layer_surface;

        if (!ls)
            continue;

        /*
         * ============================================================
         * Exclusive zone:
         *
         *   > 0  -> reservation
         *    0  -> no reservation
         *   -1  -> no reservation
         *
         * Hanya positive exclusive zone yang mempengaruhi work-area.
         * ============================================================
         */
        if (ls->exclusive_zone <= 0)
            continue;

        const bool top =
            (ls->anchor &
             ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) != 0;

        const bool bottom =
            (ls->anchor &
             ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) != 0;

        const bool left =
            (ls->anchor &
             ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) != 0;

        const bool right =
            (ls->anchor &
             ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) != 0;

        /*
         * ============================================================
         * CONTEXT:
         *
         * Exclusive reservation hanya valid apabila ada tepat satu
         * exclusive edge.
         *
         * Valid:
         *
         *   TOP
         *   TOP + LEFT + RIGHT
         *
         *   BOTTOM
         *   BOTTOM + LEFT + RIGHT
         *
         *   LEFT
         *   LEFT + TOP + BOTTOM
         *
         *   RIGHT
         *   RIGHT + TOP + BOTTOM
         *
         * Corner-only anchor seperti:
         *
         *   TOP + LEFT
         *
         * tidak mempunyai exclusive edge yang unik.
         *
         * Begitu juga:
         *
         *   TOP + BOTTOM
         *   LEFT + RIGHT
         *   ALL FOUR
         *
         * ============================================================
         */

        const bool valid_top =
            top &&
            !bottom &&
            (left == right);

        const bool valid_bottom =
            bottom &&
            !top &&
            (left == right);

        const bool valid_left =
            left &&
            !right &&
            (top == bottom);

        const bool valid_right =
            right &&
            !left &&
            (top == bottom);

        int32_t reservation =
            ls->exclusive_zone;

        /*
         * ============================================================
         * TOP
         * ============================================================
         */
        if (valid_top) {

            reservation += ls->margin_top;

            if (reservation < 0)
                reservation = 0;

            if (reservation > (int32_t)area->height)
                reservation = (int32_t)area->height;

            area->y += reservation;
            area->height -= (uint32_t)reservation;

            LOGI(
                "exclusive TOP surf=%p "
                "zone=%d margin=%d "
                "usable=%ux%u@%d,%d",
                (void *)surf,
                ls->exclusive_zone,
                ls->margin_top,
                area->width,
                area->height,
                area->x,
                area->y);

            continue;
        }

        /*
         * ============================================================
         * BOTTOM
         * ============================================================
         */
        if (valid_bottom) {

            reservation += ls->margin_bottom;

            if (reservation < 0)
                reservation = 0;

            if (reservation > (int32_t)area->height)
                reservation = (int32_t)area->height;

            area->height -= (uint32_t)reservation;

            LOGI(
                "exclusive BOTTOM surf=%p "
                "zone=%d margin=%d "
                "usable=%ux%u@%d,%d",
                (void *)surf,
                ls->exclusive_zone,
                ls->margin_bottom,
                area->width,
                area->height,
                area->x,
                area->y);

            continue;
        }

        /*
         * ============================================================
         * LEFT
         * ============================================================
         */
        if (valid_left) {

            reservation += ls->margin_left;

            if (reservation < 0)
                reservation = 0;

            if (reservation > (int32_t)area->width)
                reservation = (int32_t)area->width;

            area->x += reservation;
            area->width -= (uint32_t)reservation;

            LOGI(
                "exclusive LEFT surf=%p "
                "zone=%d margin=%d "
                "usable=%ux%u@%d,%d",
                (void *)surf,
                ls->exclusive_zone,
                ls->margin_left,
                area->width,
                area->height,
                area->x,
                area->y);

            continue;
        }

        /*
         * ============================================================
         * RIGHT
         * ============================================================
         */
        if (valid_right) {

            reservation += ls->margin_right;

            if (reservation < 0)
                reservation = 0;

            if (reservation > (int32_t)area->width)
                reservation = (int32_t)area->width;

            area->width -= (uint32_t)reservation;

            LOGI(
                "exclusive RIGHT surf=%p "
                "zone=%d margin=%d "
                "usable=%ux%u@%d,%d",
                (void *)surf,
                ls->exclusive_zone,
                ls->margin_right,
                area->width,
                area->height,
                area->x,
                area->y);

            continue;
        }

        /*
         * Invalid/corner anchor:
         *
         * The layer remains a normal layer surface, but its
         * exclusive zone cannot be associated with one unique edge.
         *
         * Therefore it does not modify the work-area.
         */
        LOGI(
            "exclusive ignored: invalid edge "
            "surf=%p zone=%d anchor=0x%x",
            (void *)surf,
            ls->exclusive_zone,
            ls->anchor);
    }

    LOGI(
        "layer work-area "
        "x=%d y=%d size=%ux%u",
        area->x,
        area->y,
        area->width,
        area->height);
}

/* layer_shell interface                                                     */
/* ------------------------------------------------------------------------- */

static const struct zwlr_layer_shell_v1_interface
layer_shell_impl = {

    .destroy = layer_shell_destroy,

    .get_layer_surface =
        layer_shell_get_layer_surface,
};


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

    /*
     * CONTEXT:
     *
     * Geometry sudah dipilih oleh compositor pada saat configure.
     *
     * layer_surface_get_geometry() hanya membaca geometry tersebut.
     * Ia TIDAK boleh menghitung ulang anchor/margin.
     */
    layer_surface_calculate_size(
            surf,
            width,
            height);

    *x = surf->wm_x;
    *y = surf->wm_y;

    return true;
}

/*
 * CONTEXT:
 *
 * Android Surface adalah display host Trierarch.
 *
 * srv->output_width / srv->output_height sudah berasal dari
 * ukuran physical Android Surface.
 *
 * Layer-shell configure hanya mem-publish geometry tersebut
 * kepada client layer-shell.
 *
 * Configure ini BUKAN membuat display kedua.
 * Configure ini juga BUKAN membuat window geometry baru.
 *
 * Jalur geometry:
 *
 *     Android Surface
 *           |
 *           v
 *     srv->output_width/height
 *           |
 *           v
 *     layer configure
 *           |
 *           +----> layer client
 *           |
 *           +----> layer work-area
 *                       |
 *                       v
 *                   XDG geometry
 *
 * Dengan demikian layer-shell dan XDG memakai display
 * coordinate space yang sama.
 */
void send_layer_surface_configure(struct compositor_surface *surf)
{
    if (!surf ||
        !surf->layer_surface ||
        !surf->layer_surface_res ||
        !surf->srv)
        return;

    struct wayland_server *srv = surf->srv;

    uint32_t width = 1;
    uint32_t height = 1;

    /*
     * ================================================================
     * GEOMETRY
     * ================================================================
     *    
     * Size berasal dari layer-shell state + physical output.
     *
     * Position kemudian dihitung sekali dan disimpan ke wm_x/wm_y.
     *
     * Setelah helper selesai:
     *
     *     wm_x/wm_y = compositor geometry authority
     */
    layer_surface_calculate_size(
            surf,
            &width,
            &height);

    layer_surface_calculate_position(
            surf,
            width,
            height);

    /*
     * layer_surface_get_geometry() sekarang hanya menjadi consumer
     * geometry. Jangan menggunakan pointer wm_x/wm_y sebagai output
     * parameter di sini karena posisi sudah dihitung di atas.
     */
    uint32_t serial =
            wl_display_next_serial(srv->display);

    zwlr_layer_surface_v1_send_configure(
            surf->layer_surface_res,
            serial,
            width,
            height);

    LOGI(
        "layer DIRECT configure "
        "surf=%p serial=%u geometry=%ux%u@%d,%d "
        "anchor=0x%x margin=%d,%d,%d,%d "
        "exclusive=%d layer=%u",
        (void *)surf,
        serial,
        width,
        height,
        surf->wm_x,
        surf->wm_y,
        surf->layer_surface->anchor,
        surf->layer_surface->margin_top,
        surf->layer_surface->margin_right,
        surf->layer_surface->margin_bottom,
        surf->layer_surface->margin_left,
        surf->layer_surface->exclusive_zone,
        surf->layer_surface->layer);
}

void layer_surface_notify_output_change(
        struct wayland_server *srv)
{
    if (!srv)
        return;
    pthread_mutex_lock(
            &srv->surfaces_mutex);
    struct compositor_surface *surf;
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
