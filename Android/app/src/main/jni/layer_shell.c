/*
 * wlr-layer-shell (zwlr_layer_shell_v1)
 *
 * Trierarch compositor JNI / Android display host.
 *
 * ================================================================
 * TRIERARCH CONTRACT
 * ================================================================
 *
 * output.c adalah SOURCE OF TRUTH untuk output geometry.
 *
 *     output.c
 *         |
 *         +-- output_width
 *         +-- output_height
 *         |
 *         v
 *     layer_shell_get_layer_surface()
 *         |
 *         v
 *     layer_shell_set_layer_surface()
 *         |
 *         v
 *     send_layer_surface_configure()
 *         |
 *         v
 *     zwlr_layer_surface_v1.configure
 *         |
 *         v
 *     Wayland client
 *
 * Tidak ada geometry calculation kedua di layershell.c.
 *
 * layershell.c tidak menentukan output size.
 *
 * layershell.c hanya membaca:
 *
 *     srv->output_width
 *     srv->output_height
 *
 * lalu meneruskan ukuran tersebut ke configure.
 *
 * Android SurfaceDestroyed tidak menghancurkan wl_surface atau
 * layer-shell role. Lifecycle refresh tetap menggunakan pipeline
 * yang sama.
 *
 * Implementasi policy mengikuti model Phoc sesuai contract
 * Trierarch, bukan asumsi generic wlroots.
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
#define TRIERARCH_LAYER_Z_OVERLAY       (30000)

extern void keyboard_focus_update(
        struct wayland_server *srv,
        struct compositor_surface *surface);

/*
 * ================================================================
 * INTERNAL PIPELINE
 * ================================================================
 *
 * Output refresh selalu melewati:
 *
 *     layer_shell_get_layer_surface()
 *         ↓
 *     layer_shell_set_layer_surface()
 *         ↓
 *     send_layer_surface_configure()
 *
 * Jangan mengirim configure langsung dari caller lain.
 */

static bool layer_shell_get_layer_surface(
        struct compositor_surface *surf,
        uint32_t *width,
        uint32_t *height);

static void layer_shell_set_layer_surface(
        struct compositor_surface *surf,
        uint32_t width,
        uint32_t height);

static void send_layer_surface_configure(
        struct compositor_surface *surf,
        uint32_t width,
        uint32_t height);

void layer_surface_notify_output_change(
        struct wayland_server *srv);


/*
 * ================================================================
 * LAYER RESOURCE LIFETIME
 * ================================================================
 */

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

    /*
     * CONTEXT:
     *
     * Ini hanya destroy wl_resource layer-shell.
     *
     * Android SurfaceDestroyed TIDAK masuk melalui callback ini.
     *
     * layer_surface_state lifetime mengikuti compositor_surface
     * lifecycle di surface.c.
     *
     * Karena itu jangan free surf->layer_surface di sini.
     */
    surf->layer_surface_res = NULL;

    /*
     * layer_surface_state tetap hidup sampai compositor_surface
     * melakukan teardown role/state secara penuh.
     */
}


/*
 * ================================================================
 * zwlr_layer_surface_v1 REQUESTS
 * ================================================================
 */

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
        "margin=%d,%d,%d,%d",
        (void *)surf,
        zone,
        anchor,
        edge,
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

    struct compositor_surface *surf =
            wl_resource_get_user_data(resource);

    if (!surf || !surf->layer_surface)
        return;

    surf->layer_surface->margin_top = top;
    surf->layer_surface->margin_right = right;
    surf->layer_surface->margin_bottom = bottom;
    surf->layer_surface->margin_left = left;

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

    if (mode ==
            ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE &&
        surf->srv &&
        surf->srv->keyboard_focus == surf) {
        keyboard_focus_update(
                surf->srv,
                NULL);
    }

    LOGI(
        "layer keyboard interactivity "
        "surf=%p mode=%u",
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

    if (!surf || !surf->layer_surface) {
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
        "layer popup attached "
        "surf=%p popup=%p",
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
        "layer configure ack "
        "surf=%p serial=%u",
        (void *)surf,
        serial);
}

/*
 * IMPORTANT:
 *
 * Ini adalah REQUEST HANDLER protocol.
 *
 * Jangan gunakan nama layer_shell_set_layer_surface().
 *
 * layer_shell_set_layer_surface() dicadangkan untuk internal
 * Trierarch configure pipeline.
 */
static void layer_surface_set_layer_request(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t layer)
{
    (void)client;

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

    LOGI(
        "layer set_layer "
        "surf=%p layer=%u z=%d",
        (void *)surf,
        layer,
        surf->z_order);
}


/*
 * ================================================================
 * LAYER SURFACE PROTOCOL IMPLEMENTATION
 * ================================================================
 */

static const struct zwlr_layer_surface_v1_interface
layer_surface_impl = {
    .destroy =
        layer_surface_destroy,

    .set_size =
        layer_surface_set_size,

    .set_anchor =
        layer_surface_set_anchor,

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
        layer_surface_set_layer_request,
};


/*
 * ================================================================
 * LAYER-SHELL GLOBAL
 * ================================================================
 */

static void layer_shell_destroy(
        struct wl_client *client,
        struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}


/*
 * ================================================================
 * INTERNAL OUTPUT → LAYER-SHELL PIPELINE
 * ================================================================
 *
 * Ini BUKAN protocol request.
 *
 * Fungsi ini hanya mengambil output geometry dari server.
 *
 * SOURCE OF TRUTH:
 *
 *     srv->output_width
 *     srv->output_height
 */

static bool layer_shell_get_layer_surface(
        struct compositor_surface *surf,
        uint32_t *width,
        uint32_t *height)
{
    if (!surf ||
        !surf->srv ||
        !surf->layer_surface ||
        !surf->layer_surface_res ||
        !width ||
        !height)
        return false;

    struct wayland_server *srv = surf->srv;

    /*
     * OUTPUT.C IS THE AUTHORITY.
     *
     * Jangan mengambil ukuran dari:
     *
     *   GTK
     *   XDG
     *   wl_surface cached size
     *   Android Surface object
     *   exclusive zone
     *   margin
     *   work area
     */
    const int32_t output_width =
            srv->output_width;

    const int32_t output_height =
            srv->output_height;

    if (output_width <= 0 ||
        output_height <= 0) {
        LOGE(
            "layer get surface rejected "
            "invalid output geometry "
            "surf=%p output=%dx%d",
            (void *)surf,
            output_width,
            output_height);

        return false;
    }

    *width = (uint32_t)output_width;
    *height = (uint32_t)output_height;

    LOGI(
        "layer get surface "
        "surf=%p output=%ux%u",
        (void *)surf,
        *width,
        *height);

    return true;
}


/*
 * ================================================================
 * INTERNAL LAYER-SURFACE SET
 * ================================================================
 *
 * Fungsi ini adalah titik masuk configure setelah geometry
 * diperoleh dari layer_shell_get_layer_surface().
 *
 * Tidak menghitung geometry.
 * Tidak membaca physical Android surface.
 * Tidak membuat ukuran alternatif.
 */

static void layer_shell_set_layer_surface(
        struct compositor_surface *surf,
        uint32_t width,
        uint32_t height)
{
    if (!surf ||
        !surf->layer_surface ||
        !surf->layer_surface_res ||
        !surf->srv)
        return;

    /*
     * Configure selalu diteruskan ke satu implementation.
     */
    send_layer_surface_configure(
            surf,
            width,
            height);
}


/*
 * ================================================================
 * CONFIGURE IMPLEMENTATION
 * ================================================================
 *
 * Satu-satunya tempat di Trierarch yang mengirim
 * zwlr_layer_surface_v1.configure.
 *
 * Geometry berasal langsung dari output.c melalui argument.
 */

static void send_layer_surface_configure(
        struct compositor_surface *surf,
        uint32_t width,
        uint32_t height)
{
    if (!surf ||
        !surf->layer_surface ||
        !surf->layer_surface_res ||
        !surf->srv)
        return;

    struct wayland_server *srv =
            surf->srv;

    if (width == 0 || height == 0) {
        LOGE(
            "layer configure rejected "
            "surf=%p geometry=%ux%u",
            (void *)surf,
            width,
            height);
        return;
    }

    uint32_t serial =
            wl_display_next_serial(
                    srv->display);

    zwlr_layer_surface_v1_send_configure(
            surf->layer_surface_res,
            serial,
            width,
            height);

    LOGI(
        "layer configure "
        "surf=%p mode=%s serial=%u "
        "geometry=%ux%u@%d,%d "
        "anchor=0x%x "
        "margin=%d,%d,%d,%d "
        "exclusive=%d layer=%u",
        (void *)surf,
        srv->wm_mode == WM_MODE_DIRECT ?
            "DIRECT" : "NESTED",
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


/*
 * ================================================================
 * WAYLAND get_layer_surface REQUEST
 * ================================================================
 *
 * Ini adalah protocol entrypoint.
 *
 * Nama sengaja dibedakan dari internal:
 *
 *     layer_shell_get_layer_surface()
 *
 * supaya tidak ambigu.
 *
 * Initial role creation:
 *
 *     client
 *       ↓
 *     get_layer_surface request
 *       ↓
 *     compositor_surface role
 *       ↓
 *     initial output geometry
 *       ↓
 *     configure
 */

static void layer_shell_get_layer_surface_request(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id,
        struct wl_resource *surface_res,
        struct wl_resource *output,
        uint32_t layer,
        const char *namespace)
{
    /*
     * Trierarch menggunakan output.c sebagai authority.
     *
     * Client-provided wl_output tidak digunakan untuk menentukan
     * ukuran configure.
     */
    (void)output;

    struct wayland_server *srv =
            wl_resource_get_user_data(resource);

    struct compositor_surface *surf =
            wl_resource_get_user_data(surface_res);

    if (!srv ||
        !surf ||
        surf->srv != srv) {
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
            "layer role rejected: "
            "surf=%p already has "
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

    if (layer >
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wl_resource_post_error(
                resource,
                ZWLR_LAYER_SHELL_V1_ERROR_INVALID_LAYER,
                "invalid layer %u",
                layer);
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
    state->layer = layer;

    /*
     * Existing Trierarch policy.
     */
    state->exclusive_zone = 120;

    if (namespace) {
        strncpy(
                state->namespace_name,
                namespace,
                sizeof(state->namespace_name) - 1);
    }

    surf->layer_surface = state;
    surf->layer_surface_res = layer_res;

    switch (layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        surf->z_order =
                TRIERARCH_LAYER_Z_BACKGROUND;
        break;

    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        surf->z_order =
                TRIERARCH_LAYER_Z_BOTTOM;
        break;

    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        surf->z_order =
                TRIERARCH_LAYER_Z_TOP;
        break;

    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
    default:
        surf->z_order =
                TRIERARCH_LAYER_Z_OVERLAY;
        break;
    }

    surf->wm_x = 0;
    surf->wm_y = 0;

    wl_resource_set_implementation(
            layer_res,
            &layer_surface_impl,
            surf,
            layer_surface_resource_destroy);

    /*
     * ============================================================
     * INITIAL OUTPUT PIPELINE
     * ============================================================
     *
     * Jangan configure langsung di sini.
     *
     * Gunakan pipeline yang sama dengan output refresh:
     *
     *     output state
     *         ↓
     *     get
     *         ↓
     *     set
     *         ↓
     *     configure
     */
    uint32_t width = 0;
    uint32_t height = 0;

    if (layer_shell_get_layer_surface(
            surf,
            &width,
            &height)) {

        layer_shell_set_layer_surface(
                surf,
                width,
                height);
    }

    LOGI(
        "new layer_surface "
        "surf=%p layer=%u ns=%s",
        (void *)surf,
        layer,
        namespace ? namespace : "");
}


/*
 * ================================================================
 * GLOBAL INTERFACE
 * ================================================================
 */

static const struct zwlr_layer_shell_v1_interface
layer_shell_impl = {
    .destroy =
        layer_shell_destroy,

    .get_layer_surface =
        layer_shell_get_layer_surface_request,
};


/*
 * ================================================================
 * OUTPUT CHANGE / ANDROID LIFECYCLE REFRESH
 * ================================================================
 *
 * Android SurfaceDestroyed TIDAK menghapus role layer-shell.
 *
 * Ketika output/lifecycle berubah, jangan membuat geometry baru
 * di sini.
 *
 * Jalur tetap:
 *
 *     output.c
 *         ↓
 *     output_width/output_height
 *         ↓
 *     layer_shell_get_layer_surface()
 *         ↓
 *     layer_shell_set_layer_surface()
 *         ↓
 *     send_layer_surface_configure()
 */

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

        if (!surf->layer_surface_res) {
            LOGI(
                "layer output change skipped "
                "surf=%p logical=%dx%d "
                "layer resource unavailable",
                (void *)surf,
                srv->output_width,
                srv->output_height);

            continue;
        }

        uint32_t width = 0;
        uint32_t height = 0;

        /*
         * STEP 1:
         *
         * Ambil ukuran terbaru dari output.c.
         */
        if (!layer_shell_get_layer_surface(
                surf,
                &width,
                &height)) {
            continue;
        }

        /*
         * STEP 2:
         *
         * Teruskan state output ke layer-surface.
         */
        layer_shell_set_layer_surface(
                surf,
                width,
                height);

        LOGI(
            "layer output change "
            "surf=%p routed "
            "output=%ux%u",
            (void *)surf,
            width,
            height);
    }

    pthread_mutex_unlock(
            &srv->surfaces_mutex);
}


/*
 * ================================================================
 * GLOBAL BIND
 * ================================================================
 */

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

    LOGI(
        "bind zwlr_layer_shell_v1 version=%u",
        version);
}
