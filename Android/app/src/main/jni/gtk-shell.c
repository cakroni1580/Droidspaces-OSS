#include "server_internal.h"
#include "gtk-shell-server-protocol.h"

#include <android/log.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "TrierarchGtkShell"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/*
 * ================================================================
 * GTK SHELL ARCHITECTURE
 * ================================================================
 *
 * gtk-shell.c TIDAK memiliki authority terhadap:
 *
 *   - surface geometry
 *   - wm_x / wm_y
 *   - output size
 *   - maximize/fullscreen
 *   - configure serial
 *   - xdg_toplevel.configure
 *   - gtk_surface1.configure
 *
 * Semua geometry/state diproses oleh compositor/output.c.
 *
 * gtk-shell.c hanya menjadi protocol bridge antara GTK client
 * dengan compositor_surface.
 *
 * Flow:
 *
 *   GTK client
 *       |
 *       +--> gtk_shell1
 *       |
 *       +--> gtk_surface1
 *               |
 *               v
 *        compositor_surface
 *               |
 *               v
 *            output.c
 *               |
 *        +------+------+
 *        |             |
 *      xdg-shell    gtk-shell
 *
 * ================================================================
 */


/*
 * Keyboard focus bridge.
 *
 * GTK request_focus -> compositor keyboard focus.
 *
 * GTK tidak menjadi authority.
 */
extern void keyboard_focus_update(
        struct wayland_server *srv,
        struct compositor_surface *surface);
/*
 * Return work-area yang boleh digunakan GTK tiling.
 *
 * IMPORTANT:
 *
 * Jangan menghitung exclusive_zone di sini.
 * layer-shell.c adalah authority untuk work-area.
 */
extern void layer_shell_get_work_area(
        struct wayland_server *srv,
        struct compositor_surface *exclude,
        struct trierarch_work_area *area);


/* --------------------------------------------------------------- */
/* gtk_surface1 state                                              */
/* --------------------------------------------------------------- */

struct gtk_surface_state {
    struct wl_resource *resource;
    struct wl_resource *wl_surface;

    /*
     * Direct reference ke compositor surface.
     *
     * Tidak memakai DBus karena Trierarch embedded di Android JNI.
     */
    struct compositor_surface *surface;

    /*
     * Lifecycle listener untuk wl_surface.
     */
    struct wl_listener surface_destroy_listener;
};


/* Forward declarations. */

static const struct gtk_surface1_interface gtk_surface_impl;
static const struct gtk_shell1_interface gtk_shell_impl;


/* --------------------------------------------------------------- */
/* wl_surface lifecycle                                             */
/* --------------------------------------------------------------- */
/*
 * ================================================================
 * GTK SHELL <-> LAYER-SHELL WORK-AREA BRIDGE
 * ================================================================
 *
 * GTK shell tidak membaca:
 *
 *     layer_surface->exclusive_zone
 *
 * secara langsung.
 *
 * Source of truth work-area tetap berada di layer-shell.c:
 *
 *     layer_shell_get_work_area()
 *
 * API ini sudah menangani:
 *
 *     exclusive_zone
 *     anchor
 *     margin
 *     WM_MODE_DIRECT / WM_MODE_NESTED
 *
 * GTK shell hanya meminta hasil akhirnya.
 *
 * Flow:
 *
 *     layer-shell exclusive_zone
 *              |
 *              v
 *     layer_shell_get_work_area()
 *              |
 *              v
 *       trierarch_work_area
 *              |
 *              v
 *       GTK tiling geometry
 * ================================================================
 */

static bool gtk_shell_get_work_area(
        struct compositor_surface *surf,
        struct trierarch_work_area *area)
{
    if (!surf ||
        !surf->srv ||
        !area)
        return false;

    /*
     * layer-shell API sudah menangani:
     *
     *     WM_MODE_DIRECT
     *     WM_MODE_NESTED
     *
     * serta seluruh exclusive-zone calculation.
     */
    layer_shell_get_work_area(
        surf->srv,
        surf,
        area);

    return true;
}


/*
 * GTK tiling geometry.
 *
 * Work-area berasal dari layer-shell.
 * Tiling state tetap berasal dari compositor_surface.
 */
static bool gtk_shell_get_tiling_geometry(
        struct compositor_surface *surf,
        int32_t *x,
        int32_t *y,
        uint32_t *width,
        uint32_t *height)
{
    if (!surf ||
        !surf->srv ||
        !x ||
        !y ||
        !width ||
        !height)
        return false;

    /*
     * Tiling hanya digunakan pada DIRECT.
     */
    if (surf->srv->wm_mode != WM_MODE_DIRECT)
        return false;

    uint32_t tiling =
        gtk_surface_get_tiling_state(surf);

    if (!tiling)
        return false;

    struct trierarch_work_area area;

    if (!gtk_shell_get_work_area(
            surf,
            &area))
        return false;

    /*
     * ------------------------------------------------------------
     * Tiling geometry memakai DIRECT work-area.
     *
     * Untuk tahap ini state tiling tetap menjadi source of truth.
     * API ini hanya menyediakan area yang sudah dikurangi
     * exclusive-zone layer-shell.
     * ------------------------------------------------------------
     */
    *x = area.x;
    *y = area.y;
    *width = area.width;
    *height = area.height;

    LOGI(
        "gtk tiling work-area "
        "surface=%p "
        "tiling=0x%x "
        "area=%ux%u+%d+%d",
        (void *)surf,
        tiling,
        area.width,
        area.height,
        area.x,
        area.y);

    return true;
}

static void gtk_surface_wl_surface_destroy(
        struct wl_listener *listener,
        void *data)
{
    (void)data;

    struct gtk_surface_state *state =
        wl_container_of(listener, state,
                        surface_destroy_listener);

    /*
     * wl_surface sudah mati.
     *
     * Putuskan seluruh reference agar request GTK berikutnya
     * tidak pernah mengakses compositor_surface yang sudah tidak
     * valid.
     */
    state->wl_surface = NULL;
    state->surface = NULL;

    LOGI("gtk_surface wl_surface destroyed");
}


/* --------------------------------------------------------------- */
/* gtk_surface resource lifecycle                                   */
/* --------------------------------------------------------------- */

static void gtk_surface_resource_destroy(
        struct wl_resource *resource)
{
    struct gtk_surface_state *state =
        wl_resource_get_user_data(resource);

    if (!state)
        return;

    /*
     * Putuskan hubungan:
     *
     * compositor_surface -> gtk_surface_state
     */
    if (state->surface &&
        state->surface->gtk_surface == state) {

        state->surface->gtk_surface = NULL;
    }

    /*
     * Remove wl_surface destroy listener.
     */
    if (state->surface_destroy_listener.link.prev &&
        state->surface_destroy_listener.link.next) {

        wl_list_remove(
            &state->surface_destroy_listener.link);

        wl_list_init(
            &state->surface_destroy_listener.link);
    }

    free(state);
}


/* --------------------------------------------------------------- */
/* GTK metadata                                                     */
/* --------------------------------------------------------------- */

static void gtk_surface_set_dbus_properties(
        struct wl_client *client,
        struct wl_resource *resource,
        const char *application_id,
        const char *app_menu_path,
        const char *menubar_path,
        const char *window_object_path,
        const char *application_object_path,
        const char *unique_bus_name)
{
    (void)client;
    (void)resource;

    /*
     * Trierarch embedded Android tidak menggunakan metadata
     * DBus milik GTK.
     *
     * Request tetap diterima agar client GTK kompatibel.
     */
    LOGI(
        "gtk_surface.set_dbus_properties "
        "app_id='%s' "
        "app_menu='%s' "
        "menubar='%s' "
        "window='%s' "
        "application='%s' "
        "bus='%s'",
        application_id ? application_id : "(null)",
        app_menu_path ? app_menu_path : "(null)",
        menubar_path ? menubar_path : "(null)",
        window_object_path ? window_object_path : "(null)",
        application_object_path ? application_object_path : "(null)",
        unique_bus_name ? unique_bus_name : "(null)");
}


/* --------------------------------------------------------------- */
/* GTK modal                                                         */
/* --------------------------------------------------------------- */

static void gtk_surface_set_modal(
        struct wl_client *client,
        struct wl_resource *resource)
{
    (void)client;
    (void)resource;

    /*
     * Modal state belum menjadi authority GTK.
     *
     * Jangan mengubah geometry atau focus di sini.
     */
}

static void gtk_surface_unset_modal(
        struct wl_client *client,
        struct wl_resource *resource)
{
    (void)client;
    (void)resource;

    /*
     * Modal state belum dikelola oleh gtk-shell.
     */
}


/* --------------------------------------------------------------- */
/* GTK present                                                       */
/* --------------------------------------------------------------- */

static void gtk_surface_present(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t timestamp)
{
    (void)client;

    struct gtk_surface_state *state =
        wl_resource_get_user_data(resource);

    if (!state || !state->surface)
        return;

    /*
     * GTK memberitahukan bahwa surface ingin dipresentasikan.
     *
     * IMPORTANT:
     *
     * Jangan mengirim configure dari sini.
     *
     * Geometry/configure sekarang merupakan responsibility
     * compositor/output.c.
     */
    LOGI(
        "gtk_surface.present "
        "timestamp=%u surface=%p",
        timestamp,
        (void *)state->surface);

    /*
     * Tidak ada:
     *
     *     send_toplevel_configure()
     *
     * di sini.
     *
     * output.c akan membaca compositor_surface dan menentukan
     * geometry/configure yang benar.
     */
}


/* --------------------------------------------------------------- */
/* GTK focus request                                                 */
/* --------------------------------------------------------------- */

static void gtk_surface_request_focus(
        struct wl_client *client,
        struct wl_resource *resource,
        const char *startup_id)
{
    (void)client;
    (void)startup_id;

    struct gtk_surface_state *state =
        wl_resource_get_user_data(resource);

    if (!state || !state->surface)
        return;

    struct compositor_surface *surf =
        state->surface;

    if (!surf->srv)
        return;

    /*
     * GTK meminta surface menjadi keyboard focus.
     *
     * Focus authority tetap berada di compositor.
     */
    LOGI(
        "gtk_surface.request_focus "
        "surface=%p app_id=%s title=%s",
        (void *)surf,
        surf->app_id,
        surf->title);

    keyboard_focus_update(
        surf->srv,
        surf);

    /*
     * Jangan mengirim configure di sini.
     *
     * Setelah focus berubah, output/compositor state akan menjadi
     * sumber configure.
     */
}


/* --------------------------------------------------------------- */
/* gtk_shell.get_gtk_surface                                        */
/* --------------------------------------------------------------- */

static void gtk_shell_get_gtk_surface(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id,
        struct wl_resource *surface)
{
    struct wayland_server *srv =
        wl_resource_get_user_data(resource);

    struct compositor_surface *surf =
        surface ? wl_resource_get_user_data(surface) : NULL;

    /*
     * ------------------------------------------------------------
     * Validate wl_surface.
     *
     * gtk-shell protocol ini tidak mendefinisikan
     * GTK_SHELL1_ERROR_INVALID_SURFACE.
     *
     * Karena itu invalid surface cukup ditolak oleh compositor.
     * ------------------------------------------------------------
     */
    if (!surf || !srv || surf->srv != srv) {

        LOGE(
            "gtk_shell.get_gtk_surface: invalid wl_surface "
            "surface=%p compositor_surface=%p srv=%p",
            (void *)surface,
            (void *)surf,
            (void *)srv);

        return;
    }

    /*
     * ------------------------------------------------------------
     * Satu wl_surface hanya boleh memiliki satu gtk_surface1.
     * ------------------------------------------------------------
     */
    if (surf->gtk_surface) {

        LOGE(
            "gtk_shell.get_gtk_surface: "
            "surface already has gtk_surface "
            "surface=%p",
            (void *)surf);

        return;
    }

    struct wl_resource *gtk_surface =
        wl_resource_create(
            client,
            &gtk_surface1_interface,
            wl_resource_get_version(resource),
            id);

    if (!gtk_surface) {
        wl_client_post_no_memory(client);
        return;
    }

    struct gtk_surface_state *state =
        calloc(1, sizeof(*state));

    if (!state) {
        wl_resource_destroy(gtk_surface);
        wl_client_post_no_memory(client);
        return;
    }

    wl_list_init(
        &state->surface_destroy_listener.link);

    /*
     * Link:
     *
     * gtk_surface_state
     *        |
     *        +----> wl_surface
     *        |
     *        +----> compositor_surface
     */
    state->resource = gtk_surface;
    state->wl_surface = surface;
    state->surface = surf;

    state->surface_destroy_listener.notify =
        gtk_surface_wl_surface_destroy;

    wl_resource_add_destroy_listener(
        surface,
        &state->surface_destroy_listener);

    /*
     * Reverse link:
     *
     * compositor_surface -> gtk_surface_state
     */
    surf->gtk_surface = state;

    wl_resource_set_implementation(
        gtk_surface,
        &gtk_surface_impl,
        state,
        gtk_surface_resource_destroy);

    LOGI(
        "gtk_surface1 created "
        "surface=%p",
        (void *)surf);
}

/* --------------------------------------------------------------- */
/* GTK state translation                                             */
/* --------------------------------------------------------------- */

/*
 * GTK shell mempunyai state tiling sendiri.
 *
 * Source of truth tetap compositor_surface.
 *
 * GTK shell hanya menerjemahkan state compositor menjadi:
 *
 *   GTK_SURFACE1_STATE_TILED
 *   GTK_SURFACE1_STATE_TILED_TOP
 *   GTK_SURFACE1_STATE_TILED_RIGHT
 *   GTK_SURFACE1_STATE_TILED_BOTTOM
 *   GTK_SURFACE1_STATE_TILED_LEFT
 *
 * GTK shell TIDAK menentukan apakah surface harus tiled.
 */

/*
 * Return non-zero apabila compositor menganggap surface
 * sedang tiled.
 *
 * API ini sengaja dipisahkan dari gtk-shell.c supaya
 * xdg-shell dan layer-shell dapat memakai state yang sama.
 *
 * Implementasinya harus membaca state authority milik
 * compositor_surface.
 */
static uint32_t gtk_surface_get_tiling_state(
        struct compositor_surface *surf)
{
    if (!surf)
        return 0;

    switch (compositor_surface_get_tiling(surf)) {

    case COMPOSITOR_TILING_ALL:
        return GTK_SURFACE1_STATE_TILED;

    case COMPOSITOR_TILING_TOP:
        return GTK_SURFACE1_STATE_TILED_TOP;

    case COMPOSITOR_TILING_RIGHT:
        return GTK_SURFACE1_STATE_TILED_RIGHT;

    case COMPOSITOR_TILING_BOTTOM:
        return GTK_SURFACE1_STATE_TILED_BOTTOM;

    case COMPOSITOR_TILING_LEFT:
        return GTK_SURFACE1_STATE_TILED_LEFT;

    case COMPOSITOR_TILING_NONE:
    default:
        return 0;
    }
}


/*
 * ---------------------------------------------------------------
 * GTK edge constraints
 * ---------------------------------------------------------------
 *
 * GTK shell v2 menyediakan configure_edges().
 *
 * Ini BUKAN posisi window.
 *
 * Ini memberi tahu GTK edge mana yang masih boleh di-resize.
 *
 * Karena compositor Trierarch saat ini belum mempunyai
 * edge-specific resize policy, jangan mengarang constraint.
 *
 * Array kosong berarti tidak ada constraint khusus.
 */
static void gtk_surface_build_edge_constraints(
        struct compositor_surface *surf,
        struct wl_array *edges)
{
    wl_array_init(edges);

    if (!surf)
        return;

    uint32_t resize_edges =
        compositor_surface_get_resize_edges(surf);

    /*
     * GTK shell protocol menggunakan satu uint32_t
     * untuk setiap edge.
     */

    if (resize_edges & COMPOSITOR_RESIZE_TOP) {

        uint32_t *edge =
            wl_array_add(edges, sizeof(*edge));

        if (edge)
            *edge = GTK_SURFACE1_EDGE_RESIZABLE_TOP;
    }

    if (resize_edges & COMPOSITOR_RESIZE_RIGHT) {

        uint32_t *edge =
            wl_array_add(edges, sizeof(*edge));

        if (edge)
            *edge = GTK_SURFACE1_EDGE_RESIZABLE_RIGHT;
    }

    if (resize_edges & COMPOSITOR_RESIZE_BOTTOM) {

        uint32_t *edge =
            wl_array_add(edges, sizeof(*edge));

        if (edge)
            *edge = GTK_SURFACE1_EDGE_RESIZABLE_BOTTOM;
    }

    if (resize_edges & COMPOSITOR_RESIZE_LEFT) {

        uint32_t *edge =
            wl_array_add(edges, sizeof(*edge));

        if (edge)
            *edge = GTK_SURFACE1_EDGE_RESIZABLE_LEFT;
    }
}


/* --------------------------------------------------------------- */
/* GTK startup notification                                          */
/* --------------------------------------------------------------- */

static void gtk_shell_set_startup_id(
        struct wl_client *client,
        struct wl_resource *resource,
        const char *startup_id)
{
    (void)client;
    (void)resource;
    (void)startup_id;

    /*
     * Trierarch embedded tidak menggunakan GTK startup
     * notification sebagai window-management authority.
     */
}


/* --------------------------------------------------------------- */
/* GTK system bell                                                   */
/* --------------------------------------------------------------- */

static void gtk_shell_system_bell(
        struct wl_client *client,
        struct wl_resource *resource,
        struct wl_resource *surface)
{
    (void)client;
    (void)resource;
    (void)surface;

    LOGI("gtk_shell.system_bell");
}


/* --------------------------------------------------------------- */
/* GTK launch notification                                           */
/* --------------------------------------------------------------- */

static void gtk_shell_notify_launch(
        struct wl_client *client,
        struct wl_resource *resource,
        const char *startup_id)
{
    (void)client;
    (void)resource;
    (void)startup_id;

    LOGI("gtk_shell.notify_launch");
}


/* --------------------------------------------------------------- */
/* Protocol implementations                                          */
/* --------------------------------------------------------------- */

static const struct gtk_surface1_interface gtk_surface_impl = {
    .set_dbus_properties = gtk_surface_set_dbus_properties,
    .set_modal           = gtk_surface_set_modal,
    .unset_modal         = gtk_surface_unset_modal,
    .present             = gtk_surface_present,
    .request_focus       = gtk_surface_request_focus,
};


static const struct gtk_shell1_interface gtk_shell_impl = {
    .get_gtk_surface = gtk_shell_get_gtk_surface,
    .set_startup_id  = gtk_shell_set_startup_id,
    .system_bell     = gtk_shell_system_bell,
    .notify_launch   = gtk_shell_notify_launch,
}

/*
 * --------------------------------------------------------------------
 * GTK surface configure sender.
 *
 * Dipanggil oleh output.c setelah compositor_surface geometry
 * selesai diproses.
 *
 * Geometry tidak dihitung di gtk-shell.c.
 *
 * gtk-shell hanya menerjemahkan state compositor menjadi
 * protocol GTK.
 *
 * WM_MODE_DIRECT:
 *
 *     compositor = WM authority
 *     tiling + resize state boleh dipublikasikan
 *
 * WM_MODE_NESTED:
 *
 *     host WM = WM authority
 *     gtk-shell tetap aktif
 *     tetapi compositor tidak memaksakan tiling/resize state
 * --------------------------------------------------------------------
 */
void send_gtk_surface_configure(
        struct compositor_surface *surf)
{
    if (!surf ||
        !surf->gtk_surface ||
        !surf->srv)
        return;

    struct gtk_surface_state *state =
        surf->gtk_surface;

    if (!state->resource)
        return;

    /*
     * ------------------------------------------------------------
     * Geometry source of truth.
     * ------------------------------------------------------------
     */
    int32_t width = 0;
    int32_t height = 0;

    /*
     * ================================================================
     * GTK geometry source
     * ================================================================
     *
     * DIRECT + tiled:
     *
     *     layer-shell exclusive_zone
     *              ↓
     *       DIRECT work-area
     *              ↓
     *       GTK tiling geometry
     *
     * DIRECT + non-tiled:
     *
     *     normal compositor geometry
     *
     * NESTED:
     *
     *     normal compositor geometry
     *     (tiling API tidak digunakan)
     * ================================================================
     */
    if (!gtk_shell_get_tiling_geometry(
            surf,
            &surf->wm_x,
            &surf->wm_y,
            (uint32_t *)&width,
            (uint32_t *)&height)) {

        compositor_surface_get_logical_size(
            surf,
            &width,
            &height);
    }

    if (width <= 0 || height <= 0)
        return;

    /*
     * ------------------------------------------------------------
     * GTK configure payload.
     * ------------------------------------------------------------
     */
    struct wl_array states;
    struct wl_array edges;

    wl_array_init(&states);
    wl_array_init(&edges);

    /*
     * ------------------------------------------------------------
     * DIRECT
     * ------------------------------------------------------------
     *
     * Compositor menjadi WM authority.
     */
    if (surf->srv->wm_mode == WM_MODE_DIRECT) {

        /*
         * Tiling state berasal dari compositor_surface.
         */
        uint32_t gtk_state =
            gtk_surface_get_tiling_state(surf);

        if (gtk_state != 0) {

            uint32_t *state_id =
                wl_array_add(
                    &states,
                    sizeof(*state_id));

            if (state_id)
                *state_id = gtk_state;
        }

        /*
         * Resize constraints juga berasal dari
         * compositor_surface.
         */
        gtk_surface_build_edge_constraints(
            surf,
            &edges);

        LOGI(
            "gtk configure DIRECT "
            "surface=%p "
            "geometry=%dx%d "
            "tiling=%u "
            "resize_edges=0x%x",
            (void *)surf,
            (int)width,
            (int)height,
            gtk_state,
            compositor_surface_get_resize_edges(surf));
    }

    /*
     * ------------------------------------------------------------
     * NESTED
     * ------------------------------------------------------------
     *
     * GTK tetap menerima configure.
     *
     * Tetapi tidak ada WM state yang dipaksakan compositor.
     */
    else {

        LOGI(
            "gtk configure NESTED "
            "surface=%p "
            "geometry=%dx%d "
            "tiling=ignored "
            "resize=ignored",
            (void *)surf,
            (int)width,
            (int)height);
    }

    /*
     * ------------------------------------------------------------
     * Send GTK protocol events.
     * ------------------------------------------------------------
     *
     * Configure GTK tidak menentukan geometry.
     *
     * Geometry sudah diproses oleh compositor/output.c.
     */
    gtk_surface1_send_configure(
        state->resource,
        &states);

    gtk_surface1_send_configure_edges(
        state->resource,
        &edges);

    wl_array_release(&states);
    wl_array_release(&edges);

    LOGI(
        "send_gtk_surface_configure "
        "surface=%p "
        "mode=%s "
        "geometry=%dx%d",
        (void *)surf,
        surf->srv->wm_mode == WM_MODE_DIRECT
            ? "DIRECT"
            : "NESTED",
        (int)width,
        (int)height);
}

void compositor_surface_set_tiling(
        struct compositor_surface *surf,
        enum compositor_tiling_state state)
{
    if (!surf)
        return;

    surf->tiling_state = state;
}

enum compositor_tiling_state
compositor_surface_get_tiling(
        struct compositor_surface *surf)
{
    if (!surf)
        return COMPOSITOR_TILING_NONE;

    return surf->tiling_state;
}


void compositor_surface_set_resize_edges(
        struct compositor_surface *surf,
        uint32_t edges)
{
    if (!surf)
        return;

    surf->resize_edges = edges;
}

uint32_t compositor_surface_get_resize_edges(
        struct compositor_surface *surf)
{
    if (!surf)
        return COMPOSITOR_RESIZE_NONE;

    return surf->resize_edges;
}

void gtk_shell_bind(
        struct wl_client *client,
        void *data,
        uint32_t version,
        uint32_t id)
{
    struct wayland_server *srv = data;

    /*
     * gtk-shell protocol saat ini dibatasi sampai version 3.
     */
    if (version > 3)
        version = 3;

    struct wl_resource *resource =
        wl_resource_create(
            client,
            &gtk_shell1_interface,
            version,
            id);

    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(
        resource,
        &gtk_shell_impl,
        srv,
        NULL);

    LOGI(
        "bind gtk_shell1 "
        "version=%u",
        version);
}
