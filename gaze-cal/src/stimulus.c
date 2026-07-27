/* gaze-cal/src/stimulus.c - see stimulus.h. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#include "stimulus.h"

/* Shared by gz_screen_find and gz_stimulus_open so neither opens a second
 * connection to say the same thing. */
static int find_output(Display *dpy, const char *name, struct gz_screen *out) {
    Window root = RootWindow(dpy, DefaultScreen(dpy));

    /* Current, not XRRGetScreenResources: the latter re-polls every output,
     * which takes hundreds of ms and is not needed to read a live layout. */
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
    if (res == NULL) {
        fprintf(stderr, "stimulus: XRandR returned no screen resources\n");
        return -1;
    }

    RROutput primary = XRRGetOutputPrimary(dpy, root);
    int found = 0;

    fprintf(stderr, "stimulus: connected outputs:");
    for (int i = 0; i < res->noutput; i++) {
        XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (oi == NULL) continue;
        if (oi->connection == RR_Connected && oi->crtc != 0) {
            XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
            if (ci != NULL) {
                int is_primary = (res->outputs[i] == primary);
                fprintf(stderr, " %s(%dx%d+%d+%d%s)", oi->name,
                        (int)ci->width, (int)ci->height, ci->x, ci->y,
                        is_primary ? ",primary" : "");
                int want = (name != NULL) ? (strcmp(oi->name, name) == 0) : is_primary;
                if (want && !found) {
                    found = 1;
                    snprintf(out->name, sizeof out->name, "%s", oi->name);
                    out->x = ci->x;
                    out->y = ci->y;
                    out->w = (int)ci->width;
                    out->h = (int)ci->height;
                }
                XRRFreeCrtcInfo(ci);
            }
        }
        XRRFreeOutputInfo(oi);
    }
    fprintf(stderr, "\n");
    XRRFreeScreenResources(res);

    if (!found) {
        /* Loud, and no fallback. A stimulus drawn on the wrong output is not a
         * degraded calibration, it is a calibration of the wrong thing, and it
         * looks exactly like a good one from every later step. */
        if (name != NULL)
            fprintf(stderr, "stimulus: no connected output named %s. REFUSING.\n", name);
        else
            fprintf(stderr, "stimulus: X has no primary output. REFUSING. Name one with\n"
                            "  xrandr --output DP-2 --primary\n"
                            "or pass --output NAME.\n");
        return -1;
    }
    if (out->w <= 0 || out->h <= 0) {
        fprintf(stderr, "stimulus: output %s has an empty CRTC\n", out->name);
        return -1;
    }
    return 0;
}

int gz_screen_find(const char *name, struct gz_screen *out) {
    Display *dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fprintf(stderr, "stimulus: cannot open the X display ($DISPLAY)\n");
        return -1;
    }
    int event_base = 0, error_base = 0;
    if (!XRRQueryExtension(dpy, &event_base, &error_base)) {
        fprintf(stderr, "stimulus: the X server has no RandR extension\n");
        XCloseDisplay(dpy);
        return -1;
    }
    int r = find_output(dpy, name, out);
    XCloseDisplay(dpy);
    return r;
}

struct gz_stimulus {
    Display *dpy;
    Window win;
    GC gc;
    unsigned long black, white;
    struct gz_screen screen;
    int open;
};

static struct gz_stimulus g_stim;

struct gz_stimulus *gz_stimulus_open(const char *output) {
    if (g_stim.open) return &g_stim;

    memset(&g_stim, 0, sizeof g_stim);

    g_stim.dpy = XOpenDisplay(NULL);
    if (g_stim.dpy == NULL) {
        fprintf(stderr, "stimulus: cannot open the X display ($DISPLAY)\n");
        return NULL;
    }
    int event_base = 0, error_base = 0;
    if (!XRRQueryExtension(g_stim.dpy, &event_base, &error_base)) {
        fprintf(stderr, "stimulus: the X server has no RandR extension\n");
        XCloseDisplay(g_stim.dpy);
        g_stim.dpy = NULL;
        return NULL;
    }
    if (find_output(g_stim.dpy, output, &g_stim.screen) != 0) {
        XCloseDisplay(g_stim.dpy);
        g_stim.dpy = NULL;
        return NULL;
    }

    int scr = DefaultScreen(g_stim.dpy);
    g_stim.black = BlackPixel(g_stim.dpy, scr);
    g_stim.white = WhitePixel(g_stim.dpy, scr);

    XSetWindowAttributes at;
    memset(&at, 0, sizeof at);
    /* override_redirect keeps KWin out of the way entirely: no decoration, no
     * placement policy, no focus steal. The terminal that launched this keeps
     * the keyboard, which is the only way to interrupt a run. */
    at.override_redirect = True;
    at.background_pixel = g_stim.black;
    at.event_mask = ExposureMask;

    g_stim.win = XCreateWindow(g_stim.dpy, RootWindow(g_stim.dpy, scr),
                               g_stim.screen.x, g_stim.screen.y,
                               (unsigned)g_stim.screen.w, (unsigned)g_stim.screen.h,
                               0, CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWEventMask, &at);
    g_stim.gc = XCreateGC(g_stim.dpy, g_stim.win, 0, NULL);
    XMapRaised(g_stim.dpy, g_stim.win);
    XSync(g_stim.dpy, False);

    g_stim.open = 1;
    fprintf(stderr, "stimulus: %s %dx%d at +%d+%d\n", g_stim.screen.name,
            g_stim.screen.w, g_stim.screen.h, g_stim.screen.x, g_stim.screen.y);
    return &g_stim;
}

const struct gz_screen *gz_stimulus_screen(const struct gz_stimulus *s) {
    return &s->screen;
}

int gz_stimulus_blank(struct gz_stimulus *s) {
    if (!s->open) return -1;
    XClearWindow(s->dpy, s->win);
    XSync(s->dpy, False);
    return 0;
}

int gz_stimulus_show(struct gz_stimulus *s, double nx, double ny) {
    if (!s->open) return -1;

    int px = 0, py = 0;
    gz_screen_point_px(&s->screen, nx, ny, &px, &py);
    /* The window is placed at the output's root offset, so drawing is in
     * window coordinates. Subtracting the offset here is what the hardcoded
     * +4000+0 in the brief would have got wrong twice over. */
    int wx = px - s->screen.x, wy = py - s->screen.y;

    XClearWindow(s->dpy, s->win);
    XSetForeground(s->dpy, s->gc, s->white);
    XFillArc(s->dpy, s->win, s->gc,
             wx - GZ_STIM_DOT_PX / 2, wy - GZ_STIM_DOT_PX / 2,
             GZ_STIM_DOT_PX, GZ_STIM_DOT_PX, 0, 360 * 64);
    XSetForeground(s->dpy, s->gc, s->black);
    XFillArc(s->dpy, s->win, s->gc,
             wx - GZ_STIM_INNER_PX / 2, wy - GZ_STIM_INNER_PX / 2,
             GZ_STIM_INNER_PX, GZ_STIM_INNER_PX, 0, 360 * 64);

    /* Sync, not flush: the caller times a settle window from here, and a
     * flushed-but-unprocessed request would start that clock early. */
    XSync(s->dpy, False);
    return 0;
}

void gz_stimulus_close(struct gz_stimulus *s) {
    if (!s->open) return;
    XFreeGC(s->dpy, s->gc);
    XDestroyWindow(s->dpy, s->win);
    XCloseDisplay(s->dpy);
    s->dpy = NULL;
    s->open = 0;
}
