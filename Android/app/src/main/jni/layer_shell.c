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


void trierarch_output_layout_reset(
        struct wayland_server *srv);
/* ------------------------------------------------------------------------- */
/* layer_surface resource                                                    */
/* ------------------------------------------------------------------------- */
extern void keyboard_focus_update(
        struct wayland_server *srv,
        struct compositor_surface *surface);

static void trierarch_output_layout_recompute(
        struct wayland_server *srv,
        struct compositor_surface *exclude)
{
    if (!srv)
        return;

    /*
     * CONTEXT NOTE:
     *
     * Physical Android Surface adalah satu-satunya output authority.
     *
     * Jangan pernah mempertahankan output_width/output_height lama
     * ketika physical output sudah berubah.
     *
     * reset() mengambil ulang:
     *
     *     srv->output_width
     *     srv->output_height
     *
     * sehingga seluruh reservation dihitung ulang terhadap output
     * physical terbaru.
     */
    trierarch_output_layout_reset(srv);

    struct trierarch_output_layout *layout =
        &srv->output_layout;

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
         * exclusive_zone:
         *
         *   > 0 : producer usable-area reservation
         *    0  : no reservation
         *   -1  : exclusive layer, but no area reservation
         */
        if (ls->exclusive_zone <= 0)
            continue;

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

        /*
         * Phoc-style edge reservation.
         */
        const bool valid_top =
            top && !bottom && (left == right);

        const bool valid_bottom =
            bottom && !top && (left == right);

        const bool valid_left =
            left && !right && (top == bottom);

        const bool valid_right =
            right && !left && (top == bottom);

        int64_t reservation;

        if (valid_top) {

            reservation =
                (int64_t)ls->exclusive_zone +
                (int64_t)ls->margin_top;

            if (reservation < 0)
                reservation = 0;

            /*
             * Reservation tidak boleh melebihi physical output.
             */
            if (reservation >
                (int64_t)layout->output_height)

                reservation =
                    layout->output_height;

            if (reservation >
                layout->exclusive_top)

                layout->exclusive_top =
                    (int32_t)reservation;

            continue;
        }

        if (valid_bottom) {

            reservation =
                (int64_t)ls->exclusive_zone +
                (int64_t)ls->margin_bottom;

            if (reservation < 0)
                reservation = 0;

            if (reservation >
                (int64_t)layout->output_height)

                reservation =
                    layout->output_height;

            if (reservation >
                layout->exclusive_bottom)

                layout->exclusive_bottom =
                    (int32_t)reservation;

            continue;
        }

        if (valid_left) {

            reservation =
                (int64_t)ls->exclusive_zone +
                (int64_t)ls->margin_left;

            if (reservation < 0)
                reservation = 0;

            if (reservation >
                (int64_t)layout->output_width)

                reservation =
                    layout->output_width;

            if (reservation >
                layout->exclusive_left)

                layout->exclusive_left =
                    (int32_t)reservation;

            continue;
        }

        if (valid_right) {

            reservation =
                (int64_t)ls->exclusive_zone +
                (int64_t)ls->margin_right;

            if (reservation < 0)
                reservation = 0;

            if (reservation >
                (int64_t)layout->output_width)

                reservation =
                    layout->output_width;

            if (reservation >
                layout->exclusive_right)

                layout->exclusive_right =
                    (int32_t)reservation;

            continue;
        }
    }

    /*
     * ================================================================
     * PHYSICAL OUTPUT -> WORK AREA
     * ================================================================
     *
     * Work area selalu berada di dalam:
     *
     *     [0, output_width)
     *     [0, output_height)
     *
     * Bahkan jika dua exclusive reservation saling bertabrakan.
     */
    int32_t width =
        (int32_t)layout->output_width -
        layout->exclusive_left -
        layout->exclusive_right;

    int32_t height =
        (int32_t)layout->output_height -
        layout->exclusive_top -
        layout->exclusive_bottom;

    if (width < 1)
        width = 1;

    if (height < 1)
        height = 1;

    int32_t x =
        layout->exclusive_left;

    int32_t y =
        layout->exclusive_top;

    /*
     * Jangan biarkan origin keluar dari physical output.
     */
    if (x < 0)
        x = 0;

    if (y < 0)
        y = 0;

    if (x >= (int32_t)layout->output_width)
        x = (int32_t)layout->output_width - 1;

    if (y >= (int32_t)layout->output_height)
        y = (int32_t)layout->output_height - 1;

    /*
     * Pastikan width/height tidak melewati boundary physical output.
     */
    if (width >
        (int32_t)layout->output_width - x)

        width =
            (int32_t)layout->output_width - x;

    if (height >
        (int32_t)layout->output_height - y)

        height =
            (int32_t)layout->output_height - y;

    if (width < 1)
        width = 1;

    if (height < 1)
        height = 1;

    layout->work_area.x = x;
    layout->work_area.y = y;

    layout->work_area.width =
        (uint32_t)width;

    layout->work_area.height =
        (uint32_t)height;

    LOGI(
        "output layout recompute "
        "physical=%ux%u "
        "exclusive[top=%d right=%d bottom=%d left=%d] "
        "work=%ux%u@%d,%d",
        layout->output_width,
        layout->output_height,
        layout->exclusive_top,
        layout->exclusive_right,
        layout->exclusive_bottom,
        layout->exclusive_left,
        layout->work_area.width,
        layout->work_area.height,
        layout->work_area.x,
        layout->work_area.y);
}

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
     * CONTEXT NOTE:
     *
     * Destroying a layer role removes its reservation producer.
     *
     * Karena work-area adalah output-global state, setelah
     * role hilang layout harus direcompute.
     *
     * Ini juga menjamin XDG/GTK tidak membaca exclusive zone
     * dari role yang sudah mati.
     */
    struct wayland_server *srv = surf->srv;

    surf->layer_surface_res = NULL;

    if (surf->layer_surface) {
        surf->layer_surface->popup_res = NULL;
        free(surf->layer_surface);
        surf->layer_surface = NULL;
    }

    /*
     * Layer-shell role sudah benar-benar hilang.
     * Recompute dilakukan setelah state dibersihkan.
     */
    if (srv) {
        pthread_mutex_lock(&srv->surfaces_mutex);

        trierarch_output_layout_recompute(
            srv,
            NULL);

        pthread_mutex_unlock(&srv->surfaces_mutex);
    }
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
     * CONTEXT:
     *
     * set_size() adalah pending layer-shell state.
     *
     * State ini baru menjadi geometry protocol setelah
     * compositor mengirim configure berikutnya.
     *
     * Jangan menyentuh wm_x/wm_y atau work-area di sini.
     */
    surf->layer_surface->requested_width = width;
    surf->layer_surface->requested_height = height;

    /*
     * CONTEXT NOTE:
     *
     * set_size() mengubah protocol geometry state.
     *
     * Trierarch contract tidak menggunakan wlroots pending-state
     * gate, sehingga geometry/configure harus langsung mengikuti
     * state terbaru.
     *
     * Work-area sendiri tetap dihitung dari seluruh layer reservation.
     */
    if (surf->srv) {

        pthread_mutex_lock(
            &surf->srv->surfaces_mutex);

        trierarch_output_layout_recompute(
            surf->srv,
            NULL);

        /*
         * Reconfigure layer surface setelah geometry berubah.
         */
        if (surf->layer_surface_res)
            send_layer_surface_configure(surf);

        pthread_mutex_unlock(
            &surf->srv->surfaces_mutex);
    }

    LOGI(
        "layer set_size surf=%p requested=%ux%u",
        (void *)surf,
        width,
        height);
}

/* AFTER */

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

    /*
     * CONTEXT NOTE:
     *
     * Anchor dapat mengubah edge yang menjadi reservation producer.
     * Karena itu output usable-area harus direcompute setelah state
     * berubah.
     */
    surf->layer_surface->anchor = anchor;

    if (surf->srv) {
        pthread_mutex_lock(&surf->srv->surfaces_mutex);

        trierarch_output_layout_recompute(
            surf->srv,
            NULL);
        /*
         * Anchor/margin/exclusive zone dapat mengubah geometry
         * yang harus dipublish kepada layer client.
         */
        if (surf->layer_surface_res)
            send_layer_surface_configure(surf);

        pthread_mutex_unlock(&surf->srv->surfaces_mutex);
    }

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
    /*
     * CONTEXT NOTE:
     *
     * Protocol state berubah -> output layout harus dihitung ulang.
     *
     * Namun fungsi ini tetap tidak menghitung geometry XDG.
     * Ia hanya invalidates/recomputes output usable-area state.
     */
    surf->layer_surface->exclusive_zone = zone;

    if (surf->srv) {
        pthread_mutex_lock(&surf->srv->surfaces_mutex);

        trierarch_output_layout_recompute(
            surf->srv,
            NULL);
     
       if (surf->layer_surface_res)
           send_layer_surface_configure(surf);

        pthread_mutex_unlock(&surf->srv->surfaces_mutex);
    }

    LOGI(
        "layer exclusive reservation changed "
        "surf=%p zone=%d",
        (void *)surf,
        zone);
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

    /*
     * CONTEXT NOTE:
     *
     * Margin adalah bagian dari reservation geometry.
     * Perubahan margin harus memicu recompute output layout.
     */
    surf->layer_surface->margin_top = top;
    surf->layer_surface->margin_right = right;
    surf->layer_surface->margin_bottom = bottom;
    surf->layer_surface->margin_left = left;

    if (surf->srv) {
        pthread_mutex_lock(&surf->srv->surfaces_mutex);

        trierarch_output_layout_recompute(
            surf->srv,
            NULL);
     
        if (surf->layer_surface_res)
            send_layer_surface_configure(surf);

        pthread_mutex_unlock(&surf->srv->surfaces_mutex);
    }

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
     * ================================================================
     * ROLE OWNERSHIP
     * ================================================================
     *
     * compositor_surface adalah object yang sama dengan yang dibuat
     * oleh wl_compositor.create_surface() di surface.c.
     *
     * layer_surface_state hanya dibuat SATU KALI di sini.
     *
     * Setelah assignment:
     *
     *     surf->layer_surface     -> layer_surface_state
     *     surf->layer_surface_res -> zwlr_layer_surface_v1 resource
     *
     * surface.c tidak membuat state baru.
     * surface.c hanya membaca surf->layer_surface ketika commit.
     *
     * Ini menjaga satu wl_surface == satu layer-shell role.
     */
    if (surf->layer_surface ||
        surf->layer_surface_res ||
        surf->xdg_surface_res ||
        surf->xdg_toplevel_res) {

        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SHELL_V1_ERROR_ROLE,
                "surface already has a layer-shell role");

        LOGE(
           "layer role rejected: surf=%p "
           "layer_surface=%p layer_res=%p "
           "xdg_surface=%p xdg_toplevel=%p",
           (void *)surf,
           (void *)surf->layer_surface,
           (void *)surf->layer_surface_res,
           (void *)surf->xdg_surface_res,
           (void *)surf->xdg_toplevel_res);

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
/*
 * CONTEXT:
 *
 * Helper ini hanya menghitung ukuran layer protocol.
 *
 * Display authority tetap:
 *
 *     srv->output_width
 *     srv->output_height
 *
 * Fungsi ini tidak boleh mengubah:
 *
 *     surf->wm_x
 *     surf->wm_y
 *
 * dan tidak boleh menghitung work-area.
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

    /*
     * Android physical Surface adalah display space
     * Trierarch.
     */
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

    /*
     * Stretch menggunakan display space Android.
     *
     * exclusive_zone TIDAK ikut menentukan ukuran surface.
     */
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
 * CONTEXT NOTE:
 *
 * Public geometry accessor.
 *
 * Fungsi ini hanya membaca output layout authority.
 *
 * Tidak:
 *
 *   - scan layer surfaces
 *   - scan XDG surfaces
 *   - membaca GTK
 *   - menghitung exclusive zone
 *   - bergantung pada zwlr_layer_shell_v1 bind
 *
 * `exclude` tetap dipertahankan untuk compatibility API,
 * tetapi tidak lagi dipakai sebagai source geometry.
 *
 * Jika caller membutuhkan preview layout tanpa surface tertentu,
 * gunakan explicit layout transaction/recompute API, bukan
 * mengubah semantic global accessor ini.
 */
void layer_shell_get_work_area(
        struct wayland_server *srv,
        struct compositor_surface *exclude,
        struct trierarch_work_area *area)
{
    (void)exclude;

    if (!srv || !area)
        return;

    /*
 * CONTEXT NOTE:
 *
 * Physical output adalah baseline yang selalu valid.
 *
 * layer_shell_get_work_area() tidak bergantung pada:
 *
 *     - zwlr_layer_shell_v1 bind
 *     - keberadaan layer surface
 *     - XDG
 *     - GTK
 *
 * Work-area adalah derived state di dalam physical output.
 *
 * Jika layout belum pernah dihitung, physical output tetap
 * menjadi usable-area default.
 */
    const uint32_t physical_width =
        srv->output_width > 0 ?
        srv->output_width : 1;

    const uint32_t physical_height =
        srv->output_height > 0 ?
        srv->output_height : 1;

    if (srv->output_layout.output_width == 0 ||
        srv->output_layout.output_height == 0) {

        area->x = 0;
        area->y = 0;
        area->width = physical_width;
        area->height = physical_height;

        return;
    }

    /*
     * Layout sudah valid.
     *
     * Ini hanya membaca derived work-area.
     * Physical output tetap tidak berubah.
     */
    *area = srv->output_layout.work_area;

    if (area->width == 0 ||
        area->height == 0) {

        area->x = 0;
        area->y = 0;
        area->width = physical_width;
        area->height = physical_height;
    }
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

    /*
     * ============================================================
     * DISPLAY AUTHORITY
     * ============================================================
     *
     * Jangan mengambil ukuran dari:
     *
     *   - GTK
     *   - XDG
     *   - work-area
     *   - exclusive zone
     *   - window geometry
     *
     * Android sudah memberikan physical display geometry
     * melalui srv->output_width / srv->output_height.
     */
    uint32_t width =
        srv->output_width > 0 ?
        srv->output_width : 1;

    uint32_t height =
        srv->output_height > 0 ?
        srv->output_height : 1;

    /*
     * Layer-shell requested size tetap dihormati apabila
     * client tidak meminta stretch pada edge tertentu.
     *
     * Untuk surface fullscreen/edge-anchored, ukuran display
     * tetap berasal dari Android.
     */
    struct layer_surface_state *ls =
        surf->layer_surface;

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

    /*
     * Horizontal stretch:
     *
     * LEFT + RIGHT
     *
     * berarti layer mengikuti lebar Android display.
     */
    if (left && right) {
        int64_t w =
            (int64_t)width -
            ls->margin_left -
            ls->margin_right;

        width = w > 0 ? (uint32_t)w : 1;

    } else if (ls->requested_width > 0) {

        width = ls->requested_width;
    }

    /*
     * Vertical stretch:
     *
     * TOP + BOTTOM
     *
     * berarti layer mengikuti tinggi Android display.
     */
    if (top && bottom) {
        int64_t h =
            (int64_t)height -
            ls->margin_top -
            ls->margin_bottom;

        height = h > 0 ? (uint32_t)h : 1;

    } else if (ls->requested_height > 0) {

        height = ls->requested_height;
    }

    /*
     * Configure serial hanya protocol serial.
     *
     * Tidak digunakan sebagai buffer gate.
     */
    uint32_t serial =
        wl_display_next_serial(srv->display);

    zwlr_layer_surface_v1_send_configure(
        surf->layer_surface_res,
        serial,
        width,
        height);

    LOGI(
        "layer display configure "
        "surf=%p serial=%u "
        "display=%ux%u configure=%ux%u",
        (void *)surf,
        serial,
        srv->output_width,
        srv->output_height,
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

    /*
     * ================================================================
     * PHYSICAL OUTPUT CHANGE
     * ================================================================
     *
     * Android Surface adalah display authority.
     *
     * Begitu output_width/output_height berubah, seluruh reservation
     * layer-shell harus dihitung ulang terhadap physical output baru.
     *
     * Ini dilakukan SEBELUM configure dikirim.
     */
    trierarch_output_layout_recompute(
            srv,
            NULL);

    struct compositor_surface *surf;

    wl_list_for_each(
            surf,
            &srv->surfaces,
            link) {

        if (!surf->layer_surface)
            continue;

        send_layer_surface_configure(
                surf);
    }

    pthread_mutex_unlock(
            &srv->surfaces_mutex);

    LOGI(
        "layer output change synchronized "
        "physical=%ux%u",
        srv->output_width,
        srv->output_height);
}
/*
 * CONTEXT NOTE:
 *
 * Ini adalah equivalent semantic dari:
 *
 *     PhocOutput.usable_area
 *
 * Work-area selalu valid walaupun:
 *
 *     - zwlr_layer_shell_v1 belum bind
 *     - tidak ada layer surface
 *     - XDG belum bind
 *     - GTK belum start
 *
 * Bind protocol tidak menciptakan output layout.
 */
void trierarch_output_layout_reset(
        struct wayland_server *srv)
{
    if (!srv)
        return;

    struct trierarch_output_layout *layout =
        &srv->output_layout;

    const uint32_t ow =
        srv->output_width > 0 ?
        srv->output_width : 1;

    const uint32_t oh =
        srv->output_height > 0 ?
        srv->output_height : 1;

    layout->output_width = ow;
    layout->output_height = oh;

    layout->exclusive_top = 0;
    layout->exclusive_right = 0;
    layout->exclusive_bottom = 0;
    layout->exclusive_left = 0;

    layout->work_area.x = 0;
    layout->work_area.y = 0;
    layout->work_area.width = ow;
    layout->work_area.height = oh;
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
