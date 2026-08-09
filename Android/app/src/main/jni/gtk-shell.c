#include "server_internal.h"
#include "gtk-shell-server-protocol.h"

#include <android/log.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "TrierarchGtkShell"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

/*
 * Keyboard focus bridge.
 *
 * GTK request_focus -> wl_keyboard focus.
 */
extern void keyboard_focus_update(
        struct wayland_server *srv,
        struct compositor_surface *surface);

/* --------------------------------------------------------- */
/* gtk_surface1                                               */
/* --------------------------------------------------------- */

/* Forward declaration.
 * Digunakan oleh gtk_shell_get_gtk_surface() sebelum object
 * gtk_surface_impl didefinisikan di bawah.
 */
static const struct gtk_surface1_interface gtk_surface_impl;
static const struct gtk_shell1_interface gtk_shell_impl;

struct gtk_surface_state {
    struct wl_resource *resource;
    struct wl_resource *wl_surface;

    /*
     * Direct compositor surface reference.
     * Tidak memakai DBus karena Trierarch berjalan
     * embedded di Android JNI.
     */
    struct compositor_surface *surface;
    struct wl_listener surface_destroy_listener;
};


/* --------------------------------------------------------- */
/* gtk_surface1 resource                                     */
/* --------------------------------------------------------- */

static void gtk_surface_wl_surface_destroy(
        struct wl_listener *listener,
        void *data)
{
    (void)data;

    struct gtk_surface_state *state =
        wl_container_of(listener, state,
                        surface_destroy_listener);

    /*
     * wl_surface mati duluan.
     */
    state->wl_surface = NULL;
    state->surface = NULL;

    LOGI("gtk_surface parent wl_surface destroyed");
}


static void gtk_surface_resource_destroy(struct wl_resource *resource)
{
    struct gtk_surface_state *state =
            wl_resource_get_user_data(resource);

    if (!state)
        return;

    /*
     * Putuskan relasi dengan compositor_surface.
     */
    if (state->surface &&
            state->surface->gtk_surface == state) {

        state->surface->gtk_surface = NULL;
    }


    /*
     * Hapus listener sebelum free state.
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
    /*
     * Trierarch embedded Android tidak menggunakan
     * metadata DBus milik GTK.
     *
     * Request diterima hanya agar client GTK tetap
     * kompatibel dengan protocol.
     */

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

    LOGI("gtk_surface.present timestamp=%u surface=%p",
            timestamp,
            state->surface);
        
    send_toplevel_configure(state->surface);  
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
    /*
     * GTK meminta window menjadi active.
     *
     * Pada compositor desktop:
     * request_focus diteruskan ke window manager.
     *
     * Pada Trierarch embedded Android:
     * langsung jadikan surface sebagai keyboard focus.
     */

    if (state->surface->srv) {

        keyboard_focus_update(
                state->surface->srv,
                state->surface
        );
    }
    /*
     * GTK bukan authority.
     *
     * Setelah focus berubah,
     * seluruh protocol disinkronkan kembali
     * melalui xdg-shell.
     */

    send_toplevel_configure(
            state->surface);
}

static void gtk_shell_get_gtk_surface(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t id,
        struct wl_resource *surface)
{
    (void)resource;

    struct wl_resource *gtk_surface =
        wl_resource_create(client,
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
     * Link GTK surface <-> compositor surface.
     *
     * Tahap ini hanya menyimpan pointer sehingga request GTK
     * dapat langsung mengakses compositor_surface tanpa lookup.
     */
    struct compositor_surface *surf =
            wl_resource_get_user_data(surface);
    
        state->resource = gtk_surface;
        state->wl_surface = surface;
        state->surface = surf;

    /*
     * Listen lifecycle wl_surface.
     */
    state->surface_destroy_listener.notify =
            gtk_surface_wl_surface_destroy;

    wl_resource_add_destroy_listener(
            surface,
            &state->surface_destroy_listener);

    if (surf)
        surf->gtk_surface = state;

    wl_resource_set_implementation(
            gtk_surface,
            &gtk_surface_impl,
            state,
            gtk_surface_resource_destroy);

    LOGI("gtk_surface1 created");
}

/*
 * Trierarch tidak menggunakan GTK startup-notification.
 * inplepemt soon if possible
 * Request diterima hanya untuk menjaga kompatibilitas protocol.
 */
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

static const struct gtk_shell1_interface gtk_shell_impl = {
    .get_gtk_surface = gtk_shell_get_gtk_surface,
    .set_startup_id = gtk_shell_set_startup_id,
    .system_bell = gtk_shell_system_bell,
    .notify_launch = gtk_shell_notify_launch,
};

static const struct gtk_surface1_interface gtk_surface_impl = {
    .set_dbus_properties = gtk_surface_set_dbus_properties,
    .set_modal           = gtk_surface_set_modal,
    .unset_modal         = gtk_surface_unset_modal,
    .present             = gtk_surface_present,
    .request_focus       = gtk_surface_request_focus,
};


/* --------------------------------------------------------- */
/* gtk_shell global bind                                      */
/* --------------------------------------------------------- */

/*
 * --------------------------------------------------------------------
 * GTK configure sender.
 *
 * Dipanggil oleh output.c agar menerima geometry yang sama
 * yang sinkron dengan xdg-shell.
 *
 * Saat ini Trierarch belum mengimplementasikan tiled-edge ataupun
 * GTK window state khusus, sehingga kedua event dikirim dengan
 * array kosong (valid menurut protocol).
 * --------------------------------------------------------------------
 */
void gtk_surface_send_configure(
        struct compositor_surface *surf)
{
    if (!surf)
        return;

    if (!surf->gtk_surface)
        return;

    struct gtk_surface_state *state = surf->gtk_surface;

    if (!state->resource)
        return;

    struct wl_array states;
    struct wl_array edges;

    /*
     * ------------------------------------------------------------
     * NOTE:
     * GTK belum memiliki window state sendiri.
     *
     * Semua state diterjemahkan dari compositor_surface
     * yang dikelola xdg-shell.
     * ------------------------------------------------------------
     */

    wl_array_init(&states);
    wl_array_init(&edges);

    uint32_t *state_id = wl_array_add(&states, sizeof(*state_id));
    if (state_id)
        *state_id = GTK_SURFACE1_STATE_TILED;

        
    gtk_surface1_send_configure(
            state->resource,
            &states);

    gtk_surface1_send_configure_edges(
            state->resource,
            &edges);

    wl_array_release(&states);
    wl_array_release(&edges);

    LOGI("gtk_surface.configure surface=%p",
            surf);
}

void gtk_shell_bind(
        struct wl_client *client,
        void *data,
        uint32_t version,
        uint32_t id)
{
    if (version > 3)
        version = 3;

    struct wl_resource *resource =
        wl_resource_create(client,
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
            data,
            NULL);

    LOGI("bind gtk_shell1 v%u", version);
}
