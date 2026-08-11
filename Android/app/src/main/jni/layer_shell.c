/*
 * wlr-layer-shell (zwlr_layer_shell_v1)
 * note contract:layershell.c berjalan di trierarch compositor jni sebagai compositor display host
 * note contract:buat agar agar mengikuti phoc alih alih wlroots.
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


/* ------------------------------------------------------------------------- */
/* layer_surface resource                                                    */
/* ------------------------------------------------------------------------- */
extern void keyboard_focus_update(
        struct wayland_server *srv,
        struct compositor_surface *surface);

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

    struct compositor_surface *surf =
            wl_resource_get_user_data(resource);

    if (!surf || !surf->layer_surface)
        return;

    /*
     * CONTEXT:
     *
     * Standard wlr-layer-shell:
     *
     *   > 0  = reserve output space
     *    0   = no exclusive reservation
     *   -1   = do not move this surface for other exclusive surfaces
     *
     * Values below -1 are invalid.
     */
    if (zone < -1) {

        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
                "invalid exclusive zone %d",
                zone);

        return;
    }
    surf->layer_surface->exclusive_zone = zone;

    LOGI(
       "layer exclusive reservation changed "
        "surf=%p zone=%d "
        "geometry remains independent "
        "anchor=0x%x margin=%d,%d,%d,%d",
        (void *)surf,
        zone,
        surf->layer_surface->anchor,
        surf->layer_surface->margin_top,
        surf->layer_surface->margin_right,
        surf->layer_surface->margin_bottom,
        surf->layer_surface->margin_left);
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

/*
 * -------------------------------------------------------------------------
 * Layer-shell popup integration
 * -------------------------------------------------------------------------
 *
 * CONTEXT:
 *
 * zwlr_layer_surface_v1.get_popup() menerima xdg_popup resource
 * yang dibuat client dengan parent layer-surface.
 *
 * Layer-shell tidak membuat popup protocol object sendiri.
 * xdg_popup tetap dimiliki oleh xdg-shell.c.
 *
 * Tugas layer-shell hanya:
 *
 *   1. menerima popup resource;
 *   2. memastikan layer surface masih valid;
 *   3. memastikan hanya satu popup yang diregistrasikan;
 *   4. menyimpan popup resource sebagai child dari layer surface.
 *
 * Positioning tetap menggunakan xdg_positioner milik xdg-shell.
 *
 * Tidak ada render traversal di sini.
 * Popup tetap menjadi compositor_surface biasa dan masuk
 * ke srv->surfaces seperti surface lainnya.
 */
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

    /*
     * CONTEXT:
     *
     * Layer surface hanya boleh mempunyai satu popup
     * yang diregistrasikan sebagai child pada satu waktu.
     *
     * Jangan overwrite popup_res karena popup lama masih
     * dapat digunakan oleh client.
     */
    if (surf->layer_surface->popup_res) {

        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
                "layer surface already has a popup");
        return;
    }

    /*
     * Pastikan popup memang berasal dari client yang sama.
     *
     * Ini mencegah resource dari client lain dipasang
     * sebagai child layer surface.
     */
    if (!popup ||
        wl_resource_get_client(popup) !=
            wl_resource_get_client(resource)) {

        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SURFACE_V1_ERROR_INVALID_SURFACE_STATE,
                "popup belongs to another client");
        return;
    }

    /*
     * Simpan popup sebagai child protocol object.
     *
     * Positioning dan configure popup tetap ditangani
     * oleh xdg-shell popup path.
     */
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

    /*
     * PHOC/Trierarch contract:
     *
     * ack_configure() tidak menjadi buffer gate.
     *
     * Client tetap wajib melakukan ack sesuai protocol,
     * tetapi Trierarch tidak menyimpan:
     *
     *   - configured bool
     *   - first_buffer_allowed
     *   - acked_serial
     *
     * Buffer lifecycle ditentukan oleh surface_commit()
     * milik compositor, bukan oleh state machine wlroots.
     */
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
     *
    if (surf->layer_surface ||
        surf->layer_surface_res) {

        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
                "surface already has a layer-shell role");
        return;
    }*/

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
    
    /*
     * PHOC/Trierarch contract:
     *
     * Layer surface langsung menjadi role aktif setelah
     * zwlr_layer_surface_v1 berhasil dibuat.
     *
     * Tidak ada pending/configure gate internal.
     */
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
         * PHOC/Trierarch contract:
         *
         * Initial configure tetap dikirim sebagai bagian
         * dari protocol layer-shell.
         *
         * Configure memberi client geometry yang harus
         * digunakan untuk initial commit.
         *
         * Tidak ada internal pending-buffer/configure gate.
         */
        send_layer_surface_configure(surf);
}
/* AFTER */

/*
 * ================================================================
 * CONTEXT:
 *
 * Layer-shell geometry adalah geometry milik layer surface sendiri.
 *
 * Geometry ini TIDAK boleh berasal dari:
 *
 *     layer_shell_get_work_area()
 *     exclusive_zone
 *     GTK tiling
 *     XDG geometry
 *
 * Sebaliknya, work-area hanya merupakan reservation information
 * untuk consumer lain.
 *
 * IMPORTANT:
 *
 * layer_surface_calculate_size() tidak lagi menulis:
 *
 *     surf->wm_x
 *     surf->wm_y
 *
 * wm_x/wm_y dipakai sebagai geometry compositor/XDG.
 * Layer-shell hanya menghasilkan geometry protocol configure.
 * ================================================================
 */
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

    const uint32_t ow =
        srv->output_width > 0 ?
        srv->output_width : 1;

    const uint32_t oh =
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

    const bool stretch_x = left && right;
    const bool stretch_y = top && bottom;

    /*
     * ------------------------------------------------------------
     * X dimension
     * ------------------------------------------------------------
     *
     * exclusive_zone sengaja TIDAK digunakan.
     *
     * Reservation tidak mengecilkan layer surface.
     */
    if (stretch_x) {
        int64_t w =
            (int64_t)ow -
            ls->margin_left -
            ls->margin_right;

        *width = w > 0 ?
            (uint32_t)w : 1;

    } else if (ls->requested_width > 0) {

        *width = ls->requested_width;

    } else {

        *width = 1;
    }

    /*
     * ------------------------------------------------------------
     * Y dimension
     * ------------------------------------------------------------
     *
     * exclusive_zone sengaja TIDAK digunakan.
     */
    if (stretch_y) {
        int64_t h =
            (int64_t)oh -
            ls->margin_top -
            ls->margin_bottom;

        *height = h > 0 ?
            (uint32_t)h : 1;

    } else if (ls->requested_height > 0) {

        *height = ls->requested_height;

    } else {

        *height = 1;
    }

    /*
     * ============================================================
     * IMPORTANT:
     *
     * Jangan menulis surf->wm_x / surf->wm_y di sini.
     *
     * Layer-shell geometry tidak boleh menjadi geometry authority
     * XDG/compositor.
     *
     * Position layer surface untuk protocol configure dihitung
     * hanya ketika diperlukan oleh layer-shell sendiri.
     * ============================================================
     */
}

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

    layer_surface_calculate_size(
            surf,
            width,
            height);

    /*
     * Layer-shell position adalah protocol geometry.
     * Jangan mengambil geometry XDG/compositor dari wm_x/wm_y.
     */
    struct layer_surface_state *ls =
        surf->layer_surface;

    struct wayland_server *srv =
        surf->srv;

    const uint32_t ow =
        srv->output_width > 0 ?
        srv->output_width : 1;

    const uint32_t oh =
        srv->output_height > 0 ?
        srv->output_height : 1;

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
        *x = ls->margin_left;
    } else if (right) {
        *x =
            (int32_t)ow -
            (int32_t)*width -
            ls->margin_right;
    } else {
        *x =
            ((int32_t)ow -
            (int32_t)*width) / 2;
    }

    if (top) {
        *y = ls->margin_top;
    } else if (bottom) {
        *y =
            (int32_t)oh -
            (int32_t)*height -
            ls->margin_bottom;
    } else {
        *y =
            ((int32_t)oh -
             (int32_t)*height) / 2;
    }

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

    /*
     * CONTEXT:
     * Geometry dihitung langsung dari state layer-shell
     * saat configure dikirim.
     *
     * Tidak ada state pending/configured yang disimpan
     * untuk mengontrol buffer lifecycle.
     */
    layer_surface_calculate_size(
            surf,
            &width,
            &height);

    /*
     * Configure serial tetap diperlukan oleh protocol.
     *
     * Serial ini hanya protocol serial untuk configure.
     * Bukan buffer gate.
     */
    uint32_t serial =
        wl_display_next_serial(surf->srv->display);

    zwlr_layer_surface_v1_send_configure(
            surf->layer_surface_res,
            serial,
            width,
            height);

    LOGI(
        "layer configure surf=%p serial=%u size=%ux%u",
        (void *)surf,
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
