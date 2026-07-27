/* gaze-cal/tests/test_display.c
 *
 * The gate, driven without hardware. The socket-facing half runs over a
 * socketpair standing in for the daemon, so a test can answer a
 * get_display_area with bytes the real device would never send and check that
 * the gate refuses rather than believes them.
 *
 * The point of this file is the refusals. Proving the gate accepts a matching
 * geometry proves almost nothing: a function that returned 0 unconditionally
 * would pass that. Every accepting case below is paired with a rejecting one.
 */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/display.h"

#ifdef NDEBUG
#error "test_display.c relies on assert(); do not build it with NDEBUG"
#endif

/* ---------- wire helpers, same shapes as tests/test_client.c ---------- */

static void put_be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static void put_be64(unsigned char *p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

static size_t put_hdr(unsigned char *b, uint8_t type, uint32_t len) {
    b[0] = type;
    b[1] = (unsigned char)(len & 0xFF);
    b[2] = (unsigned char)((len >> 8) & 0xFF);
    b[3] = (unsigned char)((len >> 16) & 0xFF);
    b[4] = (unsigned char)((len >> 24) & 0xFF);
    return 5;
}

static size_t put_status(unsigned char *b, uint8_t present, uint8_t cal, uint8_t ver) {
    size_t n = put_hdr(b, GZ_SRV_STATUS, 3);
    b[n++] = present; b[n++] = cal; b[n++] = ver;
    return n;
}

static size_t put_response(unsigned char *b, uint8_t cmd, const void *body, size_t len) {
    size_t n = put_hdr(b, GZ_SRV_RESPONSE, (uint32_t)(len + 1));
    b[n++] = cmd;
    if (len) memcpy(b + n, body, len);
    return n + len;
}

static size_t put_err(unsigned char *b, uint32_t code) {
    size_t n = put_hdr(b, GZ_SRV_ERR, 4);
    b[n++] = (unsigned char)(code & 0xFF);
    b[n++] = (unsigned char)((code >> 8) & 0xFF);
    b[n++] = (unsigned char)((code >> 16) & 0xFF);
    b[n++] = (unsigned char)((code >> 24) & 0xFF);
    return n;
}

/* Builds a display area body exactly the way the device does. */
static size_t put_q42(unsigned char *p, double v) {
    p[0] = 4;
    put_be32(p + 1, 8);
    put_be64(p + 5, (uint64_t)(int64_t)(v * 4398046511104.0 + (v < 0 ? -0.5 : 0.5)));
    return 13;
}

static size_t put_tag(unsigned char *p, uint32_t tag) {
    p[0] = 5;
    put_be32(p + 1, 4);
    put_be32(p + 5, tag);
    return 9;
}

static size_t build_da(unsigned char *p, const double c[9]) {
    p[0] = 0; p[1] = 0;
    size_t n = 2;
    for (int i = 0; i < 3; i++) {
        n += put_tag(p + n, GZ_TLV_TAG_POINT3D);
        n += put_q42(p + n, c[i * 3 + 0]);
        n += put_q42(p + n, c[i * 3 + 1]);
        n += put_q42(p + n, c[i * 3 + 2]);
    }
    n += put_tag(p + n, 0x010100);
    p[n] = 2; put_be32(p + n + 1, 4); put_be32(p + n + 5, 0x3039);
    return n + 9;
}

static size_t build_da_for(unsigned char *p, struct gz_rect r) {
    double c[9];
    gz_rect_to_corners(r, c);
    return build_da(p, c);
}

static void read_exact(int fd, unsigned char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        assert(r > 0);
        got += (size_t)r;
    }
}

/* Drains the mandatory subscribe so later reads see only real commands. */
static void eat_subscribe(int fd) {
    unsigned char sub[5];
    read_exact(fd, sub, sizeof sub);
    assert(sub[0] == GZ_CMD_SUBSCRIBE);
}

static const struct gz_rect real_panel = { 597, 336, -298.5, 10, 0, 0 };

/* ---------- config parsing ---------- */

static const char *write_tmp(const char *name, const char *text) {
    static char path[256];
    snprintf(path, sizeof path, "/tmp/gz_test_%d_%s.json", (int)getpid(), name);
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    if (text) fwrite(text, 1, strlen(text), f);
    fclose(f);
    return path;
}

static void test_config_matches_the_shipped_file(void) {
    /* The real ~/.config/tobii.json as of 2026-07-27, byte for byte, and the
     * corners the device reads back from it: TL=(-295.2, 338.7, 0),
     * BL=(-295.2, 5.0, 0). Anything else here means gaze-cal and the daemon
     * disagree about what the operator asked for, and the gate would compare
     * against the wrong thing.
     *
     * The 597 x 336 in the plan and in CLAUDE.md was never measured: it was
     * the plan's own example carrying a "MEASURE YOURS" comment. The panel is
     * an Alienware AW2725DF, 590.42 x 333.72 mm of active area. */
    const char *p = write_tmp("shipped",
        "{\n"
        "  \"display_area\": {\n"
        "    \"w_mm\": 590.42,\n"
        "    \"h_mm\": 333.72,\n"
        "    \"z_mm\": 0,\n"
        "    \"tilt\": 0,\n"
        "    \"cx\": 0,\n"
        "    \"cy\": \"b - 5\"\n"
        "  }\n"
        "}\n");
    struct gz_rect r;
    assert(gz_config_load_rect(p, &r) == 0);
    assert(r.w_mm == 590.42 && r.h_mm == 333.72);
    assert(r.ox_mm == -295.21);     /* -cx - w/2 */
    assert(fabs(r.oy_mm - 5.0) < 1e-9);  /* cy = -166.86 - 5, oy = -cy - h/2 */
    assert(r.z_mm == 0 && r.tilt_deg == 0);
    unlink(p);

    /* The previous shipped values, kept because the anchor grammar has to give
     * the same answer whatever the panel is. */
    p = write_tmp("shipped_old",
        "{\"display_area\":{\"w_mm\":597,\"h_mm\":336,\"z_mm\":0,\"tilt\":0,"
        "\"cx\":0,\"cy\":\"b - 10\"}}");
    assert(gz_config_load_rect(p, &r) == 0);
    assert(r.ox_mm == -298.5 && r.oy_mm == 10.0);
    unlink(p);
}

static void test_config_key_order_does_not_matter(void) {
    /* The daemon reads the object into a map before computing the halves, so
     * cx listed before w_mm still gets the right origin. A single-pass reader
     * that converted on sight would centre a 597 mm panel as if it were the
     * 1500 mm template. */
    const char *p = write_tmp("order",
        "{\"display_area\":{\"cy\":\"b - 10\",\"cx\":0,\"h_mm\":336,\"w_mm\":597}}");
    struct gz_rect r;
    assert(gz_config_load_rect(p, &r) == 0);
    assert(r.ox_mm == -298.5 && r.oy_mm == 10.0);
    unlink(p);
}

static void test_config_ignores_unknown_keys_and_nesting(void) {
    const char *p = write_tmp("extra",
        "{\"other\":{\"a\":[1,2,{\"b\":null}],\"c\":true},"
        " \"display_area\":{\"w_mm\":597,\"h_mm\":336,\"cx\":0,\"cy\":0,"
        "                   \"future_key\":[{\"x\":false}]},"
        " \"trailing\":\"s\"}");
    struct gz_rect r;
    assert(gz_config_load_rect(p, &r) == 0);
    assert(r.w_mm == 597 && r.h_mm == 336);
    assert(r.oy_mm == -168.0);
    unlink(p);
}

static void test_config_refuses_rather_than_defaulting(void) {
    /* The daemon's loadDisplayArea swallows every failure with
     * `catch return .{}` and silently uses 1500x1000. A gate that did the same
     * would compare the device against a number nobody chose, and would
     * REPORT A MATCH whenever the device also held the template. */
    const char *bad[] = {
        "",                                     /* empty */
        "{",                                    /* truncated */
        "[]",                                   /* not an object */
        "{\"display_area\": 5}",                /* not an object either */
        "{\"display_area\":{\"w_mm\":}}",        /* no value */
        "{\"display_area\":{\"w_mm\":597,}}",    /* trailing comma */
        "{\"display_area\":{\"w_mm\":\"597\"}}", /* a string where a number goes */
        "{\"display_area\":{}} junk",           /* trailing garbage */
        "{\"nothing\":1}",                      /* no display_area at all */
        "{\"display_area\":{\"cy\":\"q\"}}",     /* unknown anchor letter */
        "{\"display_area\":{\"cy\":\"l\"}}",     /* horizontal anchor on y */
        "{\"display_area\":{\"cx\":\"t\"}}",     /* vertical anchor on x */
        "{\"display_area\":{\"cy\":\"b - \"}}",  /* sign with no number */
        "{\"display_area\":{\"cy\":\"b - 10x\"}}", /* trailing text */
        "{\"display_area\":{\"cy\":\"b * 10\"}}",  /* unsupported operator */
        /* An invalid JSON escape, in a value the reader otherwise skips. A
         * reader that ignored the escape table would accept this file and
         * return a geometry from it. */
        "{\"other\":\"a\\qb\",\"display_area\":{\"w_mm\":597}}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const char *p = write_tmp("bad", bad[i]);
        struct gz_rect r = { 1, 2, 3, 4, 5, 6 };
        assert(gz_config_load_rect(p, &r) == -2);
        assert(r.w_mm == 1 && r.h_mm == 2);   /* untouched on refusal */
        unlink(p);
    }

    struct gz_rect r;
    assert(gz_config_load_rect("/nonexistent/tobii.json", &r) == -1);
}

static void test_config_refuses_a_file_longer_than_the_daemon_reads(void) {
    /* The daemon reads 4096 bytes and parses that. A longer file is truncated
     * before it ever reaches its JSON parser, so the two readers would be
     * looking at different bytes. Exactly 4096 must still be accepted. */
    char big[8192];
    const char *head = "{\"pad\":\"";
    const char *tail = "\",\"display_area\":{\"w_mm\":597,\"h_mm\":336,\"cx\":0,\"cy\":0}}";
    size_t hl = strlen(head), tl = strlen(tail);

    memcpy(big, head, hl);
    memset(big + hl, 'x', 4096 - hl - tl);
    memcpy(big + 4096 - tl, tail, tl);
    big[4096] = '\0';
    const char *p = write_tmp("exact", big);
    struct gz_rect r;
    assert(gz_config_load_rect(p, &r) == 0);
    assert(r.w_mm == 597);
    unlink(p);

    memcpy(big, head, hl);
    memset(big + hl, 'x', 4097 - hl - tl);
    memcpy(big + 4097 - tl, tail, tl);
    big[4097] = '\0';
    p = write_tmp("toobig", big);
    assert(gz_config_load_rect(p, &r) == -2);
    unlink(p);
}

static void test_config_reproduces_the_absent_cx_trap(void) {
    /* Faithfulness beats correctness here. A config with w_mm but no cx leaves
     * the origin at the 1500x1000 template's -750, which is what the daemon
     * does. Diverging would make the gate disagree with the geometry the
     * daemon actually wrote. */
    const char *p = write_tmp("nocx", "{\"display_area\":{\"w_mm\":597,\"h_mm\":336}}");
    struct gz_rect r;
    assert(gz_config_load_rect(p, &r) == 0);
    assert(r.w_mm == 597 && r.h_mm == 336);
    assert(r.ox_mm == -750 && r.oy_mm == -500);
    unlink(p);
}

static void test_anchor_expr_grammar(void) {
    double v;
    assert(gz_parse_anchor_expr("b - 10", 168, 1, &v) == 0 && v == -178);
    assert(gz_parse_anchor_expr("t + 5",  168, 1, &v) == 0 && v ==  173);
    assert(gz_parse_anchor_expr("c",      168, 1, &v) == 0 && v ==    0);
    assert(gz_parse_anchor_expr("  b",    168, 1, &v) == 0 && v == -168);
    assert(gz_parse_anchor_expr("l+2",  298.5, 0, &v) == 0 && v == -296.5);
    assert(gz_parse_anchor_expr("r",    298.5, 0, &v) == 0 && v ==  298.5);
    assert(gz_parse_anchor_expr("b - 2.5", 168, 1, &v) == 0 && v == -170.5);

    assert(gz_parse_anchor_expr("",       168, 1, &v) != 0);
    assert(gz_parse_anchor_expr("x",      168, 1, &v) != 0);
    assert(gz_parse_anchor_expr("t",    298.5, 0, &v) != 0);   /* wrong axis */
    assert(gz_parse_anchor_expr("l",      168, 1, &v) != 0);   /* wrong axis */
    assert(gz_parse_anchor_expr("b 10",   168, 1, &v) != 0);   /* no operator */
    assert(gz_parse_anchor_expr("b -",    168, 1, &v) != 0);
    assert(gz_parse_anchor_expr("b - 1 2", 168, 1, &v) != 0);

    /* All four axis-specific anchors refuse the wrong axis. main.zig does the
     * same, and a reader that did not would silently take a vertical anchor as
     * a horizontal offset, which moves the origin by half a panel. */
    assert(gz_parse_anchor_expr("b",  298.5, 0, &v) != 0);
    assert(gz_parse_anchor_expr("r",    168, 1, &v) != 0);
    assert(gz_parse_anchor_expr("c",  298.5, 0, &v) == 0 && v == 0);
}

static void test_config_accepts_a_valid_json_escape(void) {
    /* Paired with the invalid-escape refusal above: a reader that ignored the
     * escape table would accept both, and one that refused all escapes would
     * reject a legal file. */
    const char *p = write_tmp("esc",
        "{\"note\":\"a\\/b\\n\","
        " \"display_area\":{\"w_mm\":597,\"h_mm\":336,\"cx\":0,\"cy\":0}}");
    struct gz_rect r;
    assert(gz_config_load_rect(p, &r) == 0);
    assert(r.w_mm == 597 && r.h_mm == 336);
    unlink(p);
}

/* ---------- stderr capture ----------
 *
 * Only for the messages that are load-bearing rather than informational. The
 * unmeasured-mounting caveat is one: an OK from the gate is the last thing
 * between a calibration and a wrong one, and a readback cannot tell an
 * unmeasured 0 from a measured one, so the warning has to reach whoever sees
 * the OK. Untested, it is decorative and drifts.
 *
 * The capture file is unlinked in cap_end rather than at creation, so an abort
 * inside a captured region leaves the assertion text on disk instead of
 * swallowing it. */
static char cap_buf[16384];
static char cap_path[128];
static int cap_fd = -1, cap_saved = -1;

static void cap_begin(void) {
    fflush(stderr);
    snprintf(cap_path, sizeof cap_path, "/tmp/gz_cap_%d.txt", (int)getpid());
    cap_saved = dup(STDERR_FILENO);
    assert(cap_saved >= 0);
    cap_fd = open(cap_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(cap_fd >= 0);
    assert(dup2(cap_fd, STDERR_FILENO) >= 0);
}

static const char *cap_end(void) {
    fflush(stderr);
    assert(dup2(cap_saved, STDERR_FILENO) >= 0);
    close(cap_saved);
    assert(lseek(cap_fd, 0, SEEK_SET) == 0);
    ssize_t r = read(cap_fd, cap_buf, sizeof cap_buf - 1);
    if (r < 0) r = 0;
    cap_buf[r] = '\0';
    close(cap_fd);
    unlink(cap_path);
    return cap_buf;
}

/* One line of the caveat, chosen so the assertion does not straddle a wrap. */
#define CAVEAT_MARK "z_mm, tilt, cx and cy are the daemon's"

static void test_the_success_caveat_follows_the_verdict(void) {
    /* display.h names gz_display_gate as Task 13's entry, so the caveat cannot
     * live in the CLI wrapper: the library caller would lose it silently. It
     * is printed by the function that produces the verdict, and every caller
     * inherits it. */
    double c[9];
    gz_rect_to_corners(real_panel, c);
    cap_begin();
    int rc = gz_display_verify(c, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG);
    const char *out = cap_end();
    assert(rc == 0);
    assert(strstr(out, CAVEAT_MARK) != NULL);

    /* Not on a refusal. There the operator has a concrete thing to do, and a
     * caveat about parameters nobody measured only buries it. */
    struct gz_rect placeholder = { 1500, 1000, -750, -500, 0, 0 };
    gz_rect_to_corners(placeholder, c);
    cap_begin();
    rc = gz_display_verify(c, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG);
    out = cap_end();
    assert(rc == -1);
    assert(strstr(out, "REFUSING TO CALIBRATE") != NULL);
    assert(strstr(out, CAVEAT_MARK) == NULL);
}

/* ---------- verify ---------- */

static void test_verify_accepts_the_real_geometry_and_refuses_a_wrong_one(void) {
    double c[9];
    gz_rect_to_corners(real_panel, c);
    assert(gz_display_verify(c, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG) == 0);

    struct gz_rect placeholder = { 1500, 1000, -750, -500, 0, 0 };
    gz_rect_to_corners(placeholder, c);
    assert(gz_display_verify(c, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG) == -1);

    /* Right size, wrong place. The case width and height alone would pass. */
    struct gz_rect shifted = real_panel;
    shifted.oy_mm -= 40;
    gz_rect_to_corners(shifted, c);
    assert(gz_display_verify(c, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG) == -1);
}


/* ---------- the socket-facing gate ----------
 *
 * The daemon stand-in runs in a forked child driven by a script: it reads one
 * command, writes that step's reply, and repeats. Queuing replies up front
 * instead would be a different test, because gz_client_request clears its
 * response slot before sending and would discard anything that arrived early,
 * and it would never exercise a retry, which by definition needs a second
 * answer to a second command.
 *
 * The child exits with the number of commands it saw AFTER the subscribe, so a
 * test can assert that a gate refused without putting anything on the wire.
 */

#define FAKE_MAX_STEPS 4
#define FAKE_STEP_CAP 4096

struct step {
    unsigned char buf[FAKE_STEP_CAP];
    size_t len;                 /* 0 means the daemon simply says nothing */
};

struct fake {
    int fd;                     /* the child's end, closed in the parent */
    pid_t pid;
    struct gz_client c;
};

static int child_read(int fd, unsigned char *b, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, b + got, n - got);
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}

static int child_write(int fd, const unsigned char *b, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, b + sent, n - sent);
        if (w <= 0) return 0;
        sent += (size_t)w;
    }
    return 1;
}

/* hangup: close right after the status instead of waiting, which is what a
 * daemon dying mid-session looks like from here. */
static void fake_start(struct fake *f, uint8_t present, uint8_t cal, uint8_t ver,
                       const struct step *steps, size_t nsteps, int hangup) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    f->pid = fork();
    assert(f->pid >= 0);

    if (f->pid == 0) {
        close(sv[0]);
        int d = sv[1];
        int cmds = 0;
        unsigned char hdr[5];

        /* Every client must subscribe on connect, so that is the first thing
         * on the wire and it is not one of the scripted commands. */
        if (!child_read(d, hdr, 5) || hdr[0] != GZ_CMD_SUBSCRIBE) _exit(90);
        unsigned char st[16];
        size_t n = put_status(st, present, cal, ver);
        if (!child_write(d, st, n)) _exit(cmds);

        if (!hangup) {
            for (size_t i = 0; i < nsteps; i++) {
                if (!child_read(d, hdr, 5)) _exit(cmds);
                uint32_t plen = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8)
                              | ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);
                unsigned char skip[FAKE_STEP_CAP];
                if (plen > sizeof skip) _exit(91);
                if (plen && !child_read(d, skip, plen)) _exit(cmds);
                cmds++;
                if (steps[i].len && !child_write(d, steps[i].buf, steps[i].len)) _exit(cmds);
            }
            /* Silence, not EOF, for anything further: a client that keeps
             * sending must see a timeout rather than a dead peer. */
            for (;;) {
                unsigned char junk[64];
                ssize_t r = read(d, junk, sizeof junk);
                if (r <= 0) break;
            }
        }
        _exit(cmds);
    }

    close(sv[1]);
    f->fd = sv[0];
    assert(gz_client_adopt(&f->c, f->fd) == 0);
}

/* Returns the number of commands the daemon saw after the subscribe. */
static int fake_stop(struct fake *f) {
    gz_client_close(&f->c);
    int st = 0;
    assert(waitpid(f->pid, &st, 0) == f->pid);
    assert(WIFEXITED(st));
    assert(WEXITSTATUS(st) < 90);
    return WEXITSTATUS(st);
}

static void step_da(struct step *s, struct gz_rect r) {
    unsigned char body[256];
    size_t n = build_da_for(body, r);
    assert(n == 164);
    s->len = put_response(s->buf, GZ_CMD_GET_DISPLAY_AREA, body, n);
}

static void step_body(struct step *s, const unsigned char *body, size_t n) {
    s->len = put_response(s->buf, GZ_CMD_GET_DISPLAY_AREA, body, n);
}

static void step_err(struct step *s, uint32_t code) {
    s->len = put_err(s->buf, code);
}

static void step_silent(struct step *s) { s->len = 0; }

/* A short deadline for the give-up paths, so the suite does not spend
 * GZ_CLIENT_CMD_TIMEOUT_MS per case. The path under test is the same one. */
#define TEST_TIMEOUT_MS 200

static void test_gate_passes_on_a_matching_device(void) {
    struct step s[1];
    step_da(&s[0], real_panel);
    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 1, 0);
    assert(gz_display_gate(&f.c, NULL, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG)
           == GZ_GATE_OK);
    assert(fake_stop(&f) == 1);
}

static void test_gate_refuses_a_mismatched_device(void) {
    /* THE TEST THIS FILE EXISTS FOR. The device answers cleanly, the bytes
     * parse, the geometry is a perfectly ordinary rectangle, and it is not the
     * one the config asked for. */
    struct gz_rect wrong[] = {
        { 1500, 1000, -750,   -500, 0,  0 },   /* the daemon's template */
        { 597,   336, -298.5, -158, 0,  0 },   /* right size, 168 mm low */
        { 531,   299, -265.5,   10, 0,  0 },   /* a 24 inch panel, not 27 */
        { 597,   336, -298.5,   10, 0, -8 },   /* tilted */
        { 597,   336, -298.5,   10, 60, 0 },   /* 60 mm further back */
        { 597,   336,  298.5,   10, 0,  0 },   /* mirrored about x */
        { 336,   597, -168.0,   10, 0,  0 },   /* w and h swapped */
    };
    for (size_t i = 0; i < sizeof wrong / sizeof wrong[0]; i++) {
        struct step s[1];
        step_da(&s[0], wrong[i]);
        struct fake f;
        fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 1, 0);
        assert(gz_display_gate(&f.c, NULL, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG)
               == GZ_GATE_MISMATCH);
        assert(fake_stop(&f) == 1);
    }
}

static void test_gate_refuses_an_unparseable_reply(void) {
    /* A reply of the right length and the wrong shape must be refused, not
     * decoded into whatever falls out. The first case is the shape the plan
     * originally assumed: nine native doubles at offset 0. */
    double raw[9] = { -298.5, 346, 0, 298.5, 346, 0, -298.5, 10, 0 };
    unsigned char nine_doubles[72];
    memcpy(nine_doubles, raw, sizeof raw);

    unsigned char right[256];
    size_t right_len = build_da_for(right, real_panel);
    assert(right_len == 164);

    unsigned char zeros[164];
    memset(zeros, 0, sizeof zeros);

    /* Right grammar, one structural byte flipped: the prolog tag of the second
     * corner. This is the near-miss that a decoder skipping the tag check
     * would accept and turn into a wrong-but-plausible rectangle. */
    unsigned char bad_tag[256];
    memcpy(bad_tag, right, sizeof bad_tag);
    bad_tag[50 + 8] ^= 0x01;

    struct { const unsigned char *b; size_t n; } cases[] = {
        { nine_doubles, sizeof nine_doubles },
        { zeros,        sizeof zeros        },
        { bad_tag,      164                 },
        { right,        140                 },   /* truncated mid-point */
        { right,        0                   },   /* empty body */
        { (const unsigned char *)"x", 1     },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct step s[1];
        step_body(&s[0], cases[i].b, cases[i].n);
        struct fake f;
        fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 1, 0);
        assert(gz_display_gate(&f.c, NULL, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG)
               == GZ_GATE_UNKNOWN);
        assert(fake_stop(&f) == 1);
    }
}

static void test_gate_refuses_an_unknown_protocol_version(void) {
    /* THE VERSION GATE. It fires before any command reaches the wire, which is
     * the whole point: a version bump means a message changed shape, so
     * anything read afterwards would have been parsed under a grammar we had
     * just been told may no longer hold. The child is scripted to answer
     * correctly, so a gate that asked would have got a clean geometry and
     * passed. The command count proves it never asked. */
    uint8_t versions[] = { 0, 2, 7, 255 };
    for (size_t i = 0; i < sizeof versions; i++) {
        struct step s[1];
        step_da(&s[0], real_panel);
        struct fake f;
        fake_start(&f, 1, 0, versions[i], s, 1, 0);
        assert(gz_display_gate(&f.c, NULL, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG)
               == GZ_GATE_UNKNOWN);
        assert(fake_stop(&f) == 0);
    }

    /* The version this build knows is not refused, so the check above is not
     * simply always failing. */
    struct step s[1];
    step_da(&s[0], real_panel);
    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 1, 0);
    assert(gz_display_gate(&f.c, NULL, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG)
           == GZ_GATE_OK);
    assert(fake_stop(&f) == 1);
}

static void test_gate_refuses_when_the_device_is_absent(void) {
    struct step s[1];
    step_da(&s[0], real_panel);
    struct fake f;
    fake_start(&f, 0, 0, GZ_PROTOCOL_VERSION, s, 1, 0);
    assert(gz_display_gate(&f.c, NULL, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG)
           == GZ_GATE_UNKNOWN);
    assert(fake_stop(&f) == 0);   /* nothing sent to an absent device */
}

static void test_gate_refuses_without_a_status_frame(void) {
    /* The daemon sends status as the first frame on every connection, so its
     * absence is not slowness. Commanding something that has not identified
     * itself is exactly what this refuses. */
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct gz_client c;
    assert(gz_client_adopt(&c, sv[0]) == 0);
    eat_subscribe(sv[1]);
    assert(gz_display_gate_status(&c, TEST_TIMEOUT_MS) == -1);
    gz_client_close(&c);
    close(sv[1]);
}

static void test_read_retries_usb_busy(void) {
    /* err_code 2 is usb_busy: nothing reached the device, the two sides are
     * still in step, so a retry on the same connection is correct and the
     * third attempt gets a real answer. */
    struct step s[3];
    step_err(&s[0], GZ_ERRCODE_USB_BUSY);
    step_err(&s[1], GZ_ERRCODE_USB_BUSY);
    step_da(&s[2], real_panel);

    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 3, 0);
    double c[9];
    assert(gz_display_read(&f.c, NULL, TEST_TIMEOUT_MS, c) == 0);
    assert(gz_corners_to_rect(c).w_mm == 597);
    assert(fake_stop(&f) == 3);
}

static void test_read_gives_up_on_a_non_retryable_error(void) {
    /* err_code 1 is failed, and an unknown code is treated the same way: the
     * Zig enum is non-exhaustive, and only usb_busy is known to have left the
     * device untouched. Retrying anything else risks repeating a command that
     * did reach the device. */
    uint32_t codes[] = { GZ_ERRCODE_FAILED, 99, 0 };
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        struct step s[1];
        step_err(&s[0], codes[i]);
        struct fake f;
        fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 1, 0);
        double c[9];
        assert(gz_display_read(&f.c, NULL, TEST_TIMEOUT_MS, c) == GZ_CLIENT_REMOTE);
        assert(fake_stop(&f) == 1);   /* one attempt, not three */
    }
}

static void test_read_stops_retrying_usb_busy_eventually(void) {
    /* A device that is busy forever must not loop forever. */
    struct step s[FAKE_MAX_STEPS];
    for (size_t i = 0; i < FAKE_MAX_STEPS; i++) step_err(&s[i], GZ_ERRCODE_USB_BUSY);
    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, FAKE_MAX_STEPS, 0);
    double c[9];
    assert(gz_display_read(&f.c, NULL, TEST_TIMEOUT_MS, c) == GZ_CLIENT_REMOTE);
    assert(fake_stop(&f) == 3);
}

static void test_read_does_not_retry_a_timeout_on_the_same_connection(void) {
    /* After a timeout the daemon may still answer, and an err frame carries no
     * cmd_type, so a late one lands on whatever command follows. With no path
     * to reconnect through, the only correct move is to give up after one. */
    struct step s[2];
    step_silent(&s[0]);
    step_da(&s[1], real_panel);
    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 2, 0);
    double c[9];
    assert(gz_display_read(&f.c, NULL, TEST_TIMEOUT_MS, c) == GZ_CLIENT_TIMEOUT);
    assert(fake_stop(&f) == 1);
}

static void test_read_reports_a_dead_link(void) {
    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, NULL, 0, 1);
    double c[9];
    int r = gz_display_read(&f.c, NULL, TEST_TIMEOUT_MS, c);
    assert(r == GZ_CLIENT_RECONNECT || r == GZ_CLIENT_TIMEOUT);
    assert(fake_stop(&f) == 0);
}

static void test_gaze_traffic_does_not_disturb_the_gate(void) {
    /* The daemon keeps streaming at 33 Hz while a command is in flight, so the
     * reply arrives buried in gaze frames rather than alone. */
    struct step s[1];
    size_t w = 0;
    struct gz_gaze_sample g;
    memset(&g, 0, sizeof g);
    for (int i = 0; i < 4; i++) {
        g.frame_counter = (uint32_t)(4 * (i + 1));
        w += put_hdr(s[0].buf + w, GZ_SRV_GAZE, (uint32_t)sizeof g);
        memcpy(s[0].buf + w, &g, sizeof g);
        w += sizeof g;
    }
    unsigned char body[256];
    size_t n = build_da_for(body, real_panel);
    w += put_response(s[0].buf + w, GZ_CMD_GET_DISPLAY_AREA, body, n);
    g.frame_counter = 24;
    w += put_hdr(s[0].buf + w, GZ_SRV_GAZE, (uint32_t)sizeof g);
    memcpy(s[0].buf + w, &g, sizeof g);
    w += sizeof g;
    assert(w <= FAKE_STEP_CAP);
    s[0].len = w;

    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 1, 0);
    assert(gz_display_gate(&f.c, NULL, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG)
           == GZ_GATE_OK);
    assert(f.c.gaze_frames >= 4);
    assert(fake_stop(&f) == 1);
}

static void test_a_reply_to_another_command_is_not_the_answer(void) {
    /* A reply carrying a perfectly valid display area, tagged as cal_apply's,
     * must not be read as this geometry. Replies correlate on cmd_type and
     * nothing else. */
    unsigned char body[256];
    size_t n = build_da_for(body, real_panel);
    struct step s[1];
    s[0].len = put_response(s[0].buf, GZ_CMD_CAL_APPLY, body, n);

    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 1, 0);
    double c[9];
    assert(gz_display_read(&f.c, NULL, TEST_TIMEOUT_MS, c) == GZ_CLIENT_TIMEOUT);
    assert(f.c.resp_mismatch >= 1);
    assert(fake_stop(&f) == 1);
}

static void test_the_library_entry_prints_the_caveat(void) {
    struct step s[1];
    step_da(&s[0], real_panel);
    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 1, 0);

    cap_begin();
    int g = gz_display_gate(&f.c, NULL, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG);
    const char *out = cap_end();
    assert(g == GZ_GATE_OK);
    assert(strstr(out, CAVEAT_MARK) != NULL);
    assert(fake_stop(&f) == 1);
}

/* ---------- the reconnect path, over a real listening socket ----------
 *
 * Everything above passes path == NULL, so the reconnect branch has no coverage
 * there while production always reaches it with a real path through
 * gz_cmd_display. That divergence is what hid the missing re-gate, so these
 * tests take the shipped path: a listening AF_UNIX socket, gz_client_connect,
 * and a child that serves one scripted session per connection.
 */

#define SRV_MAX_CONN 3

struct conn_script {
    uint8_t present, cal, ver;
    struct step steps[FAKE_MAX_STEPS];
    size_t nsteps;
};

struct server {
    char path[96];
    pid_t pid;
    int report;                 /* read end: one byte of command count per conn */
};

/* Serves nconn sessions in order, then exits. Reports each session's
 * post-subscribe command count, so a test can prove the client refused before
 * sending rather than after. */
static void server_start(struct server *s, const struct conn_script *sc, size_t nconn) {
    snprintf(s->path, sizeof s->path, "/tmp/gz_gate_%d.sock", (int)getpid());
    unlink(s->path);

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(lfd >= 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    assert(strlen(s->path) < sizeof a.sun_path);
    memcpy(a.sun_path, s->path, strlen(s->path));
    assert(bind(lfd, (struct sockaddr *)&a, sizeof a) == 0);
    assert(listen(lfd, (int)nconn + 1) == 0);

    int pipefd[2];
    assert(pipe(pipefd) == 0);

    s->pid = fork();
    assert(s->pid >= 0);
    if (s->pid == 0) {
        close(pipefd[0]);
        for (size_t i = 0; i < nconn; i++) {
            int d = accept(lfd, NULL, NULL);
            if (d < 0) break;
            unsigned char cmds = 0;
            unsigned char hdr[5];

            if (child_read(d, hdr, 5) && hdr[0] == GZ_CMD_SUBSCRIBE) {
                unsigned char st[16];
                size_t n = put_status(st, sc[i].present, sc[i].cal, sc[i].ver);
                if (child_write(d, st, n)) {
                    for (size_t j = 0; j < sc[i].nsteps; j++) {
                        if (!child_read(d, hdr, 5)) break;
                        uint32_t plen = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8)
                                      | ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);
                        unsigned char skip[FAKE_STEP_CAP];
                        if (plen > sizeof skip) break;
                        if (plen && !child_read(d, skip, plen)) break;
                        cmds++;
                        if (sc[i].steps[j].len &&
                            !child_write(d, sc[i].steps[j].buf, sc[i].steps[j].len)) break;
                    }
                    /* Wait for the client to hang up rather than closing here,
                     * so the reconnect is the client's decision. A close would
                     * hand it an EOF and a different code path. */
                    for (;;) {
                        unsigned char junk[64];
                        ssize_t r = read(d, junk, sizeof junk);
                        if (r <= 0) break;
                    }
                }
            }
            close(d);
            if (write(pipefd[1], &cmds, 1) != 1) break;
        }
        close(pipefd[1]);
        close(lfd);
        _exit(0);
    }

    close(pipefd[1]);
    close(lfd);
    s->report = pipefd[0];
}

/* Returns how many sessions the daemon completed, filling counts[]. */
static size_t server_stop(struct server *s, unsigned char *counts, size_t cap) {
    size_t n = 0;
    while (n < cap) {
        ssize_t r = read(s->report, counts + n, 1);
        if (r != 1) break;
        n++;
    }
    close(s->report);
    int st = 0;
    assert(waitpid(s->pid, &st, 0) == s->pid);
    unlink(s->path);
    return n;
}

static void test_reconnect_regates_an_upgraded_daemon(void) {
    /* THE REGRESSION. gz_client_reconnect goes through gz_client_init, which
     * memsets the client and clears have_status and version_mismatch. Session 1
     * is a version this build knows and then goes silent, forcing a reconnect.
     * Session 2 is a version it does not know, and is primed to answer the
     * geometry correctly, so a gate that failed to re-check would decode a
     * clean 164-byte body under a grammar it had just been told may have moved.
     * The session-2 command count proves it never asked. */
    struct conn_script sc[2];
    memset(sc, 0, sizeof sc);
    sc[0].present = 1; sc[0].ver = GZ_PROTOCOL_VERSION;
    sc[0].nsteps = 1;  step_silent(&sc[0].steps[0]);
    sc[1].present = 1; sc[1].ver = GZ_PROTOCOL_VERSION + 1;
    sc[1].nsteps = 1;  step_da(&sc[1].steps[0], real_panel);

    struct server s;
    server_start(&s, sc, 2);

    struct gz_client c;
    assert(gz_client_connect(&c, s.path) == 0);
    assert(gz_display_gate(&c, s.path, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG)
           == GZ_GATE_UNKNOWN);
    gz_client_close(&c);

    unsigned char counts[SRV_MAX_CONN];
    assert(server_stop(&s, counts, SRV_MAX_CONN) == 2);
    assert(counts[0] == 1);   /* the command that went unanswered */
    assert(counts[1] == 0);   /* nothing asked of the daemon it does not know */
}

static void test_reconnect_regates_a_daemon_that_lost_its_device(void) {
    /* The same door, a different refusal: the reconnect lands on a daemon whose
     * tracker has gone. Reading a geometry from it is meaningless. */
    struct conn_script sc[2];
    memset(sc, 0, sizeof sc);
    sc[0].present = 1; sc[0].ver = GZ_PROTOCOL_VERSION;
    sc[0].nsteps = 1;  step_silent(&sc[0].steps[0]);
    sc[1].present = 0; sc[1].ver = GZ_PROTOCOL_VERSION;
    sc[1].nsteps = 1;  step_da(&sc[1].steps[0], real_panel);

    struct server s;
    server_start(&s, sc, 2);
    struct gz_client c;
    assert(gz_client_connect(&c, s.path) == 0);
    assert(gz_display_gate(&c, s.path, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG)
           == GZ_GATE_UNKNOWN);
    gz_client_close(&c);

    unsigned char counts[SRV_MAX_CONN];
    assert(server_stop(&s, counts, SRV_MAX_CONN) == 2);
    assert(counts[1] == 0);
}

static void test_reconnect_succeeds_against_the_same_daemon(void) {
    /* Pairs with the two above: without this, "refused after reconnecting"
     * would be indistinguishable from "reconnecting is broken". Session 2 is
     * the same version and answers correctly, and the gate passes. */
    struct conn_script sc[2];
    memset(sc, 0, sizeof sc);
    sc[0].present = 1; sc[0].ver = GZ_PROTOCOL_VERSION;
    sc[0].nsteps = 1;  step_silent(&sc[0].steps[0]);
    sc[1].present = 1; sc[1].ver = GZ_PROTOCOL_VERSION;
    sc[1].nsteps = 1;  step_da(&sc[1].steps[0], real_panel);

    struct server s;
    server_start(&s, sc, 2);
    struct gz_client c;
    assert(gz_client_connect(&c, s.path) == 0);
    assert(gz_display_gate(&c, s.path, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG)
           == GZ_GATE_OK);
    gz_client_close(&c);

    unsigned char counts[SRV_MAX_CONN];
    assert(server_stop(&s, counts, SRV_MAX_CONN) == 2);
    assert(counts[0] == 1 && counts[1] == 1);
}

static void test_the_shipped_path_refuses_a_mismatch(void) {
    /* One straight run of the production path, no reconnect: connect by socket
     * path, gate with that same path in hand, wrong geometry. */
    struct conn_script sc[1];
    memset(sc, 0, sizeof sc);
    sc[0].present = 1; sc[0].ver = GZ_PROTOCOL_VERSION;
    sc[0].nsteps = 1;
    struct gz_rect placeholder = { 1500, 1000, -750, -500, 0, 0 };
    step_da(&sc[0].steps[0], placeholder);

    struct server s;
    server_start(&s, sc, 1);
    struct gz_client c;
    assert(gz_client_connect(&c, s.path) == 0);
    assert(gz_display_gate(&c, s.path, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG)
           == GZ_GATE_MISMATCH);
    gz_client_close(&c);

    unsigned char counts[SRV_MAX_CONN];
    assert(server_stop(&s, counts, SRV_MAX_CONN) == 1);
    assert(counts[0] == 1);
}

/* ---------- a quad that is not a rectangle ---------- */

static void test_a_skewed_quad_is_refused(void) {
    /* gz_corners_to_rect takes the width from the top edge and the origin from
     * bl, so a quad the daemon could never have written collapses into a
     * rectangle on neither edge. Here it collapses onto exactly the geometry
     * the config asks for, which is the whole danger: without the shape check
     * the gate reports OK on corners that are not a display area. */
    double skew[9] = { -298.5, 346, 0,  298.5, 346, 0,  -298.5, 10, 0 };
    skew[6] = -260.0;   /* bl.x pulled in: tl.x != bl.x */

    struct gz_rect r = gz_corners_to_rect(skew);
    assert(r.w_mm == 597.0);            /* the top edge still measures right */
    assert(r.ox_mm == -260.0);          /* but the origin is 38.5 mm off */
    assert(gz_corners_are_rectangular(skew, GZ_DA_TOL_MM) == 0);
    assert(gz_display_verify(skew, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG) == -1);

    /* A tilted top edge is the other way the shape can fail, and it is the one
     * that would otherwise be absorbed by the tilt the conversion recovers. */
    double lean[9] = { -298.5, 346, 0,  298.5, 352, 0,  -298.5, 10, 0 };
    assert(gz_corners_are_rectangular(lean, GZ_DA_TOL_MM) == 0);
    assert(gz_display_verify(lean, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG) == -1);

    double lean_z[9] = { -298.5, 346, 0,  298.5, 346, 9,  -298.5, 10, 0 };
    assert(gz_corners_are_rectangular(lean_z, GZ_DA_TOL_MM) == 0);

    /* The real device is rectangular, and a genuine tilt still is: the tilt
     * moves the whole top edge together, so tr stays level with tl. */
    double good[9];
    gz_rect_to_corners(real_panel, good);
    assert(gz_corners_are_rectangular(good, GZ_DA_TOL_MM) == 1);
    struct gz_rect tilted = { 597, 336, -298.5, 10, 25, -12.5 };
    gz_rect_to_corners(tilted, good);
    assert(gz_corners_are_rectangular(good, GZ_DA_TOL_MM) == 1);
}

static void test_the_gate_refuses_a_skewed_quad_on_the_wire(void) {
    double skew[9] = { -298.5, 346, 0,  298.5, 346, 0,  -260.0, 10, 0 };
    struct step s[1];
    unsigned char body[256];
    size_t n = build_da(body, skew);
    s[0].len = put_response(s[0].buf, GZ_CMD_GET_DISPLAY_AREA, body, n);

    struct fake f;
    fake_start(&f, 1, 0, GZ_PROTOCOL_VERSION, s, 1, 0);
    assert(gz_display_gate(&f.c, NULL, real_panel, GZ_DA_TOL_MM, GZ_DA_TOL_DEG)
           == GZ_GATE_MISMATCH);
    assert(fake_stop(&f) == 1);
}

int main(void) {
    test_config_matches_the_shipped_file();
    test_config_key_order_does_not_matter();
    test_config_ignores_unknown_keys_and_nesting();
    test_config_refuses_rather_than_defaulting();
    test_config_reproduces_the_absent_cx_trap();
    test_config_refuses_a_file_longer_than_the_daemon_reads();
    test_anchor_expr_grammar();
    test_config_accepts_a_valid_json_escape();

    test_verify_accepts_the_real_geometry_and_refuses_a_wrong_one();
    test_the_success_caveat_follows_the_verdict();

    test_gate_passes_on_a_matching_device();
    test_gate_refuses_a_mismatched_device();
    test_gate_refuses_an_unparseable_reply();
    test_gate_refuses_an_unknown_protocol_version();
    test_gate_refuses_when_the_device_is_absent();
    test_gate_refuses_without_a_status_frame();

    test_read_retries_usb_busy();
    test_read_gives_up_on_a_non_retryable_error();
    test_read_stops_retrying_usb_busy_eventually();
    test_read_does_not_retry_a_timeout_on_the_same_connection();
    test_read_reports_a_dead_link();
    test_gaze_traffic_does_not_disturb_the_gate();
    test_a_reply_to_another_command_is_not_the_answer();
    test_the_library_entry_prints_the_caveat();

    test_reconnect_regates_an_upgraded_daemon();
    test_reconnect_regates_a_daemon_that_lost_its_device();
    test_reconnect_succeeds_against_the_same_daemon();
    test_the_shipped_path_refuses_a_mismatch();

    test_a_skewed_quad_is_refused();
    test_the_gate_refuses_a_skewed_quad_on_the_wire();

    printf("all display tests passed\n");
    return 0;
}
