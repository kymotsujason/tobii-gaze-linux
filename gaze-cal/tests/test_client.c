/* gaze-cal/tests/test_client.c
 *
 * The connection layer. Everything here runs without hardware and without a
 * daemon: the socket-facing paths are driven over a socketpair, so a test can
 * be both a real fd and a byte-exact assertion about what went on the wire.
 * The live-daemon coverage is tests/test_client_live.c.
 *
 * Expectations trace to vendor/tobiifree/driver/src/daemon_protocol.zig and to
 * vendor/tobiifree/applications/tobiifreed/src/server.zig. ARCHITECTURE.md is
 * wrong about the opcodes and is never cited.
 */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/client.h"

#ifdef NDEBUG
#error "test_client.c relies on assert(); do not build it with NDEBUG"
#endif

/* ---------- frame builders ---------- */

static size_t put_hdr(unsigned char *b, uint8_t type, uint32_t len) {
    b[0] = type;
    b[1] = (unsigned char)(len & 0xFFu);
    b[2] = (unsigned char)((len >> 8) & 0xFFu);
    b[3] = (unsigned char)((len >> 16) & 0xFFu);
    b[4] = (unsigned char)((len >> 24) & 0xFFu);
    return 5;
}

static size_t put_gaze(unsigned char *b, const struct gz_gaze_sample *s) {
    size_t n = put_hdr(b, GZ_SRV_GAZE, (uint32_t)sizeof *s);
    memcpy(b + n, s, sizeof *s);
    return n + sizeof *s;
}

static size_t put_status(unsigned char *b, uint8_t present, uint8_t cal, uint8_t ver) {
    size_t n = put_hdr(b, GZ_SRV_STATUS, GZ_STATUS_SIZE);
    b[n] = present; b[n + 1] = cal; b[n + 2] = ver;
    return n + GZ_STATUS_SIZE;
}

static size_t put_err(unsigned char *b, uint32_t code) {
    size_t n = put_hdr(b, GZ_SRV_ERR, 4);
    b[n] = (unsigned char)(code & 0xFFu);
    b[n + 1] = (unsigned char)((code >> 8) & 0xFFu);
    b[n + 2] = (unsigned char)((code >> 16) & 0xFFu);
    b[n + 3] = (unsigned char)((code >> 24) & 0xFFu);
    return n + 4;
}

static size_t put_response(unsigned char *b, uint8_t cmd, const void *body, size_t body_len) {
    size_t n = put_hdr(b, GZ_SRV_RESPONSE, (uint32_t)(body_len + 1));
    b[n] = cmd;
    if (body_len > 0) memcpy(b + n + 1, body, body_len);
    return n + 1 + body_len;
}

static struct gz_gaze_sample mk_sample(uint32_t counter, double x, double y) {
    struct gz_gaze_sample s;
    memset(&s, 0, sizeof s);
    s.present_mask = GZ_BIT_FRAME_COUNTER | GZ_BIT_GAZE_2D | GZ_BIT_VALIDITY_L | GZ_BIT_VALIDITY_R;
    s.frame_counter = counter;
    s.validity_L = GZ_VALIDITY_VALID;
    s.validity_R = GZ_VALIDITY_VALID;
    s.gaze_point_2d_norm[0] = x;
    s.gaze_point_2d_norm[1] = y;
    return s;
}

/* ---------- the brief's four tests, unchanged in intent ---------- */

static void test_subscribe_is_first_bytes_out(void) {
    struct gz_client c; gz_client_init(&c);
    unsigned char out[16];
    size_t n = gz_client_take_outbound(&c, out, sizeof out);
    assert(n == 5 && out[0] == GZ_CMD_SUBSCRIBE);
    assert(out[1] == 0 && out[2] == 0 && out[3] == 0 && out[4] == 0);
    /* Drained exactly once: a second take must not replay it. */
    assert(gz_client_take_outbound(&c, out, sizeof out) == 0);
}

static void test_partial_frame_is_buffered(void) {
    struct gz_client c; gz_client_init(&c);
    unsigned char half[100] = {GZ_SRV_GAZE, 0x88, 0x01, 0, 0};   /* 392 payload */
    assert(gz_client_feed(&c, half, sizeof half) == 0);          /* 0 frames yet */
    /* Buffered, not discarded. The distinction is the whole point of the
     * parser returning 0 rather than an error. */
    assert(c.in_len == sizeof half);
    assert(c.link_broken == 0);
}

static void test_desync_requests_reconnect(void) {
    struct gz_client c; gz_client_init(&c);
    unsigned char bad[5] = {0x77, 0xFF, 0xFF, 0xFF, 0xFF};
    assert(gz_client_feed(&c, bad, sizeof bad) == GZ_CLIENT_RECONNECT);
}

static void test_watchdog_fires_without_gaze(void) {
    struct gz_client c; gz_client_init(&c);
    c.last_gaze_ns = 0;
    assert(gz_client_watchdog(&c, 1500000000ULL) == GZ_CLIENT_RECONNECT);
    assert(gz_client_watchdog(&c, 500000000ULL) == 0);
}

/* ---------- incomplete versus desync, kept apart ---------- */

static void test_incomplete_then_completed(void) {
    /* The parser's 0 and its -1 mean opposite things. A client that folds them
     * into `r <= 0` either reconnects on every torn read or spins on garbage. */
    struct gz_client c; gz_client_init(&c);
    struct gz_gaze_sample s = mk_sample(8, 0.25, 0.75);
    unsigned char frame[512];
    size_t n = put_gaze(frame, &s);
    assert(n == 397);

    for (size_t split = 1; split < n; split += 37) {
        gz_client_init(&c);
        assert(gz_client_feed(&c, frame, split) == 0);
        assert(c.link_broken == 0);
        assert(c.have_latest == 0);
        assert(gz_client_feed(&c, frame + split, n - split) == 1);
        assert(c.have_latest == 1);
        assert(c.latest.frame_counter == 8);
        assert(c.latest.gaze_point_2d_norm[0] == 0.25);
        assert(c.in_len == 0);
    }
}

static void test_byte_at_a_time_delivery(void) {
    /* The worst case a stream reader can be handed. */
    struct gz_client c; gz_client_init(&c);
    struct gz_gaze_sample s = mk_sample(12, 0.5, 0.5);
    unsigned char frame[512];
    size_t n = put_gaze(frame, &s);
    int frames = 0;
    for (size_t i = 0; i < n; i++) {
        int r = gz_client_feed(&c, frame + i, 1);
        assert(r >= 0);
        frames += r;
    }
    assert(frames == 1);
    assert(c.latest.frame_counter == 12);
}

static void test_desync_is_sticky(void) {
    /* After a desync nothing in the stream can be trusted, so a caller that
     * ignores the first GZ_CLIENT_RECONNECT must not be handed a "recovery"
     * that is really the parser locking onto a random byte offset. */
    struct gz_client c; gz_client_init(&c);
    unsigned char bad[5] = {0x77, 0, 0, 0, 0};
    assert(gz_client_feed(&c, bad, sizeof bad) == GZ_CLIENT_RECONNECT);
    assert(c.link_broken == 1);

    struct gz_gaze_sample s = mk_sample(4, 0.1, 0.2);
    unsigned char good[512];
    size_t n = put_gaze(good, &s);
    assert(gz_client_feed(&c, good, n) == GZ_CLIENT_RECONNECT);
    assert(c.have_latest == 0);
}

static void test_oversize_length_is_desync_not_wait(void) {
    /* 8194..0xFFFFFFFF is now GZ_ERR_DESYNC rather than "need more bytes", so
     * a client cannot be stalled forever by four bytes of garbage. */
    struct gz_client c; gz_client_init(&c);
    unsigned char hdr[5];
    put_hdr(hdr, GZ_SRV_RESPONSE, GZ_MAX_PAYLOAD + 1);
    assert(gz_client_feed(&c, hdr, sizeof hdr) == GZ_CLIENT_RECONNECT);

    gz_client_init(&c);
    put_hdr(hdr, GZ_SRV_RESPONSE, 0xFFFFFFFFu);
    assert(gz_client_feed(&c, hdr, sizeof hdr) == GZ_CLIENT_RECONNECT);
}

/* ---------- the body pointer must never outlive compaction ---------- */

static void test_body_is_copied_not_referenced(void) {
    /* gz_frame_parse hands back a pointer INTO the accumulator. Anything the
     * client keeps has to be a copy, because the next feed memmoves the bytes
     * out from under it. Scribbling over the accumulator afterwards is the
     * direct proof: a stored pointer would show 0xFF here. */
    struct gz_client c; gz_client_init(&c);
    unsigned char buf[2048];
    size_t n = 0;
    struct gz_gaze_sample s = mk_sample(16, 0.125, 0.875);
    n += put_status(buf + n, 1, 1, GZ_PROTOCOL_VERSION);
    n += put_response(buf + n, GZ_CMD_GET_DISPLAY_AREA, "geometry", 8);
    n += put_gaze(buf + n, &s);

    assert(gz_client_feed(&c, buf, n) == 1);
    assert(c.in_len == 0);

    memset(c.in, 0xFF, sizeof c.in);

    assert(c.have_status == 1);
    assert(c.status.device_present == 1 && c.status.calibration_applied == 1);
    assert(c.status.protocol_version == GZ_PROTOCOL_VERSION);
    assert(c.have_response == 1 && c.resp_cmd == GZ_CMD_GET_DISPLAY_AREA);
    assert(c.resp_len == 8 && memcmp(c.resp, "geometry", 8) == 0);
    assert(c.latest.frame_counter == 16);
    assert(c.latest.gaze_point_2d_norm[0] == 0.125);
    assert(c.latest.gaze_point_2d_norm[1] == 0.875);
}

static void test_state_survives_compaction_of_a_partial_tail(void) {
    /* Two whole frames plus a torn third, so the compaction at the end of feed
     * actually moves bytes rather than resetting an emptied buffer. */
    struct gz_client c; gz_client_init(&c);
    unsigned char buf[2048];
    struct gz_gaze_sample a = mk_sample(4, 0.1, 0.2);
    struct gz_gaze_sample b = mk_sample(8, 0.3, 0.4);
    struct gz_gaze_sample d = mk_sample(12, 0.6, 0.7);
    size_t n = 0;
    n += put_gaze(buf + n, &a);
    n += put_gaze(buf + n, &b);
    size_t whole = n;
    n += put_gaze(buf + n, &d);

    assert(gz_client_feed(&c, buf, whole + 40) == 2);
    assert(c.in_len == 40);
    assert(c.latest.frame_counter == 8);
    assert(c.latest.gaze_point_2d_norm[0] == 0.3);

    assert(gz_client_feed(&c, buf + whole + 40, n - whole - 40) == 1);
    assert(c.latest.frame_counter == 12);
    assert(c.latest.gaze_point_2d_norm[1] == 0.7);
    assert(c.in_len == 0);
}

static void test_feed_larger_than_the_accumulator(void) {
    /* 200 gaze frames is 79400 bytes, several times the accumulator. A reader
     * that refuses rather than draining would report a false desync. */
    struct gz_client c; gz_client_init(&c);
    size_t count = 200;
    size_t frame_len = GZ_HEADER_SIZE + sizeof(struct gz_gaze_sample);
    unsigned char *buf = malloc(count * frame_len);
    assert(buf != NULL);
    assert(count * frame_len > sizeof c.in);
    for (size_t i = 0; i < count; i++) {
        struct gz_gaze_sample s = mk_sample((uint32_t)(4 * (i + 1)), 0.5, 0.5);
        put_gaze(buf + i * frame_len, &s);
    }
    assert(gz_client_feed(&c, buf, count * frame_len) == (int)count);
    assert(c.latest.frame_counter == (uint32_t)(4 * count));
    assert(c.dropped == 0);
    free(buf);
}

/* ---------- gap detection ---------- */

static void test_gap_detection_counts_missing_samples(void) {
    struct gz_client c; gz_client_init(&c);
    unsigned char buf[2048];
    size_t n = 0;
    struct gz_gaze_sample a = mk_sample(100, 0, 0);
    struct gz_gaze_sample b = mk_sample(104, 0, 0);   /* the next one, no gap */
    struct gz_gaze_sample d = mk_sample(120, 0, 0);   /* 3 samples missing */
    n += put_gaze(buf + n, &a);
    n += put_gaze(buf + n, &b);
    n += put_gaze(buf + n, &d);
    assert(gz_client_feed(&c, buf, n) == 3);
    assert(c.dropped == 3);
    assert(c.gaze_frames == 3);
}

static void test_reconnect_resets_gap_detection(void) {
    /* The counter does NOT restart with the daemon: measured across a 15 s
     * kill and restart it ran 530932 to 531802, monotonic. The reset matters
     * for the opposite reason, which is that the counter keeps advancing while
     * the client is away, so carrying prev_counter charges the whole absence
     * to dropped. The numbers below are that measured reconnect: an 870-count
     * gap, which is 217 samples that would otherwise be reported as lost. */
    int sv[2], sv2[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct gz_client c;
    assert(gz_client_adopt(&c, sv[0]) == 0);

    unsigned char buf[1024];
    struct gz_gaze_sample a = mk_sample(530932, 0, 0);
    size_t n = put_gaze(buf, &a);
    assert(gz_client_feed(&c, buf, n) == 1);
    assert(c.dropped == 0);
    assert(c.have_prev_counter == 1);

    gz_client_close(&c);
    close(sv[1]);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv2) == 0);
    assert(gz_client_adopt(&c, sv2[0]) == 0);
    assert(c.have_prev_counter == 0);
    assert(c.dropped == 0);

    struct gz_gaze_sample b = mk_sample(531802, 0, 0);
    n = put_gaze(buf, &b);
    assert(gz_client_feed(&c, buf, n) == 1);
    assert(c.dropped == 0);          /* not 217 */
    assert(c.gaze_frames == 1);      /* per-connection, reset with the rest */

    /* The same reset also covers a counter that did go backwards, which is not
     * observed today but would report a quarter-billion drops if it happened. */
    gz_client_close(&c);
    close(sv2[1]);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(gz_client_adopt(&c, sv[0]) == 0);
    struct gz_gaze_sample d = mk_sample(4, 0, 0);
    n = put_gaze(buf, &d);
    assert(gz_client_feed(&c, buf, n) == 1);
    assert(c.dropped == 0);

    gz_client_close(&c);
    close(sv[1]);
}

/* ---------- subscribe on connect and on every reconnect ---------- */

static void read_exact(int fd, unsigned char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        assert(r > 0);
        got += (size_t)r;
    }
}

static void test_subscribe_is_sent_on_connect(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct gz_client c;
    assert(gz_client_adopt(&c, sv[0]) == 0);

    unsigned char got[5];
    read_exact(sv[1], got, sizeof got);
    assert(got[0] == GZ_CMD_SUBSCRIBE);
    assert(got[1] == 0 && got[2] == 0 && got[3] == 0 && got[4] == 0);

    gz_client_close(&c);
    close(sv[1]);
}

static void test_subscribe_is_sent_again_on_reconnect(void) {
    /* The single most common way to make this daemon look broken: a client
     * that subscribes once, reconnects, and then waits forever for samples the
     * daemon was never asked to send. There is no error frame for it. */
    int sv[2], sv2[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv2) == 0);

    struct gz_client c;
    assert(gz_client_adopt(&c, sv[0]) == 0);
    unsigned char got[5];
    read_exact(sv[1], got, sizeof got);
    assert(got[0] == GZ_CMD_SUBSCRIBE);
    close(sv[1]);

    assert(gz_client_adopt(&c, sv2[0]) == 0);
    memset(got, 0, sizeof got);
    read_exact(sv2[1], got, sizeof got);
    assert(got[0] == GZ_CMD_SUBSCRIBE);
    assert(got[1] == 0 && got[2] == 0 && got[3] == 0 && got[4] == 0);

    gz_client_close(&c);
    close(sv2[1]);
}

/* ---------- err frames ---------- */

static void test_err_codes(void) {
    struct gz_client c;
    unsigned char buf[64];
    size_t n;

    /* failed = 1: the command ran and did not work. Retrying is pointless. */
    gz_client_init(&c);
    n = put_err(buf, GZ_ERRCODE_FAILED);
    assert(gz_client_feed(&c, buf, n) == 0);
    assert(c.have_error == 1 && c.err_code == GZ_ERRCODE_FAILED);
    assert(gz_err_retryable(c.err_code) == 0);
    assert(c.link_broken == 0);      /* an err is an answer, not a desync */

    /* usb_busy = 2: nothing reached the device, so the same request may work. */
    gz_client_init(&c);
    n = put_err(buf, GZ_ERRCODE_USB_BUSY);
    assert(gz_client_feed(&c, buf, n) == 0);
    assert(c.have_error == 1 && c.err_code == GZ_ERRCODE_USB_BUSY);
    assert(gz_err_retryable(c.err_code) == 1);

    /* The Zig enum is non-exhaustive. An unknown code must be surfaced and
     * treated as a failure, never as a parse error and never as retryable. */
    gz_client_init(&c);
    n = put_err(buf, 4242);
    assert(gz_client_feed(&c, buf, n) == 0);
    assert(c.have_error == 1 && c.err_code == 4242);
    assert(gz_err_retryable(c.err_code) == 0);
    assert(c.link_broken == 0);
}

static void test_request_reports_err_frames(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct gz_client c;
    assert(gz_client_adopt(&c, sv[0]) == 0);
    unsigned char sub[5];
    read_exact(sv[1], sub, sizeof sub);

    unsigned char buf[64];
    size_t n = put_err(buf, GZ_ERRCODE_USB_BUSY);
    assert(write(sv[1], buf, n) == (ssize_t)n);

    int r = gz_client_request(&c, GZ_CMD_GET_DISPLAY_AREA, NULL, 0, 500);
    assert(r == GZ_CLIENT_REMOTE);
    assert(c.err_code == GZ_ERRCODE_USB_BUSY);
    assert(gz_err_retryable(c.err_code) == 1);

    /* The command still went out, byte for byte. */
    unsigned char cmd[5];
    read_exact(sv[1], cmd, sizeof cmd);
    assert(cmd[0] == GZ_CMD_GET_DISPLAY_AREA);

    gz_client_close(&c);
    close(sv[1]);
}

/* ---------- socket paths ---------- */

static void test_request_returns_the_matching_response(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct gz_client c;
    assert(gz_client_adopt(&c, sv[0]) == 0);
    unsigned char sub[5];
    read_exact(sv[1], sub, sizeof sub);

    /* A gaze frame and an unrelated response arrive first. Neither may be
     * mistaken for the answer, and the gaze must still be delivered. */
    unsigned char buf[2048];
    size_t n = 0;
    struct gz_gaze_sample s = mk_sample(4, 0.9, 0.1);
    n += put_gaze(buf + n, &s);
    n += put_response(buf + n, GZ_CMD_START_CAL, "\x01", 1);
    n += put_response(buf + n, GZ_CMD_GET_DISPLAY_AREA, "TTPblob", 7);
    assert(write(sv[1], buf, n) == (ssize_t)n);

    int r = gz_client_request(&c, GZ_CMD_GET_DISPLAY_AREA, NULL, 0, 500);
    assert(r == 0);
    assert(c.resp_cmd == GZ_CMD_GET_DISPLAY_AREA);
    assert(c.resp_len == 7 && memcmp(c.resp, "TTPblob", 7) == 0);
    assert(c.resp_mismatch == 1);
    assert(c.have_latest == 1 && c.latest.frame_counter == 4);

    gz_client_close(&c);
    close(sv[1]);
}

static void test_request_ignores_a_reply_to_another_command(void) {
    /* A reply to a command that already timed out must not be handed back as
     * the answer to this one. The daemon answers in order but a client that
     * gave up once is permanently one reply ahead otherwise. */
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct gz_client c;
    assert(gz_client_adopt(&c, sv[0]) == 0);
    unsigned char sub[5];
    read_exact(sv[1], sub, sizeof sub);

    unsigned char buf[64];
    size_t n = put_response(buf, GZ_CMD_START_CAL, "\x01", 1);
    assert(write(sv[1], buf, n) == (ssize_t)n);

    int r = gz_client_request(&c, GZ_CMD_GET_DISPLAY_AREA, NULL, 0, 150);
    assert(r == GZ_CLIENT_TIMEOUT);
    assert(c.resp_mismatch == 1);

    gz_client_close(&c);
    close(sv[1]);
}

static void test_adopt_does_not_leak_the_fd_it_owns(void) {
    /* The contract is "-1 with errno set and fd left at -1". A peer that is
     * already gone makes the subscribe send take EPIPE, which is reachable
     * rather than theoretical: server.zig:211-213 accepts a client past its
     * limit and closes it at once, so connect() succeeds and this send fails.
     * main.c retries every 250 ms, so a kept descriptor is one leak per
     * attempt, inside OBS in Plan 2. */
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    close(sv[1]);

    struct gz_client c;
    int rc = gz_client_adopt(&c, sv[0]);
    assert(rc == -1);
    assert(c.fd == -1);
    assert(fcntl(sv[0], F_GETFD) == -1 && errno == EBADF);   /* really closed */

    /* And the client is left reusable, not half-open. */
    int sw[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sw) == 0);
    assert(gz_client_adopt(&c, sw[0]) == 0);
    unsigned char sub[5];
    read_exact(sw[1], sub, sizeof sub);
    assert(sub[0] == GZ_CMD_SUBSCRIBE);
    gz_client_close(&c);
    close(sw[1]);
}

static void test_adopt_sets_nonblocking(void) {
    /* A blocking fd turns gz_client_poll into an unbounded wait the moment a
     * read fills the accumulator exactly, and turns a backed-up flush into a
     * stalled render thread. */
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct gz_client c;
    assert(gz_client_adopt(&c, sv[0]) == 0);
    int flags = fcntl(c.fd, F_GETFL, 0);
    assert(flags >= 0);
    assert((flags & O_NONBLOCK) != 0);
    gz_client_close(&c);
    close(sv[1]);
}

static void test_request_times_out_without_a_reply(void) {
    /* The daemon's guarantee is a reply within about 5 s or never, so a
     * blocking read is a hang. A timeout must be an ordinary return value. */
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct gz_client c;
    assert(gz_client_adopt(&c, sv[0]) == 0);
    unsigned char sub[5];
    read_exact(sv[1], sub, sizeof sub);

    uint64_t t0 = gz_now_ns();
    int r = gz_client_request(&c, GZ_CMD_GET_DISPLAY_AREA, NULL, 0, 120);
    uint64_t dt = gz_now_ns() - t0;
    assert(r == GZ_CLIENT_TIMEOUT);
    assert(dt >= 100000000ULL);       /* it really waited */
    assert(dt < 3000000000ULL);       /* and it really stopped */
    assert(c.link_broken == 0);       /* a timeout is not a desync */

    gz_client_close(&c);
    close(sv[1]);
}

static void test_poll_reports_a_clean_eof_as_reconnect(void) {
    /* The peer drains our subscribe before closing, so poll reports
     * POLLIN|POLLHUP with no POLLERR and the only thing that can detect the
     * close is recv returning 0. Measured: a peer that closes with our bytes
     * still unread raises POLLERR too, which answers first and leaves this
     * path untested. */
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct gz_client c;
    assert(gz_client_adopt(&c, sv[0]) == 0);
    unsigned char sub[5];
    read_exact(sv[1], sub, sizeof sub);
    close(sv[1]);
    assert(gz_client_poll(&c, 100) == GZ_CLIENT_RECONNECT);
    assert(c.link_broken == 1);
    gz_client_close(&c);
}

static void test_poll_reports_an_errored_peer_as_reconnect(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct gz_client c;
    assert(gz_client_adopt(&c, sv[0]) == 0);
    close(sv[1]);                        /* our subscribe still unread */
    assert(gz_client_poll(&c, 100) == GZ_CLIENT_RECONNECT);
    assert(c.link_broken == 1);
    gz_client_close(&c);
}

static void test_a_broken_link_stops_accepting_data(void) {
    /* link_broken is set by a write failure as well as by a desync, and in
     * that case the accumulator is empty and clean bytes would otherwise parse
     * happily onto a connection that is already gone. */
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct gz_client c;
    assert(gz_client_adopt(&c, sv[0]) == 0);
    close(sv[1]);

    int r = 0;
    for (int i = 0; i < 8 && r == 0; i++) {
        r = gz_client_send(&c, GZ_CMD_GET_DISPLAY_AREA, NULL, 0);
    }
    assert(r == GZ_CLIENT_RECONNECT);
    assert(c.link_broken == 1);
    assert(c.in_len == 0);

    struct gz_gaze_sample s = mk_sample(4, 0.5, 0.5);
    unsigned char buf[512];
    size_t n = put_gaze(buf, &s);
    assert(gz_client_feed(&c, buf, n) == GZ_CLIENT_RECONNECT);
    assert(c.have_latest == 0);
    assert(gz_client_watchdog(&c, gz_now_ns()) == GZ_CLIENT_RECONNECT);

    gz_client_close(&c);
}

static void test_poll_delivers_gaze_over_a_socket(void) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct gz_client c;
    assert(gz_client_adopt(&c, sv[0]) == 0);
    unsigned char sub[5];
    read_exact(sv[1], sub, sizeof sub);

    unsigned char buf[2048];
    size_t n = 0;
    n += put_status(buf + n, 1, 0, GZ_PROTOCOL_VERSION);
    struct gz_gaze_sample s = mk_sample(20, 0.4, 0.6);
    n += put_gaze(buf + n, &s);
    assert(write(sv[1], buf, n) == (ssize_t)n);

    int total = 0;
    for (int i = 0; i < 20 && total == 0; i++) {
        int r = gz_client_poll(&c, 100);
        assert(r >= 0);
        total += r;
    }
    assert(total == 1);
    assert(c.have_status == 1 && c.status.device_present == 1);
    assert(c.latest.frame_counter == 20);
    assert(c.last_gaze_ns > 0);

    gz_client_close(&c);
    close(sv[1]);
}

static void test_send_does_not_die_on_a_closed_peer(void) {
    /* Without MSG_NOSIGNAL this raises SIGPIPE and takes the process with it,
     * which in Plan 2 is OBS. */
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct gz_client c;
    assert(gz_client_adopt(&c, sv[0]) == 0);
    close(sv[1]);

    int r = 0;
    for (int i = 0; i < 8 && r == 0; i++) {
        r = gz_client_send(&c, GZ_CMD_GET_DISPLAY_AREA, NULL, 0);
    }
    assert(r == GZ_CLIENT_RECONNECT);
    gz_client_close(&c);
}

/* ---------- watchdog ---------- */

static void test_watchdog_is_armed_by_any_gaze_frame(void) {
    /* Not by a VALID one. The device streams at 33.2 Hz whether or not it sees
     * eyes, so arming on validity would reconnect every time the user looks
     * away, and the reconnect would not help. */
    struct gz_client c; gz_client_init(&c);
    struct gz_gaze_sample s = mk_sample(4, 0, 0);
    s.validity_L = GZ_VALIDITY_NOT_DETECTED;
    s.validity_R = GZ_VALIDITY_NOT_DETECTED;
    unsigned char buf[512];
    size_t n = put_gaze(buf, &s);
    assert(gz_client_feed_at(&c, buf, n, 10000000000ULL) == 1);
    assert(c.last_gaze_ns == 10000000000ULL);
    assert(c.last_valid_gaze_ns == 0);
    assert(gz_client_watchdog(&c, 10500000000ULL) == GZ_LINK_OK);

    struct gz_gaze_sample v = mk_sample(8, 0.2, 0.3);
    n = put_gaze(buf, &v);
    assert(gz_client_feed_at(&c, buf, n, 11000000000ULL) == 1);
    assert(c.last_valid_gaze_ns == 11000000000ULL);
}

static void test_watchdog_reports_stale_when_the_device_is_gone(void) {
    /* Reconnecting to a healthy daemon cannot bring back an unplugged tracker.
     * Spec section 11 needs these two told apart, and the status frame is the
     * only thing that can tell them apart. */
    struct gz_client c; gz_client_init(&c);
    unsigned char buf[64];
    size_t n = put_status(buf, 0, 0, GZ_PROTOCOL_VERSION);
    assert(gz_client_feed_at(&c, buf, n, 5000000000ULL) == 0);
    assert(c.have_status == 1 && c.status.device_present == 0);
    assert(gz_client_watchdog(&c, 7000000000ULL) == GZ_LINK_STALE);

    /* Same silence, device reported present: that is a broken stream. */
    n = put_status(buf, 1, 0, GZ_PROTOCOL_VERSION);
    assert(gz_client_feed_at(&c, buf, n, 7000000000ULL) == 0);
    assert(gz_client_watchdog(&c, 9000000000ULL) == GZ_CLIENT_RECONNECT);
}

static void test_a_returning_device_rearms_the_watchdog(void) {
    /* Found against real hardware: a 12 s hold_tobii outage, ridden out
     * correctly as stale, ended in a reconnect the instant the daemon
     * announced the tracker was back, because the status frame arrives before
     * the first gaze frame of the new session and 13.4 s of outage silence was
     * still on the clock. The outage is not the link's fault, so the device
     * gets a fresh window to resume streaming. */
    struct gz_client c; gz_client_init(&c);
    unsigned char buf[64];
    size_t n = put_status(buf, 0, 0, GZ_PROTOCOL_VERSION);
    assert(gz_client_feed_at(&c, buf, n, 5000000000ULL) == 0);
    assert(gz_client_watchdog(&c, 17000000000ULL) == GZ_LINK_STALE);

    n = put_status(buf, 1, 0, GZ_PROTOCOL_VERSION);
    assert(gz_client_feed_at(&c, buf, n, 18000000000ULL) == 0);
    assert(c.last_gaze_ns == 18000000000ULL);
    assert(gz_client_watchdog(&c, 18500000000ULL) == GZ_LINK_OK);

    /* The reprieve is one watchdog window, not indefinite. */
    assert(gz_client_watchdog(&c, 19500000000ULL) == GZ_CLIENT_RECONNECT);

    /* A repeated present=1 status is not a transition and must not re-arm,
     * or a chatty daemon would suppress the watchdog entirely. */
    n = put_status(buf, 1, 1, GZ_PROTOCOL_VERSION);
    assert(gz_client_feed_at(&c, buf, n, 19500000000ULL) == 0);
    assert(c.last_gaze_ns == 18000000000ULL);
    assert(gz_client_watchdog(&c, 19500000000ULL) == GZ_CLIENT_RECONNECT);
}

static void test_watchdog_interval_is_a_parameter(void) {
    /* Every forwarded command parks the daemon's USB thread, so gaze stops for
     * every subscribed client while some other client's batch runs. A client
     * that shares a daemon with a command issuer needs a longer interval than
     * the overlay does, and a reconnect must not quietly restore the default
     * under it, which is why the interval is an argument and not state. */
    struct gz_client c; gz_client_init(&c);
    c.last_gaze_ns = 10000000000ULL;

    /* 2.5 s of silence: over the 1 s default, under a 4 s caller interval. */
    assert(gz_client_watchdog(&c, 12500000000ULL) == GZ_CLIENT_RECONNECT);
    assert(gz_client_watchdog_for(&c, 12500000000ULL, 4000000000ULL) == GZ_LINK_OK);
    assert(gz_client_watchdog_for(&c, 15000000000ULL, 4000000000ULL) == GZ_CLIENT_RECONNECT);

    /* The default really is GZ_WATCHDOG_NS and not a second constant. */
    assert(gz_client_watchdog(&c, 10000000000ULL + GZ_WATCHDOG_NS) == GZ_LINK_OK);
    assert(gz_client_watchdog(&c, 10000000000ULL + GZ_WATCHDOG_NS + 1) == GZ_CLIENT_RECONNECT);

    /* A caller interval does not override the stale branch: an absent device
     * is still stale rather than a broken link. */
    unsigned char buf[64];
    size_t n = put_status(buf, 0, 0, GZ_PROTOCOL_VERSION);
    assert(gz_client_feed_at(&c, buf, n, 10000000000ULL) == 0);
    assert(gz_client_watchdog_for(&c, 20000000000ULL, 4000000000ULL) == GZ_LINK_STALE);
}

static void test_watchdog_tolerates_a_backwards_clock(void) {
    struct gz_client c; gz_client_init(&c);
    c.last_gaze_ns = 5000000000ULL;
    /* Unsigned subtraction would wrap to ~1.8e19 and fire instantly. */
    assert(gz_client_watchdog(&c, 4000000000ULL) == GZ_LINK_OK);
    assert(gz_client_watchdog(&c, 5000000000ULL) == GZ_LINK_OK);
}

static void test_watchdog_is_seeded_at_connect(void) {
    /* CLOCK_MONOTONIC is seconds-since-boot, so a client that leaves
     * last_gaze_ns at zero fires the watchdog on its first tick. */
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    struct gz_client c;
    assert(gz_client_adopt(&c, sv[0]) == 0);
    assert(c.last_gaze_ns > 0);
    assert(gz_client_watchdog(&c, gz_now_ns()) == GZ_LINK_OK);
    gz_client_close(&c);
    close(sv[1]);
}

/* ---------- misc ---------- */

static void test_socket_path_follows_xdg_runtime_dir(void) {
    /* daemon_protocol.zig socketPath(): $XDG_RUNTIME_DIR or /tmp, then
     * /tobiifreed/gaze.sock. Mirrored rather than hardcoded to /run/user/1000
     * so the client and the daemon cannot disagree. */
    char buf[512];
    assert(setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1) == 0);
    assert(gz_socket_path(buf, sizeof buf) == 0);
    assert(strcmp(buf, "/run/user/1000/tobiifreed/gaze.sock") == 0);

    assert(unsetenv("XDG_RUNTIME_DIR") == 0);
    assert(gz_socket_path(buf, sizeof buf) == 0);
    assert(strcmp(buf, "/tmp/tobiifreed/gaze.sock") == 0);

    assert(gz_socket_path(buf, 8) == -1);
}

static void test_connect_rejects_an_overlong_path(void) {
    /* strncpy into sun_path truncates silently, which connects to a different
     * socket than the one that was asked for. */
    struct gz_client c;
    char path[512];
    memset(path, 'a', sizeof path - 1);
    path[0] = '/';
    path[sizeof path - 1] = '\0';
    assert(gz_client_connect(&c, path) == -1);
    assert(errno == ENAMETOOLONG);
    assert(c.fd == -1);
}

static void test_connect_does_not_close_a_stale_fd(void) {
    /* A freshly declared struct gz_client holds whatever was on the stack, and
     * an fd-shaped garbage value names a live descriptor belonging to someone
     * else. In Plan 2 that someone is OBS. */
    int keep = dup(STDOUT_FILENO);
    assert(keep >= 0);

    struct gz_client c;
    memset(&c, 0xAB, sizeof c);
    c.fd = keep;
    assert(gz_client_connect(&c, "/tmp/gaze-cal-no-such-socket-11b") == -1);
    assert(fcntl(keep, F_GETFD) != -1);      /* not closed behind our back */
    assert(c.fd == -1);

    /* The path that is allowed to close it is the one that says so. */
    c.fd = keep;
    assert(gz_client_reconnect(&c, "/tmp/gaze-cal-no-such-socket-11b") == -1);
    assert(fcntl(keep, F_GETFD) == -1 && errno == EBADF);
}

static void test_connect_failure_leaves_no_fd(void) {
    struct gz_client c;
    assert(gz_client_connect(&c, "/tmp/gaze-cal-no-such-socket-11") == -1);
    assert(c.fd == -1);
    gz_client_close(&c);            /* must be safe on an unconnected client */
}

static void test_send_rejects_an_oversize_payload(void) {
    /* The bound here is the protocol's, not the device's 4096-byte blob: this
     * layer stays generic and Task 13 owns what a calibration blob may be. */
    struct gz_client c; gz_client_init(&c);
    static unsigned char big[GZ_MAX_PAYLOAD + 1];
    assert(gz_client_send(&c, GZ_CMD_CAL_APPLY, big, sizeof big) == -1);
    assert(gz_client_send(&c, GZ_CMD_CAL_APPLY, NULL, 4) == -1);
}

static void test_send_reports_a_backed_up_queue(void) {
    /* A legal payload that does not fit behind the queued subscribe is
     * transient, not a caller bug, and the two get different answers. */
    struct gz_client c; gz_client_init(&c);
    static unsigned char big[GZ_MAX_PAYLOAD];
    assert(gz_client_send(&c, GZ_CMD_CAL_APPLY, big, sizeof big) == GZ_CLIENT_FULL);
    unsigned char sink[8];
    assert(gz_client_take_outbound(&c, sink, sizeof sink) == 5);
    assert(gz_client_send(&c, GZ_CMD_CAL_APPLY, big, sizeof big) == 0);
}

static void test_outbound_holds_a_full_calibration_blob(void) {
    /* cal_apply carries up to 4096 bytes, so the command is 4101 on the wire.
     * An outbound buffer sized 4096 is five bytes short of the largest thing
     * this client has to send, and Task 13 is the one that finds out. */
    struct gz_client c; gz_client_init(&c);
    static unsigned char blob[GZ_CAL_BLOB_MAX];
    memset(blob, 0x5A, sizeof blob);
    unsigned char sink[64];
    assert(gz_client_take_outbound(&c, sink, sizeof sink) == 5);   /* the subscribe */
    assert(gz_client_send(&c, GZ_CMD_CAL_APPLY, blob, sizeof blob) == 0);
    assert(c.out_len - c.out_off == GZ_HEADER_SIZE + GZ_CAL_BLOB_MAX);
    assert(c.out[c.out_off] == GZ_CMD_CAL_APPLY);
}

static void test_take_outbound_handles_a_small_buffer(void) {
    struct gz_client c; gz_client_init(&c);
    unsigned char one[1];
    for (int i = 0; i < 5; i++) {
        assert(gz_client_take_outbound(&c, one, 1) == 1);
    }
    assert(gz_client_take_outbound(&c, one, 1) == 0);
}

static void test_display_area_frame_is_consumed_not_fatal(void) {
    /* The daemon never emits 0x03, but proto.c consumes it by length rather
     * than rejecting it. The client must not turn that into a reconnect. */
    struct gz_client c; gz_client_init(&c);
    unsigned char buf[512];
    size_t n = put_hdr(buf, GZ_SRV_DISPLAY_AREA, 4);
    memset(buf + n, 0, 4);
    n += 4;
    struct gz_gaze_sample s = mk_sample(4, 0.5, 0.5);
    n += put_gaze(buf + n, &s);
    assert(gz_client_feed(&c, buf, n) == 1);
    assert(c.link_broken == 0);
    assert(c.latest.frame_counter == 4);
}

static void test_unknown_protocol_version_is_flagged(void) {
    /* daemon_protocol.zig: a client that reads a version it does not know
     * should stop rather than parse the stream blind. */
    struct gz_client c; gz_client_init(&c);
    unsigned char buf[64];
    size_t n = put_status(buf, 1, 0, GZ_PROTOCOL_VERSION + 1);
    assert(gz_client_feed(&c, buf, n) == 0);
    assert(c.version_mismatch == 1);

    gz_client_init(&c);
    n = put_status(buf, 1, 0, GZ_PROTOCOL_VERSION);
    assert(gz_client_feed(&c, buf, n) == 0);
    assert(c.version_mismatch == 0);
}

int main(void) {
    test_subscribe_is_first_bytes_out();
    test_partial_frame_is_buffered();
    test_desync_requests_reconnect();
    test_watchdog_fires_without_gaze();

    test_incomplete_then_completed();
    test_byte_at_a_time_delivery();
    test_desync_is_sticky();
    test_oversize_length_is_desync_not_wait();

    test_body_is_copied_not_referenced();
    test_state_survives_compaction_of_a_partial_tail();
    test_feed_larger_than_the_accumulator();

    test_gap_detection_counts_missing_samples();
    test_reconnect_resets_gap_detection();

    test_subscribe_is_sent_on_connect();
    test_subscribe_is_sent_again_on_reconnect();

    test_err_codes();
    test_request_reports_err_frames();
    test_request_returns_the_matching_response();
    test_request_ignores_a_reply_to_another_command();
    test_request_times_out_without_a_reply();
    test_adopt_does_not_leak_the_fd_it_owns();
    test_adopt_sets_nonblocking();

    test_poll_reports_a_clean_eof_as_reconnect();
    test_poll_reports_an_errored_peer_as_reconnect();
    test_a_broken_link_stops_accepting_data();
    test_poll_delivers_gaze_over_a_socket();
    test_send_does_not_die_on_a_closed_peer();

    test_watchdog_is_armed_by_any_gaze_frame();
    test_watchdog_reports_stale_when_the_device_is_gone();
    test_a_returning_device_rearms_the_watchdog();
    test_watchdog_interval_is_a_parameter();
    test_watchdog_tolerates_a_backwards_clock();
    test_watchdog_is_seeded_at_connect();

    test_socket_path_follows_xdg_runtime_dir();
    test_connect_rejects_an_overlong_path();
    test_connect_does_not_close_a_stale_fd();
    test_connect_failure_leaves_no_fd();
    test_send_rejects_an_oversize_payload();
    test_send_reports_a_backed_up_queue();
    test_outbound_holds_a_full_calibration_blob();
    test_take_outbound_handles_a_small_buffer();
    test_display_area_frame_is_consumed_not_fatal();
    test_unknown_protocol_version_is_flagged();

    printf("all client tests passed\n");
    return 0;
}
