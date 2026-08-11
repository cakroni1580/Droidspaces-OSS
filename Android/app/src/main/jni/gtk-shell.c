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
 * ================================================================
 * CONTEXT: GTK DEFAULT SURFACE AUTHORITY
 * ================================================================
 *
 * GTK shell boleh attached ke wl_surface yang juga merupakan
 * layer-shell surface, tetapi GTK window-management policy hanya
 * berlaku pada XDG toplevel.
 *
 * Default desktop application:
 *
 *     wl_surface
 *          |
 *          +--> xdg_surface
 *                  |
 *                  +--> xdg_toplevel   <-- GTK geometry authority
 *
 * Layer-shell tetap valid:
 *
 *     wl_surface
 *          |
 *          +--> zwlr_layer_surface_v1
 *
 * tetapi GTK TIDAK mengambil alih geometry layer-shell.
 *
 * ================================================================
 */
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
 * Layer-shell adalah authority untuk usable output area.
 *
 * GTK shell TIDAK membaca:
 *
 *     layer_surface->exclusive_zone
 *
 * secara langsung.
 *
 * Hasil dari layer_shell_get_work_area() adalah final work-area
 * setelah seluruh layer-shell exclusive zone diterapkan.
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
/*
 * CONTEXT:
 *
 * GTK shell BUKAN geometry authority.
 *
 * Layer-shell sudah menentukan:
 *
 *     output geometry
 *          +
 *     exclusive-zone reservation
 *          =
 *     final work-area
 *
 * GTK hanya mengonsumsi work-area tersebut untuk policy
 * tiling XDG.
 *
 * Dengan demikian:
 *
 *     layer surface geometry
 *             !=
 *     GTK tiling geometry
 *
 * dan GTK tidak pernah mengubah geometry layer surface.
 *
 * Untuk COMPOSITOR_TILING_NONE:
 *
 *     GTK tidak boleh menyentuh geometry existing.
 *
 * Ini penting agar WM_MODE_DIRECT dan WM_MODE_NESTED
 * tetap memiliki geometry contract yang sama.
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
     * ============================================================
     * CONTEXT:
     * GTK WM geometry hanya berlaku untuk XDG toplevel.
     *
     * Jangan pernah menggunakan gtk tiling policy untuk:
     *
     *     zwlr_layer_surface_v1
     *
     * Layer-shell mempunyai geometry authority sendiri.
     *
     * Dengan check ini:
     *
     *     XDG toplevel  -> GTK tiling boleh diterapkan
     *     layer-shell   -> geometry dibiarkan apa adanya
     * ============================================================
     */
    if (!gtk_surface_is_xdg_toplevel(surf)) {
        LOGI(
            "gtk tiling skipped "
            "surface=%p reason=not-xdg-toplevel "
            "xdg_surface=%p xdg_toplevel=%p",
            (void *)surf,
            (void *)surf->xdg_surface_res,
            (void *)surf->xdg_toplevel_res);

        return false;
    }

    enum compositor_tiling_state tiling =
        compositor_surface_get_tiling(surf);

    /*
     * ============================================================
     * CONTEXT:
     *
     * Work-area adalah satu-satunya input geometry dari
     * layer-shell untuk GTK.
     *
     * GTK tidak menghitung exclusive-zone sendiri.
     * ============================================================
     */
    struct trierarch_work_area area;

    if (!gtk_shell_get_work_area(
            surf,
            &area))
        return false;

    /*
     * ============================================================
     * CONTEXT:
     *
     * NONE berarti GTK tidak memiliki tiling policy.
     *
     * Jangan mengganti geometry surface menjadi work-area.
     *
     * Geometry existing tetap menjadi authority.
     *
     * Caller harus mempertahankan width/height/x/y yang
     * sudah dimiliki surface.
     * ============================================================
     */
    if (tiling == COMPOSITOR_TILING_NONE)
        return false;

    /*
     * Default:
     *
     * seluruh usable work-area.
     */
    *x = area.x;
    *y = area.y;
    *width = area.width;
    *height = area.height;

    switch (tiling) {

    case COMPOSITOR_TILING_ALL:
        /*
         * GTK maximize/full tiling:
         *
         * gunakan seluruh work-area.
         */
        break;

    case COMPOSITOR_TILING_LEFT:

        *width = area.width / 2;

        break;

    case COMPOSITOR_TILING_RIGHT:

        *width = area.width -
                 (area.width / 2);

        *x = area.x +
             (int32_t)(area.width / 2);

        break;

    case COMPOSITOR_TILING_TOP:

        *height = area.height / 2;

        break;

    case COMPOSITOR_TILING_BOTTOM:

        *height = area.height -
                  (area.height / 2);

        *y = area.y +
             (int32_t)(area.height / 2);

        break;

    case COMPOSITOR_TILING_NONE:
    default:
        return false;
    }

    if (*width == 0 ||
        *height == 0)
        return false;

    LOGI(
        "gtk tiling consumer "
        "surface=%p "
        "tiling=%d "
        "layer-workarea=%ux%u+%d+%d "
        "xdg-geometry=%ux%u+%d+%d",
        (void *)surf,
        tiling,
        area.width,
        area.height,
        area.x,
        area.y,
        *width,
        *height,
        *x,
        *y);

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

bool gtk_shell_apply_tiling_geometry(
        struct compositor_surface *surf,
        uint32_t *width,
        uint32_t *height)
{
    if (!surf ||
        !surf->srv ||
        !width ||
        !height)
        return false;

    /*
     * ============================================================
     * CONTEXT:
     *
     * gtk_shell_apply_tiling_geometry() adalah geometry policy
     * untuk XDG desktop windows.
     *
     * Layer-shell TIDAK BOLEH masuk ke sini.
     *
     * Jadi walaupun layer-shell surface mempunyai:
     *
     *     surf->gtk_surface
     *
     * GTK tetap tidak boleh mengubah wm_x/wm_y miliknya.
     * ============================================================
     */
    if (!gtk_surface_is_xdg_toplevel(surf)) {
        LOGI(
            "gtk apply tiling skipped "
            "surface=%p reason=not-xdg-toplevel",
            (void *)surf);

        return false;
    }

    int32_t x = 0;
    int32_t y = 0;

    /*
     * CONTEXT:
     *
     * Hanya tiling policy GTK yang boleh masuk melalui helper ini.
     *
     * Jika tidak ada tiling:
     *
     *     GTK -> tidak melakukan apa-apa
     *
     * sehingga geometry yang berasal dari output.c / xdg-shell
     * tetap utuh.
     */
    if (compositor_surface_get_tiling(surf) ==
        COMPOSITOR_TILING_NONE) {

        LOGI(
            "gtk tiling skipped "
            "surface=%p reason=no-tiling-policy",
            (void *)surf);

        return false;
    }

    if (!gtk_shell_get_tiling_geometry(
            surf,
            &x,
            &y,
            width,
            height))
        return false;

    /*
     * CONTEXT:
     *
     * Hasil di sini adalah geometry XDG FINAL untuk WM_MODE_DIRECT.
     *
     * Ini TIDAK pernah ditulis ke layer-shell surface.
     *
     * Layer-shell tetap menggunakan geometry hasil
     * layer_surface_get_geometry().
     */
    surf->wm_x = x;
    surf->wm_y = y;

    LOGI(
        "gtk applied XDG geometry "
        "surface=%p "
        "geometry=%ux%u+%d+%d",
        (void *)surf,
        *width,
        *height,
        surf->wm_x,
        surf->wm_y);

    return true;
}

void compositor_surface_set_tiling(
        struct compositor_surface *surf,
        enum compositor_tiling_state state)
{
    if (!surf)
        return;

    surf->tiling_state = state;

    LOGI(
        "set tiling "
        "surface=%p "
        "state=%d",
        (void *)surf,
        state);

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

/*
 * ================================================================
 * GTK SHELL <-> LAYER-SHELL WORK-AREA
 * ================================================================
 *
 * CONTEXT:
 *
 * Work-area adalah geometry dasar untuk window-management
 * WM_MODE_DIRECT.
 *
 * Source of truth:
 *
 *     layer-shell
 *          ↓
 *     exclusive zones
 *          ↓
 *     usable work-area
 *
 * GTK shell dan XDG shell menggunakan work-area yang sama.
 *
 * Penting:
 *
 *     gtk-shell protocol TIDAK harus pernah dipanggil client.
 *
 * Helper ini adalah compositor-side geometry API.
 *
 * Jika:
 *
 *     - tidak ada GTK client
 *     - tidak ada layer-shell surface
 *
 * maka layer_shell_get_work_area() tetap mengembalikan
 * output area normal.
 * ================================================================
 */
bool gtk_shell_get_work_area(
        struct compositor_surface *surf,
        struct trierarch_work_area *area)
{
    if (!surf ||
        !surf->srv ||
        !area)
        return false;

    /*
     * ============================================================
     * CONTEXT:
     *
     * layer-shell tetap menjadi authority exclusive-zone.
     *
     * Namun surface XDG bukan layer surface, sehingga tidak boleh
     * diperlakukan sebagai layer-shell object yang perlu di-exclude.
     *
     * Untuk XDG:
     *
     *     exclude = NULL
     *
     * Untuk layer-shell:
     *
     *     exclude = surf
     *
     * Dengan demikian GTK/XDG membaca work-area yang sama tanpa
     * mengambil alih geometry layer-shell.
     * ============================================================
     */
    struct compositor_surface *exclude = NULL;

    if (!gtk_surface_is_xdg_toplevel(surf))
        exclude = surf;

    layer_shell_get_work_area(
        surf->srv,
        exclude,
        area);

    if (area->width == 0 ||
        area->height == 0) {

        LOGE(
            "gtk invalid layer-shell work-area "
            "surface=%p area=%ux%u+%d+%d",
            (void *)surf,
            area->width,
            area->height,
            area->x,
            area->y);

        return false;
    }

    return true;
}


/*
 * ================================================================
 * GTK MAXIMIZED GEOMETRY
 * ================================================================
 *
 * CONTEXT:
 *
 * WM_MODE_DIRECT maximize harus memakai geometry yang sama
 * dengan GTK tiling.
 *
 * Flow:
 *
 *     layer-shell exclusive zone
 *              ↓
 *          work-area
 *              ↓
 *       GTK geometry policy
 *              ↓
 *        XDG maximize
 *
 * GTK-shell protocol tidak perlu dibind oleh client.
 *
 * Jika tidak ada layer-shell reservation:
 *
 *     work-area == output
 *
 * sehingga maximize tetap normal.
 * ================================================================
 */
bool gtk_shell_get_maximized_geometry(
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
     * ============================================================
     * CONTEXT:
     *
     * MAXIMIZED adalah state window XDG.
     *
     * Layer-shell tidak boleh menerima geometry maximize dari
     * gtk-shell.
     *
     * Layer-shell tetap mengikuti:
     *
     *     layer_surface_get_geometry()
     *
     * sedangkan XDG mengikuti:
     *
     *     gtk_shell_get_maximized_geometry()
     * ============================================================
     */
    if (!gtk_surface_is_xdg_toplevel(surf)) {
        LOGI(
            "gtk maximized geometry skipped "
            "surface=%p reason=not-xdg-toplevel",
            (void *)surf);

        return false;
    }

    struct trierarch_work_area area;

    if (!gtk_shell_get_work_area(
            surf,
            &area))
        return false;

    /*
     * MAXIMIZED menggunakan seluruh usable work-area.
     *
     * Tidak membagi area seperti tiling LEFT/RIGHT/TOP/BOTTOM.
     */
    *x = area.x;
    *y = area.y;
    *width = area.width;
    *height = area.height;

    LOGI(
        "gtk maximized geometry "
        "surface=%p "
        "workarea=%ux%u+%d+%d",
        (void *)surf,
        area.width,
        area.height,
        area.x,
        area.y);

    return true;
}

/*
 * ================================================================
 * GTK SURFACE CONFIGURE
 * ================================================================
 *
 * CONTEXT:
 *
 * output.c adalah trigger geometry/state update.
 *
 * gtk-shell.c BUKAN geometry authority.
 *
 * Jalur:
 *
 *     output.c
 *          |
 *          +--> xdg-shell configure
 *          |
 *          +--> layer-shell configure
 *          |
 *          +--> GTK shell configure
 *
 * GTK configure hanya mem-publish state compositor kepada
 * gtk_surface1.
 *
 * Geometry X/Y tidak dihitung ulang di sini.
 * Geometry XDG tetap berasal dari:
 *
 *     compositor_surface
 *             +
 *     layer-shell work-area
 *             +
 *     GTK tiling policy
 *
 * Layer-shell tidak pernah dimodifikasi oleh helper ini.
 * ================================================================
 */
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
     * GTK surface tanpa XDG toplevel
     * ============================================================
     *
     * gtk_surface1 boleh attached ke wl_surface yang bukan
     * XDG toplevel.
     *
     * Tetapi state window-management seperti tiling hanya
     * berlaku pada XDG toplevel.
     *
     * Jangan mengarang GTK geometry untuk layer-shell.
     */
    if (!gtk_surface_is_xdg_toplevel(surf)) {

        LOGI(
            "gtk configure skipped "
            "surface=%p reason=not-xdg-toplevel "
            "xdg_surface=%p xdg_toplevel=%p",
            (void *)surf,
            (void *)surf->xdg_surface_res,
            (void *)surf->xdg_toplevel_res);

        return;
    }

    /*
     * ============================================================
     * TILING STATE
     * ============================================================
     *
     * State berasal dari compositor_surface.
     *
     * GTK shell hanya menerjemahkannya ke protocol state.
     */
    uint32_t state =
        gtk_surface_get_tiling_state(surf);

    /*
     * ============================================================
     * RESIZE EDGES
     * ============================================================
     *
     * Edge constraint juga berasal dari compositor state.
     *
     * GTK tidak menentukan sendiri edge yang boleh di-resize.
     */
    struct wl_array edges;

    gtk_surface_build_edge_constraints(
        surf,
        &edges);

    /*
     * ============================================================
     * CONFIGURE SERIAL
     * ============================================================
     *
     * Serial berasal dari display compositor yang sama dengan
     * xdg-shell dan layer-shell.
     */
    uint32_t serial =
        wl_display_next_serial(
            surf->srv->display);

    /*
     * ============================================================
     * GTK configure
     * ============================================================
     *
     * NOTE:
     *
     * Nama/argument event harus mengikuti generated
     * gtk-shell-server-protocol.h yang digunakan Trierarch.
     *
     * State:
     *
     *     GTK_SURFACE1_STATE_*
     *
     * Edges:
     *
     *     GTK_SURFACE1_EDGE_CONSTRAINT_*
     *
     * Geometry tidak dihitung oleh GTK shell.
     */
    gtk_surface1_send_configure(
        surf->gtk_surface->resource,
        serial,
        state,
        &edges);

    LOGI(
        "gtk surface configure "
        "surface=%p "
        "serial=%u "
        "state=0x%x "
        "edges=%zu",
        (void *)surf,
        serial,
        state,
        edges.size);

    wl_array_release(&edges);
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
