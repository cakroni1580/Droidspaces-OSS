/*
 * wlr-layer-shell (zwlr_layer_shell_v1)
 *
 * Stage 1
 * --------
 * - global bind
 * - layer_shell resource
 * - layer_surface resource
 * - request skeleton
 *
 * Belum:
 *   - configure
 *   - layout
 *   - exclusive zone
 *   - keyboard focus
 *   - popup
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

struct layer_surface_state {

    uint32_t layer;

    uint32_t anchor;

    uint32_t exclusive_zone;

    uint32_t keyboard_interactive;

    int32_t desired_width;
    int32_t desired_height;

    int32_t margin_top;
    int32_t margin_right;
    int32_t margin_bottom;
    int32_t margin_left;

    char name_space[128];
};

static void layer_surface_resource_destroy(
        struct wl_resource *resource)
{
    struct layer_surface_state *state =
            wl_resource_get_user_data(resource);

    free(state);
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

    struct layer_surface_state *state =
            wl_resource_get_user_data(resource);

    if (!state)
        return;

    state->desired_width = width;
    state->desired_height = height;
}

static void layer_surface_set_anchor(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t anchor)
{
    (void)client;

    struct layer_surface_state *state =
            wl_resource_get_user_data(resource);

    if (state)
        state->anchor = anchor;
}

static void layer_surface_set_exclusive_zone(
        struct wl_client *client,
        struct wl_resource *resource,
        int32_t zone)
{
    (void)client;

    struct layer_surface_state *state =
            wl_resource_get_user_data(resource);

    if (state)
        state->exclusive_zone = zone;
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

    struct layer_surface_state *state =
            wl_resource_get_user_data(resource);

    if (!state)
        return;

    state->margin_top = top;
    state->margin_right = right;
    state->margin_bottom = bottom;
    state->margin_left = left;
}

static void layer_surface_set_keyboard_interactivity(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t mode)
{
    (void)client;

    struct layer_surface_state *state =
            wl_resource_get_user_data(resource);

    if (state)
        state->keyboard_interactive = mode;
}

static void layer_surface_get_popup(
        struct wl_client *client,
        struct wl_resource *resource,
        struct wl_resource *popup)
{
    (void)client;
    (void)resource;
    (void)popup;
}

static void layer_surface_ack_configure(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t serial)
{
    (void)client;
    (void)resource;
    (void)serial;
}

static void layer_surface_set_layer(
        struct wl_client *client,
        struct wl_resource *resource,
        uint32_t layer)
{
    (void)client;

    struct layer_surface_state *state =
            wl_resource_get_user_data(resource);

    if (state)
        state->layer = layer;
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
        struct wl_resource *surface,
        struct wl_resource *output,
        uint32_t layer,
        const char *namespace)
{
    (void)resource;
    (void)output;

    struct layer_surface_state *state =
            calloc(1, sizeof(*state));

    if (!state) {
        wl_client_post_no_memory(client);
        return;
    }

    state->layer = layer;

    if (namespace)
        strncpy(state->name_space,
                namespace,
                sizeof(state->name_space) - 1);

    struct wl_resource *layer_res =
            wl_resource_create(
                    client,
                    &zwlr_layer_surface_v1_interface,
                    1,
                    id);

    if (!layer_res) {
        free(state);
        wl_client_post_no_memory(client);
        return;
    }

    wl_resource_set_implementation(
            layer_res,
            &layer_surface_impl,
            state,
            layer_surface_resource_destroy);

    LOGI("new layer_surface=%p surface=%p layer=%u namespace=%s",
            layer_res,
            surface,
            layer,
            state->name_space);
}

static const struct zwlr_layer_shell_v1_interface
layer_shell_impl = {

    .destroy = layer_shell_destroy,

    .get_layer_surface =
        layer_shell_get_layer_surface,
};

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
