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
    return surf->xdg_surface_res != NULL &&
           surf->xdg_toplevel_res != NULL;
}

static uint32_t gtk_surface_get_tiling_state(
        struct compositor_surface *surf);
bool gtk_shell_get_work_area(
        struct compositor_surface *surf,
        struct trierarch_work_area *area);
extern void keyboard_focus_update(
        struct wayland_server *srv,
        struct compositor_surface *surface);

/*
 * Layer-shell adalah authority untuk usable output area.
 * Hasil dari layer_shell_get_work_area() adalah final work-area
 * setelah seluruh layer-shell exclusive zone diterapkan.
 */
extern void layer_shell_get_work_area(
        struct wayland_server *srv,
        struct compositor_surface *exclude,
        struct trierarch_work_area *area);
struct gtk_surface_state {
    struct wl_resource *resource;
    struct wl_resource *wl_surface;
    struct compositor_surface *surface;
    struct wl_listener surface_destroy_listener;
};
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
    state->wl_surface = NULL;
    state->surface = NULL;
    LOGI("gtk_surface wl_surface destroyed");
}
static void gtk_surface_resource_destroy(
        struct wl_resource *resource)
{
    struct gtk_surface_state *state =
        wl_resource_get_user_data(resource);
    if (!state)
        return;
    if (state->surface &&
        state->surface->gtk_surface == state) {
        state->surface->gtk_surface = NULL;
    }

    if (state->surface_destroy_listener.link.prev &&
        state->surface_destroy_listener.link.next) {
        wl_list_remove(
            &state->surface_destroy_listener.link);
        wl_list_init(
            &state->surface_destroy_listener.link);
    }
    free(state);
}
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
static void gtk_surface_set_modal(
        struct wl_client *client,
        struct wl_resource *resource)
{
    (void)client;
    (void)resource;
}

static void gtk_surface_unset_modal(
        struct wl_client *client,
        struct wl_resource *resource)
{
    (void)client;
    (void)resource;
}
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
    LOGI(
        "gtk_surface.request_focus "
        "surface=%p app_id=%s title=%s",
        (void *)surf,
        surf->app_id,
        surf->title);
     if (!gtk_surface_is_xdg_toplevel(surf)) {
         LOGI(
             "gtk focus skipped "
             "surface=%p reason=not-xdg-toplevel",
             (void *)surf);
         return;
     }
     keyboard_focus_update(
        surf->srv,
        surf);
}
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
    if (!surf || !srv || surf->srv != srv) {
        LOGE(
            "gtk_shell.get_gtk_surface: invalid wl_surface "
            "surface=%p compositor_surface=%p srv=%p",
            (void *)surface,
            (void *)surf,
            (void *)srv);
        return;
    }
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
    state->resource = gtk_surface;
    state->wl_surface = surface;
    state->surface = surf;
    state->surface_destroy_listener.notify =
        gtk_surface_wl_surface_destroy;
    wl_resource_add_destroy_listener(
        surface,
        &state->surface_destroy_listener);
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
static void gtk_surface_build_edge_constraints(
        struct compositor_surface *surf,
        struct wl_array *edges)
{
    wl_array_init(edges);
    if (!surf)
        return;
    uint32_t resize_edges =
        compositor_surface_get_resize_edges(surf);
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
static void gtk_shell_set_startup_id(
        struct wl_client *client,
        struct wl_resource *resource,
        const char *startup_id)
{
    (void)client;
    (void)resource;
    (void)startup_id;
}
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
 * CONTEXT NOTE:
 *
 * WM_MODE_DIRECT geometry authority:
 *
 *     layer-shell
 *          ↓
 *     exclusive zones
 *          ↓
 *     work-area
 *          ↓
 *     XDG shell / output.c
 *          ↓
 *     compositor_surface geometry
 *
 * gtk-shell.c BUKAN geometry authority.
 *
 * GTK shell hanya boleh:
 *
 *     - expose GTK state
 *     - expose resize constraints
 *     - consume XDG state
 *
 * GTK tidak boleh menghitung atau menulis:
 *
 *     surf->wm_x
 *     surf->wm_y
 *     width
 *     height
 *
 * Geometry final harus sudah ditentukan oleh XDG/output.c.
 */
bool gtk_shell_apply_tiling_geometry(
        struct compositor_surface *surf,
        uint32_t *width,
        uint32_t *height)
{
    if (!surf || !width || !height)
        return false;

    /*
     * GTK shell hanya berlaku untuk XDG toplevel.
     *
     * Layer-shell tetap sepenuhnya berada di bawah
     * layer_surface_get_geometry().
     */
    if (!gtk_surface_is_xdg_toplevel(surf)) {

        LOGI(
            "gtk geometry skipped "
            "surface=%p reason=not-xdg-toplevel",
            (void *)surf);

        return false;
    }

    /*
     * ============================================================
     * SOURCE OF TRUTH
     * ============================================================
     *
     * WM_MODE_DIRECT sudah mempunyai geometry final pada
     * compositor_surface.
     *
     * GTK tidak menghitung ulang geometry tersebut.
     *
     * Karena itu helper ini hanya mengekspos geometry yang
     * sudah diputuskan oleh XDG/output.c.
     */
    if (*width == 0 || *height == 0) {

        LOGI(
            "gtk geometry unavailable "
            "surface=%p geometry=%ux%u",
            (void *)surf,
            *width,
            *height);

        return false;
    }

    LOGI(
        "gtk geometry follows XDG "
        "surface=%p "
        "geometry=%ux%u+%d+%d "
        "tiling=%d",
        (void *)surf,
        *width,
        *height,
        surf->wm_x,
        surf->wm_y,
        compositor_surface_get_tiling(surf));

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


bool gtk_shell_get_maximized_geometry(
        struct compositor_surface *surf,
        int32_t *x,
        int32_t *y,
        uint32_t *width,
        uint32_t *height)
{
    if (!surf ||
        !x ||
        !y ||
        !width ||
        !height)
        return false;

    /*
     * ============================================================
     * CONTEXT:
     *
     * GTK maximized geometry mengikuti WM_MODE_DIRECT.
     *
     * Geometry authority:
     *
     *     layer-shell
     *          ↓
     *     exclusive zones
     *          ↓
     *       work-area
     *          ↓
     *     XDG maximized
     *
     * Jangan mengambil logical size dari compositor_surface di
     * sini. Work-area sudah menjadi geometry authority dan sudah
     * memakai logical-size pada jalur pembentukannya.
     *
     * gtk-shell hanya membaca hasil work-area.
     * ============================================================
     */
    if (!gtk_surface_is_xdg_toplevel(surf))
        return false;

    struct trierarch_work_area area;

    /*
     * ============================================================
     * WORK-AREA
     * ============================================================
     *
     * XDG toplevel tidak di-exclude dari reservation layer-shell.
     *
     * Karena surf adalah XDG:
     *
     *     exclude = NULL
     *
     * sehingga seluruh exclusive layer-shell reservation
     * diperhitungkan.
     */
    if (!gtk_shell_get_work_area(
            surf,
            &area)) {

        LOGI(
            "gtk maximized geometry unavailable "
            "surface=%p reason=invalid-work-area",
            (void *)surf);

        return false;
    }

    /*
     * ============================================================
     * MAXIMIZED GEOMETRY
     * ============================================================
     *
     * Jangan membaca:
     *
     *     surf->wm_x
     *     surf->wm_y
     *     compositor_surface_get_logical_size()
     *
     * untuk menentukan maximize.
     *
     * Work-area adalah hasil final:
     *
     *     x      = usable work-area X
     *     y      = usable work-area Y
     *     width  = usable work-area width
     *     height = usable work-area height
     * ============================================================
     */
    *x = area.x;
    *y = area.y;
    *width = area.width;
    *height = area.height;

    LOGI(
        "gtk maximized follows work-area "
        "surface=%p geometry=%ux%u+%d+%d",
        (void *)surf,
        *width,
        *height,
        *x,
        *y);

    return true;
}

void gtk_shell_bind(
        struct wl_client *client,
        void *data,
        uint32_t version,
        uint32_t id)
{
    struct wayland_server *srv = data;
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
