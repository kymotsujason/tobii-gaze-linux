/* gaze-cal/tests/test_client_live.c
 *
 * Needs a running tobiifreed and a connected tracker. `make test-live`.
 *
 * The unit suite proves the client parses and reconnects correctly against
 * bytes it wrote itself. This one proves those bytes are the ones the daemon
 * actually sends, and it is the only place the subscribe rule can be shown to
 * matter: an unsubscribed client is not refused, it is simply never sent a
 * gaze frame, so nothing short of a real daemon can distinguish a client that
 * forgot to subscribe from one whose tracker is idle.
 */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../src/client.h"

#ifdef NDEBUG
#error "test_client_live.c relies on assert(); do not build it with NDEBUG"
#endif

static int failures = 0;

static void check(int ok, const char *what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static void socket_path(char *buf, size_t cap) {
    const char *env = getenv("TOBII_SOCK");
    if (env != NULL && env[0] != '\0') {
        snprintf(buf, cap, "%s", env);
        return;
    }
    assert(gz_socket_path(buf, cap) == 0);
}

/* A raw connection that deliberately does not subscribe. */
static int raw_connect(const char *path) {
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

static void test_first_frame_is_status(const char *path) {
    printf("first frame on a fresh connection is status\n");
    int fd = raw_connect(path);
    assert(fd >= 0);

    unsigned char buf[4096];
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    assert(n >= 5);
    check(buf[0] == GZ_SRV_STATUS, "opcode 0x04");

    struct gz_frame f;
    int r = gz_frame_parse(buf, (size_t)n, &f);
    check(r > 0, "parses");
    struct gz_status st;
    check(gz_frame_status(&f, &st) == 1, "reads as a status");
    printf("       present=%u cal=%u version=%u\n",
           st.device_present, st.calibration_applied, st.protocol_version);
    check(st.protocol_version == GZ_PROTOCOL_VERSION, "protocol version is 1");
    check(st.device_present == 1, "device present");
    close(fd);
}

/* Counts gaze frames on a raw fd for `ms` milliseconds. */
static int count_gaze_unsubscribed(int fd, int ms) {
    struct gz_client c;
    gz_client_init(&c);
    /* Deliberately drops the queued subscribe on the floor. */
    unsigned char sink[16];
    gz_client_take_outbound(&c, sink, sizeof sink);

    uint64_t end = gz_now_ns() + (uint64_t)ms * 1000000ULL;
    unsigned char buf[8192];
    int gaze = 0;
    while (gz_now_ns() < end) {
        ssize_t n = recv(fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n > 0) {
            int r = gz_client_feed(&c, buf, (size_t)n);
            if (r > 0) gaze += r;
            continue;
        }
        if (n == 0) break;
        if (errno != EAGAIN && errno != EWOULDBLOCK) break;
        struct timespec ts = {0, 20000000L};
        nanosleep(&ts, NULL);
    }
    return gaze;
}

static void test_subscribe_is_what_produces_gaze(const char *path) {
    printf("subscribe is the difference between gaze and silence\n");

    int fd = raw_connect(path);
    assert(fd >= 0);
    int silent = count_gaze_unsubscribed(fd, 1500);
    close(fd);
    printf("       unsubscribed: %d gaze frames in 1.5 s\n", silent);
    check(silent == 0, "an unsubscribed client receives zero gaze");

    struct gz_client c;
    assert(gz_client_connect(&c, path) == 0);
    int got = 0;
    uint64_t end = gz_now_ns() + 1500000000ULL;
    while (gz_now_ns() < end) {
        int r = gz_client_poll(&c, 100);
        assert(r != GZ_CLIENT_RECONNECT);
        got += r;
    }
    printf("       subscribed:   %d gaze frames in 1.5 s (%.1f Hz), dropped=%llu\n",
           got, (double)got / 1.5, (unsigned long long)c.dropped);
    check(got >= 30, "a subscribed client receives gaze");
    /* 33.2 Hz measured, so 1.5 s is about 50 frames. A wide band, because the
     * point here is the subscribe, not the rate. */
    check(got >= 40 && got <= 60, "rate is the measured 33.2 Hz, not 133");
    check(c.dropped == 0, "no dropped samples on an idle daemon");
    check(c.have_status == 1, "status arrived");
    check(c.version_mismatch == 0, "protocol version understood");
    gz_client_close(&c);
}

static void test_reconnect_resubscribes(const char *path) {
    printf("a reconnect re-subscribes and does not fabricate drops\n");
    struct gz_client c;
    assert(gz_client_connect(&c, path) == 0);

    int first = 0;
    uint64_t end = gz_now_ns() + 1000000000ULL;
    while (gz_now_ns() < end) first += gz_client_poll(&c, 100);
    uint32_t counter_before = c.latest.frame_counter;
    check(first > 20, "gaze before the reconnect");

    gz_client_close(&c);
    assert(gz_client_connect(&c, path) == 0);
    check(c.dropped == 0 && c.have_prev_counter == 0, "gap state reset");

    int second = 0;
    end = gz_now_ns() + 1000000000ULL;
    while (gz_now_ns() < end) {
        int r = gz_client_poll(&c, 100);
        assert(r != GZ_CLIENT_RECONNECT);
        second += r;
    }
    printf("       before=%d after=%d counter %u -> %u dropped=%llu\n",
           first, second, counter_before, c.latest.frame_counter,
           (unsigned long long)c.dropped);
    check(second > 20, "gaze after the reconnect, so subscribe was re-sent");
    /* The device counter keeps running across a client reconnect, so a client
     * that carried prev_counter over would report the whole gap as drops. */
    check(c.dropped == 0, "the reconnect gap is not counted as dropped samples");
    check(gz_client_watchdog(&c, gz_now_ns()) == GZ_LINK_OK, "watchdog healthy");
    gz_client_close(&c);
}

static void test_command_round_trip(const char *path) {
    printf("command and response correlation\n");
    struct gz_client c;
    assert(gz_client_connect(&c, path) == 0);

    uint64_t t0 = gz_now_ns();
    int r = gz_client_request(&c, GZ_CMD_GET_DISPLAY_AREA, NULL, 0, GZ_CLIENT_CMD_TIMEOUT_MS);
    double ms = (double)(gz_now_ns() - t0) / 1e6;
    if (r == GZ_CLIENT_REMOTE) {
        /* usb_busy is legitimate and retryable: the USB thread would not park. */
        printf("       err_code=%u retryable=%d\n", c.err_code, gz_err_retryable(c.err_code));
        check(gz_err_retryable(c.err_code), "an err frame is an answer, not a hang");
    } else {
        printf("       reply in %.1f ms, cmd_type=0x%02x, %zu body bytes\n",
               ms, c.resp_cmd, c.resp_len);
        check(r == 0, "get_display_area answered");
        check(c.resp_cmd == GZ_CMD_GET_DISPLAY_AREA, "response carries its cmd_type");
        check(c.resp_len > 0, "response has a body");
        check(ms < 500.0, "answered well inside the timeout");
    }
    /* The gaze stream is not disturbed by a command. */
    int after = 0;
    uint64_t end = gz_now_ns() + 600000000ULL;
    while (gz_now_ns() < end) after += gz_client_poll(&c, 100);
    check(after > 10, "gaze continues after a command");
    gz_client_close(&c);
}

static void test_disconnect_command_does_not_disconnect(const char *path) {
    /* Measured 2026-07-26: server.zig:357 consumes Cmd.disconnect and returns
     * without closing, so 0xFF is a no-op and gaze keeps flowing. A client
     * that sends it and then waits for EOF waits forever, so gz_client_close
     * is the only way to end a session. */
    printf("the disconnect command is a no-op, not a close\n");
    struct gz_client c;
    assert(gz_client_connect(&c, path) == 0);
    assert(gz_client_send(&c, GZ_CMD_DISCONNECT, NULL, 0) == 0);

    int gaze = 0, reconnect = 0;
    uint64_t end = gz_now_ns() + 1500000000ULL;
    while (gz_now_ns() < end) {
        int r = gz_client_poll(&c, 100);
        if (r == GZ_CLIENT_RECONNECT) { reconnect = 1; break; }
        gaze += r;
    }
    printf("       %d gaze frames after sending 0xFF\n", gaze);
    check(!reconnect, "the daemon keeps the connection open");
    check(gaze > 20, "gaze keeps flowing after 0xFF");
    check(!c.have_error, "0xFF is not refused either");
    gz_client_close(&c);
}

static void test_eof_surfaces_as_reconnect(const char *path) {
    /* Half-closing is the one way to make the daemon close its end on demand.
     * Measured: EOF arrives immediately once nothing is pending, after the
     * queued gaze has been flushed. This is the same read path a daemon
     * restart takes, and it is the only one a client can trigger safely. */
    printf("EOF from the daemon surfaces as GZ_CLIENT_RECONNECT\n");
    struct gz_client c;
    assert(gz_client_connect(&c, path) == 0);

    uint64_t end = gz_now_ns() + 500000000ULL;
    while (gz_now_ns() < end) gz_client_poll(&c, 100);
    assert(shutdown(c.fd, SHUT_WR) == 0);

    int saw_reconnect = 0;
    uint64_t t0 = gz_now_ns();
    end = t0 + 15000000000ULL;
    while (gz_now_ns() < end) {
        if (gz_client_poll(&c, 200) == GZ_CLIENT_RECONNECT) { saw_reconnect = 1; break; }
    }
    printf("       EOF %.2f s after the half-close\n", (double)(gz_now_ns() - t0) / 1e9);
    check(saw_reconnect, "EOF reported as reconnect");
    check(c.link_broken == 1, "the client is sticky-broken until reconnected");

    /* And the recovery is a full reconnect, subscribe included. */
    assert(gz_client_reconnect(&c, path) == 0);
    int gaze = 0;
    end = gz_now_ns() + 1000000000ULL;
    while (gz_now_ns() < end) {
        int r = gz_client_poll(&c, 100);
        assert(r != GZ_CLIENT_RECONNECT);
        gaze += r;
    }
    check(gaze > 20, "gaze resumes after reconnecting");
    check(c.link_broken == 0, "the reconnect cleared the broken flag");
    gz_client_close(&c);
}

int main(void) {
    char path[512];
    socket_path(path, sizeof path);
    printf("socket: %s\n\n", path);

    int probe = raw_connect(path);
    if (probe < 0) {
        fprintf(stderr, "cannot reach the daemon at %s: %s\n", path, strerror(errno));
        return 77;                       /* skip, in the autotools sense */
    }
    close(probe);

    test_first_frame_is_status(path);
    test_subscribe_is_what_produces_gaze(path);
    test_reconnect_resubscribes(path);
    test_command_round_trip(path);
    test_disconnect_command_does_not_disconnect(path);
    test_eof_surfaces_as_reconnect(path);

    printf("\n%s\n", failures == 0 ? "all live client tests passed"
                                   : "LIVE CLIENT TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
