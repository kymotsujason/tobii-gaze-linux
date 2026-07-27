/* gaze-cal/src/main.c - CLI front end.
 *
 * Task 11 provides only what the connection layer needs to be driven by hand:
 * `status` and `monitor`. Tasks 12, 13 and 15 add `display`, `calibrate` and
 * `record` here. Everything that talks to the daemon lives in client.c, so
 * this file stays argv parsing and printf.
 *
 * It is also the only file that links Xlib, through stimulus.c. calibrate.c
 * takes the stimulus as a function pointer so that the calibration sequence
 * can be tested with no display present.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "calibrate.h"
#include "client.h"
#include "display.h"
#include "stimulus.h"

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

static void install_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void print_status(const struct gz_client *c) {
    if (!c->have_status) {
        printf("status: not received\n");
        return;
    }
    printf("status: device_present=%u calibration_applied=%u protocol_version=%u%s\n",
           c->status.device_present, c->status.calibration_applied,
           c->status.protocol_version,
           c->version_mismatch ? "  (UNKNOWN VERSION)" : "");
}

static int cmd_status(const char *path) {
    struct gz_client c;
    if (gz_client_connect(&c, path) != 0) {
        fprintf(stderr, "connect %s: %s\n", path, strerror(errno));
        return 1;
    }
    /* The status frame is always the first frame on a fresh connection, and it
     * reaches unsubscribed clients too, so one poll is enough. */
    uint64_t deadline = gz_now_ns() + 2000000000ULL;
    while (!c.have_status && gz_now_ns() < deadline) {
        if (gz_client_poll(&c, 200) == GZ_CLIENT_RECONNECT) break;
    }
    print_status(&c);
    int rc = c.have_status ? 0 : 1;
    gz_client_close(&c);
    return rc;
}

static int cmd_monitor(const char *path, double seconds) {
    struct gz_client c;
    gz_client_init(&c);

    uint64_t t_end = gz_now_ns() + (uint64_t)(seconds * 1e9);
    uint64_t t_report = gz_now_ns() + 1000000000ULL;
    uint64_t connected_at = 0;
    unsigned long long window_frames = 0;
    unsigned reconnects = 0;
    int connected = 0;

    install_handlers();

    while (!stop_requested && gz_now_ns() < t_end) {
        if (!connected) {
            if (gz_client_connect(&c, path) != 0) {
                fprintf(stderr, "connect %s: %s, retrying\n", path, strerror(errno));
                sleep_ms(250);
                continue;
            }
            connected = 1;
            connected_at = gz_now_ns();
            printf("connected, subscribe sent\n");
        }

        int r = gz_client_poll(&c, 50);
        if (r == GZ_CLIENT_RECONNECT) {
            printf("link lost after %.1f s, reconnecting\n",
                   (double)(gz_now_ns() - connected_at) / 1e9);
            gz_client_close(&c);
            connected = 0;
            reconnects++;
            continue;
        }
        window_frames += (unsigned long long)r;

        int w = gz_client_watchdog(&c, gz_now_ns());
        if (w == GZ_CLIENT_RECONNECT) {
            printf("watchdog: no gaze for %.1f s, reconnecting\n",
                   (double)(gz_now_ns() - c.last_gaze_ns) / 1e9);
            gz_client_close(&c);
            connected = 0;
            reconnects++;
            continue;
        }

        uint64_t now = gz_now_ns();
        if (now >= t_report) {
            const char *link = (w == GZ_LINK_STALE) ? "STALE (device absent)" : "ok";
            printf("%6.1f Hz  frames=%llu dropped=%llu  present=%u cal=%u  link=%s",
                   (double)window_frames,
                   (unsigned long long)c.gaze_frames, (unsigned long long)c.dropped,
                   c.have_status ? c.status.device_present : 0,
                   c.have_status ? c.status.calibration_applied : 0,
                   link);
            if (c.have_latest) {
                printf("  gaze=(%.4f, %.4f) valid=%d",
                       c.latest.gaze_point_2d_norm[0], c.latest.gaze_point_2d_norm[1],
                       gz_sample_any_eye_valid(&c.latest));
            }
            printf("\n");
            fflush(stdout);
            window_frames = 0;
            t_report = now + 1000000000ULL;
        }
    }

    printf("total frames=%llu dropped=%llu reconnects=%u\n",
           (unsigned long long)c.gaze_frames, (unsigned long long)c.dropped, reconnects);
    gz_client_close(&c);
    return 0;
}

/* ---------------- the X stimulus, wired into calibrate.c ----------------
 *
 * One shim so that calibrate.c never sees an Xlib type. Everything that needs
 * a dot on the gameplay monitor goes through here. */
static int stim_show(void *ctx, double nx, double ny) {
    return gz_stimulus_show((struct gz_stimulus *)ctx, nx, ny);
}

/* Opens the window and runs `fn`. The output geometry is read from RandR at
 * runtime: the plan and CLAUDE.md both name a monitor that does not exist on
 * this machine, and a hardcoded offset would have put every dot 1440 px above
 * where the eye was looking. */
static int with_stimulus(const char *output,
                         int (*fn)(const struct gz_stim_ops *, const struct gz_screen *,
                                   void *), void *arg) {
    struct gz_stimulus *s = gz_stimulus_open(output);
    if (s == NULL) return 3;

    struct gz_stim_ops ops;
    ops.ctx = s;
    ops.show = stim_show;

    int rc = fn(&ops, gz_stimulus_screen(s), arg);

    gz_stimulus_blank(s);
    gz_stimulus_close(s);
    return rc;
}

struct cal_args { const char *sock, *cfg, *label; };

static int run_calibrate(const struct gz_stim_ops *ops, const struct gz_screen *scr, void *a) {
    struct cal_args *x = a;
    return gz_cmd_calibrate(x->sock, x->cfg, ops, scr);
}

static int run_accuracy(const struct gz_stim_ops *ops, const struct gz_screen *scr, void *a) {
    struct cal_args *x = a;
    return gz_cmd_accuracy(x->sock, x->cfg, x->label, ops, scr);
}

static int run_probe(const struct gz_stim_ops *ops, const struct gz_screen *scr, void *a) {
    struct cal_args *x = a;
    return gz_cmd_probe(x->sock, x->cfg, ops, scr);
}

static int run_fit(const struct gz_stim_ops *ops, const struct gz_screen *scr, void *a) {
    struct cal_args *x = a;
    return gz_cmd_fit(x->sock, x->cfg, ops, scr);
}

static void usage(void) {
    fprintf(stderr,
            "usage: gaze-cal [--socket PATH] <command>\n"
            "  status                     print the device status and exit\n"
            "  monitor [seconds]          stream gaze, reporting rate and drops\n"
            "  display [--config PATH] [--tol MM]\n"
            "                             read the display area back off the device and\n"
            "                             refuse if it disagrees with the config\n"
            "  probe [--output NAME] [--config PATH]\n"
            "                             settle the tracker z-axis sign and the gaze\n"
            "                             frame orientation. RUN THIS BEFORE CALIBRATING\n"
            "  calibrate [--output NAME] [--config PATH]\n"
            "                             nine-point calibration, saves the blob to\n"
            "                             ~/.local/share/tobii-gaze/calibration.bin\n"
            "  apply-saved                apply that saved blob to the device\n"
            "  fit [--output NAME] [--config PATH]\n"
            "                             nine-point sweep, then fit the host-side\n"
            "                             correction into\n"
            "                             ~/.local/share/tobii-gaze/correction.conf\n"
            "  accuracy [--output NAME] [--config PATH] [--label TEXT]\n"
            "                             measure gaze error at the nine points, and\n"
            "                             report it corrected when a fit is on disk\n"
            "  preview [--sample N]       print N normalised gaze samples on stdout\n"
            "\n"
            "--output names an X RandR output; the default is the X primary.\n"
            "display, probe, calibrate and accuracy exit 0 when they agree, 1 when the\n"
            "device disagrees (fix with tobiifreed --force-display-area), 3 when the\n"
            "geometry could not be read at all, and 2 on a usage or config error.\n");
}

/* --output/--config/--label, shared by the three stimulus commands. Returns 0,
 * or -1 on an argument this command does not take. */
static int parse_common(int argc, char **argv, int from,
                        const char **output, const char **cfg, const char **label) {
    for (int a = from; a < argc; a++) {
        if (strcmp(argv[a], "--output") == 0 && a + 1 < argc) {
            *output = argv[++a];
        } else if (strcmp(argv[a], "--config") == 0 && a + 1 < argc) {
            *cfg = argv[++a];
        } else if (label != NULL && strcmp(argv[a], "--label") == 0 && a + 1 < argc) {
            *label = argv[++a];
        } else {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    char path[512];

    /* Redirected to a file, stdout is block buffered and the event lines land
     * out of order against the unbuffered stderr ones, which makes a recovery
     * log unreadable. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (gz_socket_path(path, sizeof path) != 0) {
        fprintf(stderr, "socket path does not fit\n");
        return 2;
    }

    int i = 1;
    while (i < argc && strcmp(argv[i], "--socket") == 0) {
        if (i + 1 >= argc) { usage(); return 2; }
        if (strlen(argv[i + 1]) >= sizeof path) {
            fprintf(stderr, "socket path too long\n");
            return 2;
        }
        snprintf(path, sizeof path, "%s", argv[i + 1]);
        i += 2;
    }

    if (i >= argc) { usage(); return 2; }

    if (strcmp(argv[i], "status") == 0) return cmd_status(path);
    if (strcmp(argv[i], "display") == 0) {
        const char *cfg = NULL;
        double tol = GZ_DA_TOL_MM;
        for (int a = i + 1; a < argc; a++) {
            if (strcmp(argv[a], "--config") == 0 && a + 1 < argc) {
                cfg = argv[++a];
            } else if (strcmp(argv[a], "--tol") == 0 && a + 1 < argc) {
                /* strtod rather than atof: atof reports unparseable input as
                 * 0.0, and 0.0 is a legitimate tolerance here (the comparison
                 * is <=), so a typo would silently become the strictest gate
                 * rather than an error. */
                char *end = NULL;
                tol = strtod(argv[++a], &end);
                if (end == argv[a] || *end != '\0' || !(tol >= 0)) {
                    fprintf(stderr, "--tol wants a non-negative number of mm\n");
                    return 2;
                }
            } else {
                usage();
                return 2;
            }
        }
        return gz_cmd_display(path, cfg, tol);
    }
    if (strcmp(argv[i], "monitor") == 0) {
        double seconds = 10.0;
        if (i + 1 < argc) seconds = atof(argv[i + 1]);
        if (seconds <= 0) seconds = 10.0;
        return cmd_monitor(path, seconds);
    }
    if (strcmp(argv[i], "probe") == 0 || strcmp(argv[i], "calibrate") == 0 ||
        strcmp(argv[i], "fit") == 0 || strcmp(argv[i], "accuracy") == 0) {
        struct cal_args x = { path, NULL, NULL };
        const char *output = NULL;
        int is_acc = (strcmp(argv[i], "accuracy") == 0);
        if (parse_common(argc, argv, i + 1, &output, &x.cfg, is_acc ? &x.label : NULL) != 0) {
            usage();
            return 2;
        }
        if (strcmp(argv[i], "probe") == 0)     return with_stimulus(output, run_probe, &x);
        if (strcmp(argv[i], "calibrate") == 0) return with_stimulus(output, run_calibrate, &x);
        if (strcmp(argv[i], "fit") == 0)       return with_stimulus(output, run_fit, &x);
        return with_stimulus(output, run_accuracy, &x);
    }
    if (strcmp(argv[i], "apply-saved") == 0) {
        if (i + 1 != argc) { usage(); return 2; }
        return gz_cmd_apply_saved(path);
    }
    if (strcmp(argv[i], "preview") == 0) {
        long n = 200;
        for (int a = i + 1; a < argc; a++) {
            if (strcmp(argv[a], "--sample") == 0 && a + 1 < argc) {
                char *end = NULL;
                n = strtol(argv[++a], &end, 10);
                if (end == argv[a] || *end != '\0' || n <= 0) {
                    fprintf(stderr, "--sample wants a positive count\n");
                    return 2;
                }
            } else {
                usage();
                return 2;
            }
        }
        return gz_cmd_preview(path, n);
    }

    usage();
    return 2;
}
