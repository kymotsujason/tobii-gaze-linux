/* gaze-cal/src/client.c - see client.h. */

/* -std=c11 defines __STRICT_ANSI__, which makes glibc hide every POSIX
 * declaration this file needs, down to clock_gettime. Without it the build
 * fails on implicit declarations under -Werror. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "client.h"

uint64_t gz_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int gz_socket_path(char *buf, size_t cap) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (dir == NULL || dir[0] == '\0') dir = "/tmp";   /* daemon_protocol.zig orelse */
    int n = snprintf(buf, cap, "%s/tobiifreed/gaze.sock", dir);
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}

void gz_client_init(struct gz_client *c) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
    /* Mandatory. A client that does not subscribe receives zero samples and no
     * error frame, on the first connection and on every reconnection alike. */
    c->out_len = gz_encode_cmd(c->out, sizeof c->out, GZ_CMD_SUBSCRIBE, NULL, 0);
}

void gz_client_close(struct gz_client *c) {
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

int gz_client_adopt(struct gz_client *c, int fd) {
    if (fd < 0) { errno = EBADF; return -1; }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        int e = errno;
        close(fd);
        gz_client_init(c);
        errno = e;
        return -1;
    }

    /* init before storing fd: it clears the gap counters, the stale status and
     * the half-parsed accumulator from the previous connection, and re-queues
     * the subscribe. */
    gz_client_init(c);
    c->fd = fd;

    /* Seeded, not left at zero. CLOCK_MONOTONIC counts from boot, so a zero
     * here fires the watchdog on the first tick after connecting. */
    c->last_gaze_ns = gz_now_ns();

    int r = gz_client_flush(c);
    if (r == GZ_CLIENT_RECONNECT) return -1;
    return 0;
}

int gz_client_connect(struct gz_client *c, const char *path) {
    struct sockaddr_un a;
    size_t len = (path == NULL) ? 0 : strlen(path);

    /* First and unconditional. The client is very often a fresh stack variable
     * whose fd field is whatever was on the stack, and an fd-shaped garbage
     * value names a live descriptor belonging to someone else. This is also
     * why connect does not close a previous connection: it cannot tell one
     * from garbage. gz_client_reconnect is the call that does both. */
    gz_client_init(c);

    if (path == NULL) { errno = EINVAL; return -1; }

    /* strncpy would truncate silently and then connect to a different socket
     * than the one that was asked for. */
    if (len == 0 || len >= sizeof a.sun_path) {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    memcpy(a.sun_path, path, len + 1);

    /* Connected while still blocking: a unix connect to a listening socket
     * either succeeds now or fails now, so there is no EINPROGRESS dance.
     * gz_client_adopt switches the fd to non-blocking afterwards. */
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        int e = errno;
        close(fd);
        gz_client_init(c);
        errno = e;
        return -1;
    }

    return gz_client_adopt(c, fd);
}

int gz_client_reconnect(struct gz_client *c, const char *path) {
    /* Unlike connect, this one reads c->fd, so the client must already have
     * been through gz_client_init or gz_client_connect at least once. */
    gz_client_close(c);
    return gz_client_connect(c, path);
}

size_t gz_client_take_outbound(struct gz_client *c, unsigned char *buf, size_t cap) {
    size_t n = c->out_len - c->out_off;
    if (n > cap) n = cap;
    memcpy(buf, c->out + c->out_off, n);
    c->out_off += n;
    if (c->out_off == c->out_len) { c->out_off = 0; c->out_len = 0; }
    return n;
}

int gz_client_flush(struct gz_client *c) {
    if (c->out_off == c->out_len) { c->out_off = c->out_len = 0; return 0; }
    if (c->fd < 0) return 1;
    if (c->link_broken) return GZ_CLIENT_RECONNECT;

    while (c->out_off < c->out_len) {
        /* MSG_NOSIGNAL, not write(): a peer that has gone away raises SIGPIPE
         * otherwise, and in Plan 2 the process being killed is OBS. */
        ssize_t w = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
        if (w > 0) { c->out_off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
        c->link_broken = 1;
        return GZ_CLIENT_RECONNECT;
    }
    c->out_off = c->out_len = 0;
    return 0;
}

/* Parses every whole frame sitting in the accumulator, then compacts. Returns
 * gaze frames consumed, or GZ_CLIENT_RECONNECT. */
static int drain(struct gz_client *c, uint64_t now_ns) {
    int frames = 0;
    int desync = 0;
    size_t off = 0;

    for (;;) {
        struct gz_frame f;
        int r = gz_frame_parse(c->in + off, c->in_len - off, &f);

        /* 0 and GZ_ERR_DESYNC mean opposite things and are handled apart.
         * Folding them into `r <= 0` either discards a torn read or spins on
         * garbage forever. */
        if (r == 0) break;                       /* incomplete: keep the bytes */
        if (r == GZ_ERR_DESYNC) { desync = 1; break; }

        /* Every branch below copies what it needs out of f.body before the
         * memmove at the end of this function moves those bytes. */
        switch (f.type) {
        case GZ_SRV_GAZE: {
            struct gz_gaze_sample s;
            if (gz_frame_gaze(&f, &s)) {
                if (c->have_prev_counter) {
                    c->dropped += gz_frames_dropped(c->prev_counter, s.frame_counter);
                }
                c->prev_counter = s.frame_counter;
                c->have_prev_counter = 1;
                c->latest = s;
                c->have_latest = 1;
                c->gaze_frames++;
                c->last_gaze_ns = now_ns;
                if (gz_sample_any_eye_valid(&s)) c->last_valid_gaze_ns = now_ns;
                frames++;
            }
            break;
        }
        case GZ_SRV_STATUS: {
            struct gz_status st;
            if (gz_frame_status(&f, &st)) {
                int was_absent = (!c->have_status || !c->status.device_present);
                c->status = st;
                c->have_status = 1;
                /* A device coming back restarts the watchdog clock. The status
                 * frame lands before the first gaze frame of the new session,
                 * so without this the silence accumulated during the outage is
                 * charged to the link and forces a reconnect that fixes
                 * nothing. Measured on a 12 s hold_tobii outage: the client
                 * reconnected 13.4 s of silence later, immediately after the
                 * daemon announced the tracker was back. */
                if (st.device_present && was_absent) c->last_gaze_ns = now_ns;
                /* daemon_protocol.zig: a client that reads a version it does
                 * not know should stop rather than parse the stream blind.
                 * Recorded rather than acted on, so the caller decides. */
                c->version_mismatch = (st.protocol_version != GZ_PROTOCOL_VERSION);
            }
            break;
        }
        case GZ_SRV_RESPONSE: {
            size_t n = f.body_len;
            if (n > sizeof c->resp) n = sizeof c->resp;
            /* One slot. Overwriting a reply nobody has read yet is a lost
             * reply, which only happens to a caller pipelining commands, so it
             * is counted rather than hidden. */
            if (c->have_response) c->resp_mismatch++;
            memcpy(c->resp, f.body, n);
            c->resp_len = n;
            c->resp_cmd = f.cmd_type;
            c->have_response = 1;
            break;
        }
        case GZ_SRV_ERR: {
            uint32_t code = 0;
            if (gz_frame_err(&f, &code)) {
                c->err_code = code;
                c->have_error = 1;
            }
            break;
        }
        default:
            /* GZ_SRV_DISPLAY_AREA, which the daemon never emits. Consumed by
             * length and ignored rather than treated as corruption. */
            break;
        }
        off += (size_t)r;
    }

    if (off > 0) {
        memmove(c->in, c->in + off, c->in_len - off);
        c->in_len -= off;
    }

    if (desync) {
        /* Sticky. After a desync no byte offset in this stream is trustworthy,
         * so a caller that ignores the first reconnect request must not be
         * handed a parse that merely relocked onto plausible-looking bytes. */
        c->link_broken = 1;
        return GZ_CLIENT_RECONNECT;
    }
    return frames;
}

int gz_client_feed_at(struct gz_client *c, const unsigned char *b, size_t n, uint64_t now_ns) {
    if (c->link_broken) return GZ_CLIENT_RECONNECT;
    if (n > 0 && b == NULL) return GZ_CLIENT_RECONNECT;

    int frames = 0;
    size_t off = 0;
    while (off < n) {
        size_t space = sizeof c->in - c->in_len;
        if (space == 0) {
            /* Unreachable while the accumulator holds a whole GZ_MAX_FRAME:
             * the parser rejects a longer frame instead of waiting for it, so
             * a full buffer always contains something drainable. Kept so the
             * loop cannot spin if that invariant is ever broken. */
            c->link_broken = 1;
            return GZ_CLIENT_RECONNECT;
        }
        size_t take = n - off;
        if (take > space) take = space;
        memcpy(c->in + c->in_len, b + off, take);
        c->in_len += take;
        off += take;

        int r = drain(c, now_ns);
        if (r == GZ_CLIENT_RECONNECT) return GZ_CLIENT_RECONNECT;
        frames += r;
    }
    return frames;
}

int gz_client_feed(struct gz_client *c, const unsigned char *b, size_t n) {
    return gz_client_feed_at(c, b, n, gz_now_ns());
}

int gz_client_poll(struct gz_client *c, int timeout_ms) {
    if (c->link_broken) return GZ_CLIENT_RECONNECT;
    if (c->fd < 0) return GZ_CLIENT_RECONNECT;

    struct pollfd p;
    p.fd = c->fd;
    p.events = POLLIN;
    if (c->out_off < c->out_len) p.events |= POLLOUT;
    p.revents = 0;

    int pr = poll(&p, 1, timeout_ms);
    if (pr < 0) {
        if (errno == EINTR) return 0;
        c->link_broken = 1;
        return GZ_CLIENT_RECONNECT;
    }
    if (pr == 0) return 0;

    if (p.revents & (POLLERR | POLLNVAL)) {
        c->link_broken = 1;
        return GZ_CLIENT_RECONNECT;
    }
    if (p.revents & POLLOUT) {
        int fr = gz_client_flush(c);
        if (fr == GZ_CLIENT_RECONNECT) return GZ_CLIENT_RECONNECT;
    }

    int frames = 0;
    if (p.revents & (POLLIN | POLLHUP)) {
        /* Read straight into the accumulator tail, so a gaze frame is copied
         * once rather than twice on the way to c->latest. */
        for (;;) {
            size_t space = sizeof c->in - c->in_len;
            if (space == 0) break;
            ssize_t r = recv(c->fd, c->in + c->in_len, space, 0);
            if (r > 0) {
                c->in_len += (size_t)r;
                int d = drain(c, gz_now_ns());
                if (d == GZ_CLIENT_RECONNECT) return GZ_CLIENT_RECONNECT;
                frames += d;
                if ((size_t)r < space) break;    /* drained the socket */
                continue;
            }
            if (r == 0) {                        /* orderly shutdown by the daemon */
                c->link_broken = 1;
                return GZ_CLIENT_RECONNECT;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            c->link_broken = 1;
            return GZ_CLIENT_RECONNECT;
        }
    }
    return frames;
}

int gz_client_send(struct gz_client *c, uint8_t cmd, const void *payload, size_t n) {
    if (c->link_broken) return GZ_CLIENT_RECONNECT;

    /* Compact first: out_off only advances, so a long-lived client would
     * otherwise run out of tail space while the buffer is mostly consumed. */
    if (c->out_off > 0 && c->out_off == c->out_len) {
        c->out_off = c->out_len = 0;
    } else if (c->out_off > 0) {
        memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
        c->out_len -= c->out_off;
        c->out_off = 0;
    }

    size_t w = gz_encode_cmd(c->out + c->out_len, sizeof c->out - c->out_len, cmd, payload, n);
    if (w == 0) {
        /* Either the payload is longer than the protocol allows, which is a
         * caller bug, or the queue is backed up, which is transient. */
        if (n > GZ_MAX_PAYLOAD || (n > 0 && payload == NULL)) return -1;
        if (GZ_HEADER_SIZE + n > sizeof c->out) return -1;
        return GZ_CLIENT_FULL;
    }
    c->out_len += w;

    int r = gz_client_flush(c);
    if (r == GZ_CLIENT_RECONNECT) return GZ_CLIENT_RECONNECT;
    return 0;
}

int gz_client_request(struct gz_client *c, uint8_t cmd, const void *payload, size_t n,
                      int timeout_ms) {
    c->have_response = 0;
    c->have_error = 0;
    c->err_code = 0;

    int sr = gz_client_send(c, cmd, payload, n);
    if (sr != 0) return sr;

    if (timeout_ms < 0) timeout_ms = GZ_CLIENT_CMD_TIMEOUT_MS;
    uint64_t deadline = gz_now_ns() + (uint64_t)timeout_ms * 1000000ULL;

    for (;;) {
        uint64_t now = gz_now_ns();
        if (now >= deadline) return GZ_CLIENT_TIMEOUT;
        uint64_t left_ms = (deadline - now) / 1000000ULL;
        int slice = (int)(left_ms > 1000 ? 1000 : left_ms) + 1;

        int pr = gz_client_poll(c, slice);
        if (pr == GZ_CLIENT_RECONNECT) return GZ_CLIENT_RECONNECT;

        /* An err frame carries no cmd_type, so it cannot be correlated. In a
         * request/reply exchange it is this command's answer. */
        if (c->have_error) return GZ_CLIENT_REMOTE;

        if (c->have_response) {
            if (c->resp_cmd == cmd) return 0;
            /* A reply to an earlier command that timed out. Counted rather
             * than silently swallowed, because a nonzero count means this
             * client and the daemon are out of step. */
            c->resp_mismatch++;
            c->have_response = 0;
        }
    }
}

int gz_client_watchdog(const struct gz_client *c, uint64_t now_ns) {
    if (c->link_broken) return GZ_CLIENT_RECONNECT;
    /* Unsigned subtraction would turn a clock that appears to move backwards
     * into an 18-quintillion-nanosecond gap and fire instantly. */
    if (now_ns <= c->last_gaze_ns) return GZ_LINK_OK;
    if (now_ns - c->last_gaze_ns <= GZ_WATCHDOG_NS) return GZ_LINK_OK;

    /* Silence the daemon has already explained. Reconnecting to a healthy
     * daemon cannot bring back an unplugged tracker, and the reconnect loop it
     * would cause is worse than showing the overlay as stale. Spec section 11
     * needs these two cases told apart, and only the status frame can. */
    if (c->have_status && !c->status.device_present) return GZ_LINK_STALE;

    return GZ_CLIENT_RECONNECT;
}
