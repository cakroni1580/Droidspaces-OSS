#include "server_internal.h"
#include "gtk-shell-server-protocol.h"

#include <android/log.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "TrierarchGtkShell"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define COMPOSITOR_RESIZE_NONE   0u
#define COMPOSITOR_RESIZE_TOP    (1u << 0)
#define COMPOSITOR_RESIZE_RIGHT  (1u << 1)
#define COMPOSITOR_RESIZE_BOTTOM (1u << 2)
#define COMPOSITOR_RESIZE_LEFT   (1u << 3)

static bool gtk_surface_is_xdg_toplevel(
        struct compositor_surface *surf)
{
    if (!surf)
        return false;

    /*
     * xdg_toplevel_res adalah indicator bahwa wl_surface ini
     * sudah memperoleh role xdg_toplevel.
     *
     * GTK tiling/maximize hanya boleh diterapkan pada surface ini.
     */
    return surf->xdg_surface_res != NULL &&
           surf->xdg_toplevel_res != NULL;
}

static uint32_t gtk_surface_get_tiling_state(
        struct compositor_surface *surf);

bool gtk_shell_get_work_area(
        struct compositor_surface *surf,
        struct trierarch_work_area *area);
/*
 * Keyboard focus bridge.
 *
 * GTK shell tidak memiliki authority terhadap focus.
 * Request focus diteruskan ke compositor.
 */
extern void keyboard_focus_update(
        struct wayland_server *srv,
        struct compositor_surface *surface);

/*
 * ================================================================
 * PHOC SEMANTIC: OUTPUT USABLE AREA
 * ================================================================
 *
 * Work-area bukan milik layer-shell.
 *
 * Work-area adalah state geometry milik compositor output.
 *
 * Layer-shell hanya salah satu contributor yang dapat mengurangi
 * usable area melalui positive exclusive-zone reservation.
 *
 * Consumer seperti:
 *
 *     - XDG
 *     - GTK shell
 *     - tiling
 *     - popup placement
 *
 * hanya membaca hasil final dari output.
 *
 * Penting:
 *
 *     zwlr_layer_shell_v1 bind
 *         !=
 *     work-area availability
 *
 * Output usable-area selalu tersedia, bahkan ketika tidak ada
 * client yang pernah bind zwlr_layer_shell_v1.
 */
extern bool compositor_get_work_area(
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
        
static void gtk_surface_wl_surface_destroy(
        struct wl_listener *listener,
        void *data)
{
    (void)data;

    struct gtk_surface_state *state =
        wl_container_of(listener, state,
                        surface_destroy_listener);

    /*
     * CONTEXT:
     *
     * wl_surface sudah dihancurkan.
     *
     * Sebelum reference lokal diputus, bersihkan reverse
     * attachment pada compositor_surface.
     *
     * Ini menjaga:
     *
     *     compositor_surface->gtk_surface == NULL
     *
     * setelah wl_surface mati.
     */
    if (state->surface &&
        state->surface->gtk_surface == state) {

        state->surface->gtk_surface = NULL;
    }

    /*
     * gtk_surface_state tidak lagi boleh mengakses
     * compositor_surface maupun wl_surface.
     */
    state->wl_surface = NULL;
    state->surface = NULL;

    LOGI(
        "gtk_surface wl_surface destroyed");
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

    LOGI(
        "gtk_surface.present "
        "timestamp=%u "
        "surface=%p "
        "role=%s "
        "xdg_surface=%p "
        "xdg_toplevel=%p",
        timestamp,
        (void *)state->surface,
        gtk_surface_is_xdg_toplevel(state->surface)
            ? "xdg-toplevel"
            : "non-xdg",
        state->surface->xdg_surface_res,
        state->surface->xdg_toplevel_res);
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
// AFTER
static uint32_t gtk_surface_get_tiling_state(
        struct compositor_surface *surf)
{
    if (!surf)
        return 0;

    switch (surf->tiling_state) {

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
        surf->resize_edges;

    /*
     * GTK shell protocol menggunakan satu uint32_t
     * untuk setiap edge.
     */

    if (resize_edges & COMPOSITOR_RESIZE_TOP) {

        uint32_t *edge =
            wl_array_add(edges, sizeof(*edge));

        if (edge)
            *edge = GTK_SURFACE1_EDGE_CONSTRAINT_RESIZABLE_TOP;
    }

    if (resize_edges & COMPOSITOR_RESIZE_RIGHT) {

        uint32_t *edge =
            wl_array_add(edges, sizeof(*edge));

        if (edge)
            *edge = GTK_SURFACE1_EDGE_CONSTRAINT_RESIZABLE_RIGHT;
    }

    if (resize_edges & COMPOSITOR_RESIZE_BOTTOM) {

        uint32_t *edge =
            wl_array_add(edges, sizeof(*edge));

        if (edge)
            *edge = GTK_SURFACE1_EDGE_CONSTRAINT_RESIZABLE_BOTTOM;
    }

    if (resize_edges & COMPOSITOR_RESIZE_LEFT) {

        uint32_t *edge =
            wl_array_add(edges, sizeof(*edge));

        if (edge)
            *edge = GTK_SURFACE1_EDGE_CONSTRAINT_RESIZABLE_LEFT;
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
};



/*
 * ================================================================
 * GTK WORK-AREA CONSUMER
 * ================================================================
 *
 * PHOC SEMANTIC:
 *
 * GTK tidak mengetahui siapa yang menghasilkan usable-area.
 *
 * GTK hanya meminta current output work-area.
 *
 * Provider:
 *
 *     compositor output
 *
 * Contributors:
 *
 *     layer-shell exclusive zones
 *     output policy
 *     future compositor reservations
 *
 * BUKAN:
 *
 *     gtk-shell
 *     xdg-shell
 *     zwlr_layer_shell_v1 bind state
 *
 * Dengan semantic ini:
 *
 *     no layer-shell bind
 *         -> full physical output
 *
 *     layer-shell exists but no exclusive zone
 *         -> full physical output
 *
 *     layer-shell exclusive zone exists
 *         -> reduced usable area
 *
 *     layer-shell surface destroyed
 *         -> reservation disappears
 *         -> usable area recomputed
 */
bool gtk_shell_get_work_area(
        struct compositor_surface *surf,
        struct trierarch_work_area *area)
{
    if (!surf ||
        !surf->srv ||
        !area)
        return false;

    if (area->width == 0 ||
        area->height == 0) {

        LOGE(
            "gtk work-area invalid "
            "surface=%p "
            "output=%ux%u "
            "area=%ux%u+%d+%d",
            (void *)surf,
            surf->srv->output_width,
            surf->srv->output_height,
            area->width,
            area->height,
            area->x,
            area->y);

        return false;
    }

    LOGI(
        "gtk work-area "
        "source=compositor-output "
        "surface=%p "
        "area=%ux%u+%d+%d",
        (void *)surf,
        area->width,
        area->height,
        area->x,
        area->y);

    return true;
}

void send_gtk_surface_configure(
        struct compositor_surface *surf)
{
    if (!surf ||
        !surf->srv ||
        !surf->gtk_surface ||
        !surf->gtk_surface->resource)
        return;

/*
 * ============================================================
 * GEOMETRY CONTRACT
 * ============================================================
 *
 * send_gtk_surface_configure() BUKAN geometry authority.
 *
 * Urutan authority:
 *
 *     output.c
 *       |
 *       +--> PHYSICAL DISPLAY
 *                |
 *                v
 *       send_toplevel_configure()
 *                |
 *                v
 *       COMPOSITOR TILING
 *                |
 *                +--> wm_x
 *                +--> wm_y
 *                +--> width
 *                +--> height
 *
 * gtk-shell hanya mengonsumsi state hasil compositor.
 *
 * Work-area:
 *
 *     gtk_shell_get_work_area()
 *
 * adalah FINAL WORK AREA untuk consumer GTK.
 *
 * Work-area tersebut:
 *
 *     layer-shell final area
 *          atau
 *     physical display fallback
 *
 * Work-area TIDAK dipakai untuk menghitung tiling.
 * ============================================================
 */

    /*
 * ============================================================
 * CONTEXT:
 *
 * GTK shell hanya consumer dari dua state compositor:
 *
 *   1. tiling state
 *   2. final work-area
 *
 * Tiling geometry TIDAK dihitung di sini.
 *
 * Work-area authority:
 *
 *     layer_shell_get_work_area()
 *
 * Geometry authority:
 *
 *     compositor / output / xdg-shell
 * ============================================================
 */

    struct trierarch_work_area work_area;

    if (!gtk_shell_get_work_area(
            surf,
            &work_area)) {

        LOGE(
            "gtk configure skipped "
            "reason=work-area-unavailable "
            "surface=%p",
            (void *)surf);

        return;
    }

    uint32_t state =
        gtk_surface_get_tiling_state(surf);

    /*
     * CONTEXT:
     * 
     * gtk_surface1.configure() hanya menerima:
     *
     *     resource
     *     states
     *
     * Jadi state tiling harus dimasukkan ke wl_array states.
     */
    struct wl_array states;

    wl_array_init(&states);

    if (state != 0) {

        uint32_t *gtk_state =
            wl_array_add(
                &states,
                sizeof(*gtk_state));

        if (gtk_state)
            *gtk_state = state;
    }

/*
 * CONTEXT:
 *
 * Geometry tetap compositor-side.
 *
 * GTK shell tidak mengirim:
 *
 *     width
 *     height
 *     x
 *     y
 *
 * melalui gtk_surface1.configure().
 *
 * Geometry dapat dicatat untuk memastikan GTK shell
 * membaca state compositor yang benar.
 */
     int32_t logical_w = 0;
     int32_t logical_h = 0;

     compositor_surface_get_logical_size(
         surf,
         &logical_w,
         &logical_h);

     LOGI(
         "GTK configure "
         "surf=%p "
         "tiling_state=%u "
         "logical=%dx%d "
         "wm=%d,%d "
         "workarea=%ux%u+%d+%d",
         (void *)surf,
         state,
         logical_w,
         logical_h,
         surf->wm_x,
         surf->wm_y,
         work_area.width,
         work_area.height,
         work_area.x,
         work_area.y);

     /*
      * CONTEXT:
      *
      * Serial GTK configure.
      *
      * Protocol generated GTK shell yang kamu punya
      * hanya membutuhkan resource + states.
      */
     gtk_surface1_send_configure(
         surf->gtk_surface->resource,
         &states);

     wl_array_release(&states);
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
