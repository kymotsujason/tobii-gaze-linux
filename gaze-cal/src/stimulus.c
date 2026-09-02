/* gaze-cal/src/stimulus.c - see stimulus.h. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>
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

/* Smallest text height the setup view is allowed to draw with. A reader sits
 * about 600 mm from DP-2, whose pitch is 590.42 mm / 2560 px = 0.2306 mm, so
 * 30 px is 6.9 mm, or 40 arcmin. The one core font this machine actually has
 * is 13 px, which is 17 arcmin and too small to read from the chair, hence the
 * integer upscale below. */
#define GZ_STIM_TEXT_MIN_PX 30
#define GZ_STIM_TEXT_MAX_SCALE 4
#define GZ_STIM_COLOR_CACHE 16

struct gz_stimulus {
    Display *dpy;
    Window win;
    GC gc;
    unsigned long black, white;
    struct gz_screen screen;
    int open;
    int input_mode;              /* 1 when opened by gz_stimulus_open_input */

    /* Input mode only, all zero for a plain gz_stimulus_open. */
    Pixmap back;
    XFontStruct *font;
    char font_name[128];
    int text_scale;
    Pixmap scratch;              /* depth 1, one text line wide */
    GC scratch_gc;               /* a GC is bound to a depth, so depth 1 needs its own */
    int scratch_w, scratch_h;
    int grabbed;
    unsigned long cache_rgb[GZ_STIM_COLOR_CACHE], cache_px[GZ_STIM_COLOR_CACHE];
    int cache_n;
};

static struct gz_stimulus g_stim;

/* Every open failure past XOpenDisplay lands here, so the process never exits
 * holding a keyboard grab or a mapped override_redirect window. */
static struct gz_stimulus *open_fail(void) {
    if (g_stim.dpy != NULL) {
        if (g_stim.grabbed) XUngrabKeyboard(g_stim.dpy, CurrentTime);
        if (g_stim.font != NULL) XFreeFont(g_stim.dpy, g_stim.font);
        if (g_stim.scratch_gc != NULL) XFreeGC(g_stim.dpy, g_stim.scratch_gc);
        if (g_stim.scratch != 0) XFreePixmap(g_stim.dpy, g_stim.scratch);
        if (g_stim.back != 0) XFreePixmap(g_stim.dpy, g_stim.back);
        if (g_stim.gc != NULL) XFreeGC(g_stim.dpy, g_stim.gc);
        if (g_stim.win != 0) XDestroyWindow(g_stim.dpy, g_stim.win);
        XCloseDisplay(g_stim.dpy);
    }
    memset(&g_stim, 0, sizeof g_stim);
    return NULL;
}

/* The brief asked for a 34 px helvetica and this machine cannot supply one:
 * the X font path is "built-ins" and XListFonts returns six names, of which
 * only fixed and 6x13 are text. Keep the list anyway so a machine with the
 * bitmap font packages gets a real font, and fall back to fixed plus an
 * upscale. */
static int load_font(int scr) {
    static const char *fonts[] = {
        "-*-helvetica-bold-r-*-*-34-*-*-*-*-*-iso8859-1",
        "-*-*-bold-r-*-*-34-*-*-*-*-*-iso8859-1",
        "-*-*-medium-r-*-*-34-*-*-*-*-*-iso8859-1",
        "-*-*-bold-r-*-*-24-*-*-*-*-*-iso8859-1",
        "-*-*-medium-r-*-*-24-*-*-*-*-*-iso8859-1",
        "fixed",
        NULL
    };
    for (int i = 0; fonts[i] != NULL; i++) {
        g_stim.font = XLoadQueryFont(g_stim.dpy, fonts[i]);
        if (g_stim.font != NULL) {
            snprintf(g_stim.font_name, sizeof g_stim.font_name, "%s", fonts[i]);
            break;
        }
    }
    if (g_stim.font == NULL) {
        fprintf(stderr, "stimulus: no usable X font\n");
        return -1;
    }

    int h = g_stim.font->ascent + g_stim.font->descent;
    if (h <= 0) {
        fprintf(stderr, "stimulus: font %s has no height\n", g_stim.font_name);
        return -1;
    }
    g_stim.text_scale = 1;
    if (h < GZ_STIM_TEXT_MIN_PX) {
        g_stim.text_scale = (GZ_STIM_TEXT_MIN_PX + h - 1) / h;
        if (g_stim.text_scale > GZ_STIM_TEXT_MAX_SCALE)
            g_stim.text_scale = GZ_STIM_TEXT_MAX_SCALE;
    }

    if (g_stim.text_scale > 1) {
        /* One line of unscaled glyphs, as wide as the screen so no caller can
         * overrun it. 2560 x 13 bits is under 5 KB on the server. */
        g_stim.scratch_w = g_stim.screen.w;
        g_stim.scratch_h = h;
        g_stim.scratch = XCreatePixmap(g_stim.dpy, RootWindow(g_stim.dpy, scr),
                                       (unsigned)g_stim.scratch_w,
                                       (unsigned)g_stim.scratch_h, 1);
        g_stim.scratch_gc = XCreateGC(g_stim.dpy, g_stim.scratch, 0, NULL);
        if (g_stim.scratch_gc == NULL) {
            fprintf(stderr, "stimulus: cannot make the text scratch GC\n");
            return -1;
        }
        XSetFont(g_stim.dpy, g_stim.scratch_gc, g_stim.font->fid);
    }

    fprintf(stderr, "stimulus: font %s, %d px, drawn at %dx (%d px)\n",
            g_stim.font_name, h, g_stim.text_scale, h * g_stim.text_scale);
    return 0;
}

static int grab_keyboard(void) {
    /* The grab can fail while another client holds one, or before the map
     * completes; retry a few times over 200 ms rather than run a view that
     * Escape cannot close. */
    for (int i = 0; i < 20 && !g_stim.grabbed; i++) {
        if (XGrabKeyboard(g_stim.dpy, g_stim.win, True, GrabModeAsync, GrabModeAsync,
                          CurrentTime) == GrabSuccess) {
            g_stim.grabbed = 1;
        } else {
            struct timespec t = { 0, 10 * 1000 * 1000 };
            nanosleep(&t, NULL);
        }
    }
    if (!g_stim.grabbed) {
        fprintf(stderr, "stimulus: could not grab the keyboard\n");
        return -1;
    }
    return 0;
}

static struct gz_stimulus *open_common(const char *output, int input) {
    if (g_stim.open) {
        /* The same mode twice is the one handle, which is what a second
         * gz_stimulus_open has always got. A mismatch is refused rather than
         * served or upgraded. Handing a plain window back to open_input gives
         * a handle with no back buffer, no font and no KeyPressMask, so every
         * primitive no-ops and no key ever arrives, and handing the input
         * window back to a plain open gives a second owner whose matching
         * gz_stimulus_close ungrabs the keyboard and destroys the window under
         * the live view. Upgrading in place would only cover the first
         * direction and would still leave two owners of one close, so both
         * directions fail loudly the way find_output does. */
        if (g_stim.input_mode == input) return &g_stim;
        fprintf(stderr, "stimulus: a %s window is already open on %s, "
                        "cannot also open a %s one. REFUSING.\n",
                g_stim.input_mode ? "setup" : "plain", g_stim.screen.name,
                input ? "setup" : "plain");
        return NULL;
    }

    memset(&g_stim, 0, sizeof g_stim);

    g_stim.dpy = XOpenDisplay(NULL);
    if (g_stim.dpy == NULL) {
        fprintf(stderr, "stimulus: cannot open the X display ($DISPLAY)\n");
        return NULL;
    }
    int event_base = 0, error_base = 0;
    if (!XRRQueryExtension(g_stim.dpy, &event_base, &error_base)) {
        fprintf(stderr, "stimulus: the X server has no RandR extension\n");
        return open_fail();
    }
    if (find_output(g_stim.dpy, output, &g_stim.screen) != 0) return open_fail();

    int scr = DefaultScreen(g_stim.dpy);
    g_stim.black = BlackPixel(g_stim.dpy, scr);
    g_stim.white = WhitePixel(g_stim.dpy, scr);

    XSetWindowAttributes at;
    memset(&at, 0, sizeof at);
    /* override_redirect keeps KWin out of the way entirely: no decoration, no
     * placement policy, no focus steal. The terminal that launched this keeps
     * the keyboard, which is the only way to interrupt a run. In input mode
     * that is exactly the problem, so the grab below takes it back. */
    at.override_redirect = True;
    at.background_pixel = g_stim.black;
    at.event_mask = input ? (ExposureMask | KeyPressMask) : ExposureMask;

    g_stim.win = XCreateWindow(g_stim.dpy, RootWindow(g_stim.dpy, scr),
                               g_stim.screen.x, g_stim.screen.y,
                               (unsigned)g_stim.screen.w, (unsigned)g_stim.screen.h,
                               0, CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWEventMask, &at);
    g_stim.gc = XCreateGC(g_stim.dpy, g_stim.win, 0, NULL);
    XMapRaised(g_stim.dpy, g_stim.win);
    XSync(g_stim.dpy, False);

    fprintf(stderr, "stimulus: %s %dx%d at +%d+%d\n", g_stim.screen.name,
            g_stim.screen.w, g_stim.screen.h, g_stim.screen.x, g_stim.screen.y);

    if (input) {
        g_stim.back = XCreatePixmap(g_stim.dpy, g_stim.win,
                                    (unsigned)g_stim.screen.w, (unsigned)g_stim.screen.h,
                                    (unsigned)DefaultDepth(g_stim.dpy, scr));
        if (load_font(scr) != 0) return open_fail();
        XSetFont(g_stim.dpy, g_stim.gc, g_stim.font->fid);
        if (grab_keyboard() != 0) return open_fail();
        /* Start black rather than on whatever the pixmap was allocated over. */
        XSetForeground(g_stim.dpy, g_stim.gc, g_stim.black);
        XFillRectangle(g_stim.dpy, g_stim.back, g_stim.gc, 0, 0,
                       (unsigned)g_stim.screen.w, (unsigned)g_stim.screen.h);
    }

    g_stim.open = 1;
    g_stim.input_mode = input;
    return &g_stim;
}

struct gz_stimulus *gz_stimulus_open(const char *output) {
    return open_common(output, 0);
}

struct gz_stimulus *gz_stimulus_open_input(const char *output) {
    return open_common(output, 1);
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
    if (s == NULL || !s->open) return;
    /* The ungrab comes first and is unconditional past the flag, because a
     * grab that outlives the process is a locked keyboard for the whole
     * session. XCloseDisplay would release it too, but only if we reach it. */
    if (s->grabbed) {
        XUngrabKeyboard(s->dpy, CurrentTime);
        XFlush(s->dpy);
        s->grabbed = 0;
    }
    if (s->font != NULL) { XFreeFont(s->dpy, s->font); s->font = NULL; }
    if (s->scratch_gc != NULL) { XFreeGC(s->dpy, s->scratch_gc); s->scratch_gc = NULL; }
    if (s->scratch != 0) { XFreePixmap(s->dpy, s->scratch); s->scratch = 0; }
    if (s->back != 0) { XFreePixmap(s->dpy, s->back); s->back = 0; }
    XFreeGC(s->dpy, s->gc);
    XDestroyWindow(s->dpy, s->win);
    XCloseDisplay(s->dpy);
    s->dpy = NULL;
    s->open = 0;
    s->input_mode = 0;
}

/* XAllocColor is a round trip and every frame asks for the same handful of
 * colours, so keep them. On a TrueColor visual it only computes a pixel from
 * the masks and cannot fail, but the fallback covers the pseudocolour case
 * rather than drawing with an uninitialised pixel. */
static unsigned long pixel_for(struct gz_stimulus *s, unsigned long rgb) {
    for (int i = 0; i < s->cache_n; i++)
        if (s->cache_rgb[i] == rgb) return s->cache_px[i];

    XColor c;
    memset(&c, 0, sizeof c);
    c.red   = (unsigned short)(((rgb >> 16) & 0xFFu) * 257u);
    c.green = (unsigned short)(((rgb >> 8) & 0xFFu) * 257u);
    c.blue  = (unsigned short)((rgb & 0xFFu) * 257u);
    c.flags = DoRed | DoGreen | DoBlue;
    unsigned long px = s->white;
    int ok = XAllocColor(s->dpy, DefaultColormap(s->dpy, DefaultScreen(s->dpy)), &c);
    if (ok) px = c.pixel;

    /* Cache only a real allocation, so one transient failure does not pin this
     * rgb to white for the life of the window. */
    if (ok && s->cache_n < GZ_STIM_COLOR_CACHE) {
        s->cache_rgb[s->cache_n] = rgb;
        s->cache_px[s->cache_n] = px;
        s->cache_n++;
    }
    return px;
}

/* Every primitive draws into the back pixmap, which only input mode creates.
 * Drawing to Pixmap 0 is a BadDrawable, and Xlib's default error handler exits
 * the process, so guard rather than trust the caller. */
static int can_draw(const struct gz_stimulus *s) {
    return s != NULL && s->open && s->back != 0;
}

void gz_stimulus_clear(struct gz_stimulus *s) {
    if (!can_draw(s)) return;
    XSetForeground(s->dpy, s->gc, s->black);
    XFillRectangle(s->dpy, s->back, s->gc, 0, 0,
                   (unsigned)s->screen.w, (unsigned)s->screen.h);
}

void gz_stimulus_rect(struct gz_stimulus *s, int x, int y, int w, int h,
                      unsigned long rgb, int filled) {
    if (!can_draw(s) || w <= 0 || h <= 0) return;
    XSetForeground(s->dpy, s->gc, pixel_for(s, rgb));
    if (filled) XFillRectangle(s->dpy, s->back, s->gc, x, y, (unsigned)w, (unsigned)h);
    else        XDrawRectangle(s->dpy, s->back, s->gc, x, y, (unsigned)w, (unsigned)h);
}

void gz_stimulus_disc(struct gz_stimulus *s, int cx, int cy, int r, unsigned long rgb) {
    if (!can_draw(s) || r <= 0) return;
    XSetForeground(s->dpy, s->gc, pixel_for(s, rgb));
    XFillArc(s->dpy, s->back, s->gc, cx - r, cy - r,
             (unsigned)(2 * r), (unsigned)(2 * r), 0, 360 * 64);
}

void gz_stimulus_ring(struct gz_stimulus *s, int cx, int cy, int r, int width,
                      unsigned long rgb, int degrees) {
    if (!can_draw(s) || r <= 0 || width <= 0 || degrees <= 0) return;
    if (degrees > 360) degrees = 360;
    XSetForeground(s->dpy, s->gc, pixel_for(s, rgb));
    XSetLineAttributes(s->dpy, s->gc, (unsigned)width, LineSolid, CapButt, JoinMiter);
    /* Xlib angles start at 3 o'clock and run anticlockwise in 64ths of a
     * degree. Start at 12 o'clock and sweep clockwise, which is a negative
     * extent from 90 degrees. */
    XDrawArc(s->dpy, s->back, s->gc, cx - r, cy - r,
             (unsigned)(2 * r), (unsigned)(2 * r), 90 * 64, -degrees * 64);
    XSetLineAttributes(s->dpy, s->gc, 0, LineSolid, CapButt, JoinMiter);
}

/* Blows the one available core font up by an integer factor. The glyphs go to
 * a depth 1 pixmap, come back as a bitmap, and each horizontal run of set bits
 * becomes one scaled rectangle. Runs, not pixels: a 20 character line at 6x13
 * is about 300 rectangles rather than 1560, and they go out in batches of 256
 * so an arbitrarily long string still costs a bounded amount of memory. The
 * result is chunky, which is fine for a diagnostic view and much better than
 * text nobody can read from the chair. */
static void draw_text_scaled(struct gz_stimulus *s, int x, int y,
                             const char *text, int n, int src_w) {
    if (s->scratch == 0 || src_w <= 0) return;
    if (src_w > s->scratch_w) src_w = s->scratch_w;
    int src_h = s->scratch_h;

    XSetForeground(s->dpy, s->scratch_gc, 0);
    XFillRectangle(s->dpy, s->scratch, s->scratch_gc, 0, 0,
                   (unsigned)src_w, (unsigned)src_h);
    XSetForeground(s->dpy, s->scratch_gc, 1);
    XDrawString(s->dpy, s->scratch, s->scratch_gc, 0, s->font->ascent, text, n);

    XImage *im = XGetImage(s->dpy, s->scratch, 0, 0, (unsigned)src_w, (unsigned)src_h,
                           1UL, XYPixmap);
    if (im == NULL) return;

    int sc = s->text_scale;
    int top = y - s->font->ascent * sc;
    XRectangle batch[256];
    int nb = 0;
    for (int row = 0; row < src_h; row++) {
        int col = 0;
        while (col < src_w) {
            if (XGetPixel(im, col, row) == 0) { col++; continue; }
            int start = col;
            while (col < src_w && XGetPixel(im, col, row) != 0) col++;
            batch[nb].x = (short)(x + start * sc);
            batch[nb].y = (short)(top + row * sc);
            batch[nb].width = (unsigned short)((col - start) * sc);
            batch[nb].height = (unsigned short)sc;
            nb++;
            if (nb == (int)(sizeof batch / sizeof batch[0])) {
                XFillRectangles(s->dpy, s->back, s->gc, batch, nb);
                nb = 0;
            }
        }
    }
    if (nb > 0) XFillRectangles(s->dpy, s->back, s->gc, batch, nb);
    XDestroyImage(im);
}

int gz_stimulus_text(struct gz_stimulus *s, int x, int y, const char *text,
                     unsigned long rgb) {
    if (!can_draw(s) || s->font == NULL || text == NULL) return 0;
    int n = (int)strlen(text);
    if (n <= 0) return 0;

    int w = XTextWidth(s->font, text, n);
    XSetForeground(s->dpy, s->gc, pixel_for(s, rgb));
    if (s->text_scale <= 1) {
        XDrawString(s->dpy, s->back, s->gc, x, y, text, n);
        return w;
    }
    /* draw_text_scaled clamps to the scratch width, so report the clamped
     * width rather than one the caller cannot have seen. */
    if (w > s->scratch_w) w = s->scratch_w;
    draw_text_scaled(s, x, y, text, n, w);
    return w * s->text_scale;
}

int gz_stimulus_text_height(const struct gz_stimulus *s) {
    if (s == NULL || s->font == NULL) return 0;
    return (s->font->ascent + s->font->descent) * s->text_scale;
}

void gz_stimulus_present(struct gz_stimulus *s) {
    if (!can_draw(s)) return;
    XCopyArea(s->dpy, s->back, s->win, s->gc, 0, 0,
              (unsigned)s->screen.w, (unsigned)s->screen.h, 0, 0);
    XFlush(s->dpy);
}

static int classify_key(XKeyEvent *ev) {
    KeySym sym = XLookupKeysym(ev, 0);
    if (sym == XK_Return || sym == XK_KP_Enter) return GZ_KEY_ENTER;
    if (sym == XK_Escape) return GZ_KEY_ESCAPE;
    return GZ_KEY_OTHER;
}

int gz_stimulus_key(struct gz_stimulus *s) {
    if (s == NULL || !s->open) return GZ_KEY_NONE;

    /* The draft assigned over `key` on every press, so a stray key arriving in
     * the same poll as Enter swallowed the Enter. Keep the FIRST press instead
     * and let Escape win outright, which is the stated intent and loses
     * nothing a dwell-and-confirm view cares about. */
    int key = GZ_KEY_NONE;
    while (XPending(s->dpy) > 0) {
        XEvent ev;
        XNextEvent(s->dpy, &ev);
        if (ev.type != KeyPress) continue;
        int k = classify_key(&ev.xkey);
        if (k == GZ_KEY_ESCAPE) return GZ_KEY_ESCAPE;
        if (key == GZ_KEY_NONE) key = k;
    }
    return key;
}
