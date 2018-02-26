/*
 * Copyright © 2011-2014 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of the
 * copyright holders not be used in advertising or publicity
 * pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include "xwayland.h"
#include <randrstr.h>

#define DEFAULT_DPI 96
#define ALL_ROTATIONS (RR_Rotate_0   | \
                       RR_Rotate_90  | \
                       RR_Rotate_180 | \
                       RR_Rotate_270 | \
                       RR_Reflect_X  | \
                       RR_Reflect_Y)

static Rotation
wl_transform_to_xrandr(enum wl_output_transform transform)
{
    switch (transform) {
    default:
    case WL_OUTPUT_TRANSFORM_NORMAL:
        return RR_Rotate_0;
    case WL_OUTPUT_TRANSFORM_90:
        return RR_Rotate_90;
    case WL_OUTPUT_TRANSFORM_180:
        return RR_Rotate_180;
    case WL_OUTPUT_TRANSFORM_270:
        return RR_Rotate_270;
    case WL_OUTPUT_TRANSFORM_FLIPPED:
        return RR_Reflect_X | RR_Rotate_0;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        return RR_Reflect_X | RR_Rotate_90;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        return RR_Reflect_X | RR_Rotate_180;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        return RR_Reflect_X | RR_Rotate_270;
    }
}

static int
wl_subpixel_to_xrandr(int subpixel)
{
    switch (subpixel) {
    default:
    case WL_OUTPUT_SUBPIXEL_UNKNOWN:
        return SubPixelUnknown;
    case WL_OUTPUT_SUBPIXEL_NONE:
        return SubPixelNone;
    case WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB:
        return SubPixelHorizontalRGB;
    case WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR:
        return SubPixelHorizontalBGR;
    case WL_OUTPUT_SUBPIXEL_VERTICAL_RGB:
        return SubPixelVerticalRGB;
    case WL_OUTPUT_SUBPIXEL_VERTICAL_BGR:
        return SubPixelVerticalBGR;
    }
}

static void
output_handle_geometry(void *data, struct wl_output *wl_output, int x, int y,
                       int physical_width, int physical_height, int subpixel,
                       const char *make, const char *model, int transform)
{
    struct xwl_output *xwl_output = data;

    RROutputSetPhysicalSize(xwl_output->randr_output,
                            physical_width, physical_height);
    RROutputSetSubpixelOrder(xwl_output->randr_output,
                             wl_subpixel_to_xrandr(subpixel));
    xwl_output->x = x;
    xwl_output->y = y;

    xwl_output->rotation = wl_transform_to_xrandr(transform);
}

static void
output_add_new_mode(struct xwl_output *xwl_output, uint32_t flags,
                    int width, int height, int refresh)
{
    struct xwl_mode *xwl_mode;

    xwl_mode = calloc(1, sizeof *xwl_mode);
    if (!xwl_mode)
        FatalError("Failed to create new mode");

    xwl_mode->width = width;
    xwl_mode->height = height;
    xwl_mode->refresh = refresh;
    xwl_mode->randr_mode = xwayland_cvt(width, height, refresh / 1000.0, 0, 0);
    xorg_list_append(&xwl_mode->link, &xwl_output->modes_list);

    xwl_output->num_modes++;
    if (flags & WL_OUTPUT_MODE_PREFERRED)
        xwl_output->preferred_mode = xwl_output->num_modes;
}

static RRModePtr
output_get_current_rr_mode(struct xwl_output *xwl_output)
{
    struct xwl_mode *xwl_mode;

    xorg_list_for_each_entry(xwl_mode, &xwl_output->modes_list, link) {
        if (xwl_mode->width == xwl_output->width &&
            xwl_mode->height == xwl_output->height &&
            xwl_mode->refresh == xwl_output->refresh)
            return xwl_mode->randr_mode;
    }

    FatalError("Cannot find current mode [%ix%i]@%.2f",
               xwl_output->width, xwl_output->height, xwl_output->refresh / 1000.0);
}

static RRModePtr *
output_get_all_rr_modes(struct xwl_output *xwl_output)
{
    struct xwl_mode *xwl_mode;
    RRModePtr *rr_modes;
    int i = 0;

    rr_modes = xallocarray(xwl_output->num_modes, sizeof(RRModePtr));
    if (!rr_modes)
        FatalError("Failed to create the list of RR modes");

    xorg_list_for_each_entry(xwl_mode, &xwl_output->modes_list, link) {
        rr_modes[i++] = xwl_mode->randr_mode;
    }

    return rr_modes;
}

static void
output_handle_mode(void *data, struct wl_output *wl_output, uint32_t flags,
                   int width, int height, int refresh)
{
    struct xwl_output *xwl_output = data;

    /* Add available modes during the binding phase */
    if (xwl_output->binding)
        output_add_new_mode (xwl_output, flags, width, height, refresh);

    if (!(flags & WL_OUTPUT_MODE_CURRENT))
        return;

    xwl_output->width = width;
    xwl_output->height = height;
    xwl_output->refresh = refresh;
}

static inline void
output_get_new_size(struct xwl_output *xwl_output,
                    int *height, int *width)
{
    int output_width, output_height;

    if (xwl_output->rotation & (RR_Rotate_0 | RR_Rotate_180)) {
        output_width = xwl_output->width;
        output_height = xwl_output->height;
    } else {
        output_width = xwl_output->height;
        output_height = xwl_output->width;
    }

    if (*width < xwl_output->x + output_width)
        *width = xwl_output->x + output_width;

    if (*height < xwl_output->y + output_height)
        *height = xwl_output->y + output_height;
}

/* Approximate some kind of mmpd (m.m. per dot) of the screen given the outputs
 * associated with it.
 *
 * It will either calculate the mean mmpd of all the outputs, or default to
 * 96 DPI if no reasonable value could be calculated.
 */
static double
approximate_mmpd(struct xwl_screen *xwl_screen)
{
    struct xwl_output *it;
    int total_width_mm = 0;
    int total_width = 0;

    xorg_list_for_each_entry(it, &xwl_screen->output_list, link) {
        if (it->randr_output->mmWidth == 0)
            continue;

        total_width_mm += it->randr_output->mmWidth;
        total_width += it->width;
    }

    if (total_width_mm != 0)
        return (double)total_width_mm / total_width;
    else
        return 25.4 / DEFAULT_DPI;
}

static void
update_screen_size(struct xwl_output *xwl_output, int width, int height)
{
    struct xwl_screen *xwl_screen = xwl_output->xwl_screen;
    double mmpd;

    if (xwl_screen->root_clip_mode == ROOT_CLIP_FULL)
        SetRootClip(xwl_screen->screen, ROOT_CLIP_NONE);

    xwl_screen->width = width;
    xwl_screen->height = height;
    xwl_screen->screen->width = width;
    xwl_screen->screen->height = height;

    if (xwl_output->width == width && xwl_output->height == height) {
        xwl_screen->screen->mmWidth = xwl_output->randr_output->mmWidth;
        xwl_screen->screen->mmHeight = xwl_output->randr_output->mmHeight;
    } else {
        mmpd = approximate_mmpd(xwl_screen);
        xwl_screen->screen->mmWidth = width * mmpd;
        xwl_screen->screen->mmHeight = height * mmpd;
    }

    SetRootClip(xwl_screen->screen, xwl_screen->root_clip_mode);

    if (xwl_screen->screen->root) {
        BoxRec box = { 0, 0, width, height };

        xwl_screen->screen->root->drawable.width = width;
        xwl_screen->screen->root->drawable.height = height;
        RegionReset(&xwl_screen->screen->root->winSize, &box);
        RRScreenSizeNotify(xwl_screen->screen);
    }

    update_desktop_dimensions();
}

static void
output_handle_done(void *data, struct wl_output *wl_output)
{
    struct xwl_output *it, *xwl_output = data;
    struct xwl_screen *xwl_screen = xwl_output->xwl_screen;
    int width = 0, height = 0, has_this_output = 0;
    RRModePtr randr_current_mode;

    if (xwl_output->binding) {
        RRModePtr *randr_modes;

        randr_modes = output_get_all_rr_modes(xwl_output);
        RROutputSetModes(xwl_output->randr_output, randr_modes,
                         xwl_output->num_modes, xwl_output->preferred_mode);
        free(randr_modes);
        /* We're done with binding. do not expect further modes to list */
        xwl_output->binding = FALSE;
    }

    randr_current_mode = output_get_current_rr_mode(xwl_output);
    RRCrtcNotify(xwl_output->randr_crtc, randr_current_mode,
                 xwl_output->x, xwl_output->y,
                 xwl_output->rotation, NULL, 1, &xwl_output->randr_output);

    xorg_list_for_each_entry(it, &xwl_screen->output_list, link) {
        /* output done event is sent even when some property
         * of output is changed. That means that we may already
         * have this output. If it is true, we must not add it
         * into the output_list otherwise we'll corrupt it */
        if (it == xwl_output)
            has_this_output = 1;

        output_get_new_size(it, &height, &width);
    }

    if (!has_this_output) {
        xorg_list_append(&xwl_output->link, &xwl_screen->output_list);

        /* we did not check this output for new screen size, do it now */
        output_get_new_size(xwl_output, &height, &width);

	--xwl_screen->expecting_event;
    }

    update_screen_size(xwl_output, width, height);
}

static void
output_handle_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
}

static const struct wl_output_listener output_listener = {
    output_handle_geometry,
    output_handle_mode,
    output_handle_done,
    output_handle_scale
};

struct xwl_output *
xwl_output_create(struct xwl_screen *xwl_screen, uint32_t id)
{
    struct xwl_output *xwl_output;
    static int serial;
    char name[256];

    xwl_output = calloc(sizeof *xwl_output, 1);
    if (xwl_output == NULL) {
        ErrorF("create_output ENOMEM\n");
        return NULL;
    }

    xwl_output->output = wl_registry_bind(xwl_screen->registry, id,
                                          &wl_output_interface, 2);
    if (!xwl_output->output) {
        ErrorF("Failed binding wl_output\n");
        goto err;
    }

    xwl_output->binding = TRUE;
    xwl_output->server_output_id = id;
    wl_output_add_listener(xwl_output->output, &output_listener, xwl_output);

    snprintf(name, sizeof name, "XWAYLAND%d", serial++);

    xwl_output->xwl_screen = xwl_screen;
    xwl_output->randr_crtc = RRCrtcCreate(xwl_screen->screen, xwl_output);
    if (!xwl_output->randr_crtc) {
        ErrorF("Failed creating RandR CRTC\n");
        goto err;
    }
    RRCrtcSetRotations (xwl_output->randr_crtc, ALL_ROTATIONS);

    xwl_output->randr_output = RROutputCreate(xwl_screen->screen, name,
                                              strlen(name), xwl_output);
    if (!xwl_output->randr_output) {
        ErrorF("Failed creating RandR Output\n");
        goto err;
    }
    xorg_list_init(&xwl_output->modes_list);
    RRCrtcGammaSetSize(xwl_output->randr_crtc, 256);
    RROutputSetCrtcs(xwl_output->randr_output, &xwl_output->randr_crtc, 1);
    RROutputSetConnection(xwl_output->randr_output, RR_Connected);

    return xwl_output;

err:
    if (xwl_output->randr_crtc)
        RRCrtcDestroy(xwl_output->randr_crtc);
    if (xwl_output->output)
        wl_output_destroy(xwl_output->output);
    free(xwl_output);
    return NULL;
}

void
xwl_output_destroy(struct xwl_output *xwl_output)
{
    wl_output_destroy(xwl_output->output);
    free(xwl_output);
}

void
xwl_output_remove(struct xwl_output *xwl_output)
{
    struct xwl_output *it;
    struct xwl_screen *xwl_screen = xwl_output->xwl_screen;
    struct xwl_mode *xwl_mode, *next_xwl_mode;
    int width = 0, height = 0;

    xorg_list_for_each_entry_safe(xwl_mode, next_xwl_mode, &xwl_output->modes_list, link) {
        RRModeDestroy(xwl_mode->randr_mode);
        free(xwl_mode);
    }

    RRCrtcDestroy(xwl_output->randr_crtc);
    RROutputDestroy(xwl_output->randr_output);
    xorg_list_del(&xwl_output->link);

    xorg_list_for_each_entry(it, &xwl_screen->output_list, link)
        output_get_new_size(it, &height, &width);
    update_screen_size(xwl_output, width, height);

    xwl_output_destroy(xwl_output);
}

static Bool
xwl_randr_get_info(ScreenPtr pScreen, Rotation * rotations)
{
    *rotations = ALL_ROTATIONS;

    return TRUE;
}

#ifdef RANDR_10_INTERFACE
static Bool
xwl_randr_set_config(ScreenPtr pScreen,
                     Rotation rotation, int rate, RRScreenSizePtr pSize)
{
    return FALSE;
}
#endif

#if RANDR_12_INTERFACE
static Bool
xwl_randr_screen_set_size(ScreenPtr pScreen,
                          CARD16 width,
                          CARD16 height,
                          CARD32 mmWidth, CARD32 mmHeight)
{
    return TRUE;
}

static Bool
xwl_randr_crtc_set(ScreenPtr pScreen,
                   RRCrtcPtr crtc,
                   RRModePtr mode,
                   int x,
                   int y,
                   Rotation rotation,
                   int numOutputs, RROutputPtr * outputs)
{
    struct xwl_screen *xwl_screen = xwl_screen_get(pScreen);
    RRCrtcChanged(crtc, TRUE);

    if(mode->mode.width == 1366 && mode->mode.height == 768)
    {
        xwl_screen->current_emulated_width = 0;
        xwl_screen->current_emulated_height = 0;
    }
    else
    {
        xwl_screen->current_emulated_width = mode->mode.width;
        xwl_screen->current_emulated_height = mode->mode.height;
    }
    ErrorF("XWAYLAND: xwl_randr_crtc_set: %ux%u\n", mode->mode.width, mode->mode.height);

    struct xwl_seat *xwl_seat = xwl_screen_get_default_seat(xwl_screen_get(pScreen));
    if(xwl_seat && xwl_seat->focus_window)
    {
        BoxPtr bptr = RegionRects(&xwl_seat->focus_window->window->winSize);
        unsigned int width  = bptr->x2 - xwl_seat->focus_window->window->drawable.x;
        unsigned int height = bptr->y2 - xwl_seat->focus_window->window->drawable.y;
        xwl_check_fake_mode_setting(xwl_seat->focus_window, width, height);
    }
    return TRUE;
}

static Bool
xwl_randr_crtc_set_gamma(ScreenPtr pScreen, RRCrtcPtr crtc)
{
    return TRUE;
}

static Bool
xwl_randr_Crtc_get_gamma(ScreenPtr pScreen, RRCrtcPtr crtc)
{
    return TRUE;
}

static Bool
xwl_randr_output_set_property(ScreenPtr pScreen,
                              RROutputPtr output,
                              Atom property,
                              RRPropertyValuePtr value)
{
    return TRUE;
}

static Bool
xwl_output_validate_mode(ScreenPtr pScreen,
                         RROutputPtr output,
                         RRModePtr mode)
{
    return TRUE;
}

static void
xwl_randr_mode_destroy(ScreenPtr pScreen, RRModePtr mode)
{
    return;
}
#endif

Bool
xwl_screen_init_output(struct xwl_screen *xwl_screen)
{
    rrScrPrivPtr rp;

    if (!RRScreenInit(xwl_screen->screen))
        return FALSE;

    RRScreenSetSizeRange(xwl_screen->screen, 320, 200, 8192, 8192);

    rp = rrGetScrPriv(xwl_screen->screen);
    rp->rrGetInfo = xwl_randr_get_info;

#if RANDR_10_INTERFACE
    rp->rrSetConfig = xwl_randr_set_config;
#endif

#if RANDR_12_INTERFACE
    rp->rrScreenSetSize = xwl_randr_screen_set_size;
    rp->rrCrtcSet = xwl_randr_crtc_set;
    rp->rrCrtcSetGamma = xwl_randr_crtc_set_gamma;
    rp->rrCrtcGetGamma = xwl_randr_Crtc_get_gamma;
    rp->rrOutputSetProperty = xwl_randr_output_set_property;
    rp->rrOutputValidateMode = xwl_output_validate_mode;
    rp->rrModeDestroy = xwl_randr_mode_destroy;
#endif

    return TRUE;
}
