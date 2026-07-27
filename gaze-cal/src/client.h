/* gaze-cal/src/client.h - unix-socket connection layer over proto.c.
 *
 * Owns one connection to tobiifreed: the mandatory subscribe, an incremental
 * frame reader, a non-blocking writer, command/response correlation with a
 * timeout, gap accounting and a link watchdog. It knows nothing about the CLI,
 * because Plan 2's OBS filter plugin links this file directly and must not
 * inherit argv parsing, printf or an exit path.
 *
 * Nothing here allocates. A struct gz_client is about 33 KB and is meant to be
 * embedded in the caller's own state.
 *
 * Threading: one gz_client belongs to one thread. Spec section 7 puts the
 * socket on the plugin's own thread and hands samples to the render thread
 * through a separate seam, so no locking lives here.
 */
#ifndef GZ_CLIENT_H
#define GZ_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Negative results, distinct so a caller can switch on them. -1 is a local
 * failure with errno set; the rest describe the link or the daemon. */
#define GZ_CLIENT_RECONNECT (-2)   /* the stream is unusable: close, reconnect */
#define GZ_CLIENT_TIMEOUT   (-3)   /* no reply inside the deadline */
#define GZ_CLIENT_REMOTE    (-4)   /* daemon sent err; code in c->err_code */
#define GZ_CLIENT_FULL      (-5)   /* the outbound queue could not take it */

/* Non-negative watchdog results. Zero stays "healthy" so that the common
 * check remains `if (gz_client_watchdog(...))`. */
#define GZ_LINK_OK    0
#define GZ_LINK_STALE 1            /* no gaze, and the daemon says why */

/* Gaze arrives at a measured 33.2 Hz, one sample every 30.1 ms, so a second of
 * silence is 33 consecutive misses and cannot be jitter. The daemon gives up
 * on a stalled subscriber at about 10 s, so reacting at 1 s means the client
 * notices a wedge well before the daemon drops it.
 *
 * COUPLED TO THE DAEMON'S USB PAUSE, and this is the constant's real
 * falsifier. Every forwarded command parks the daemon's USB thread
 * (main.zig:328), which stops gaze production for EVERY subscribed client, not
 * only the one that sent the command. A command or batch that parks it for
 * longer than this interval therefore fires an unrelated client's watchdog,
 * and reconnecting cannot help, because nothing is wrong with that client's
 * link. The daemon's own eof-hold budgets 3.6 s of exactly this. At a measured
 * 30 ms per command inside a batch, 33 back-to-back commands reach 1 s.
 *
 * So 1 s is right for a daemon with one command-issuing client, which is what
 * exists today. The concrete counter-case is Task 13 calibrating while a
 * Plan 2 overlay is subscribed. A client that shares a daemon with a command
 * issuer should pass its own interval to gz_client_watchdog_for rather than
 * raise this default for everyone. */
#define GZ_WATCHDOG_NS 1000000000ULL

/* The daemon answers a command in about 30 ms, because each one parks the USB
 * thread until a poll boundary, and its own guarantee after a half-close is a
 * reply within about 5 s or never. 2 s sits far above the measured latency and
 * inside that window, so a timeout here means no reply is coming rather than
 * that the client was impatient. Calibration commands that make the device
 * gather samples pass their own, longer, deadline. */
#define GZ_CLIENT_CMD_TIMEOUT_MS 2000

/* Two whole maximum frames. Only one is needed for correctness, since a frame
 * larger than GZ_MAX_FRAME is rejected by the parser rather than waited for,
 * but two lets one read pick up about 40 gaze frames. Never size this with
 * GZ_MAX_PAYLOAD: that is five bytes short of a frame the parser calls valid. */
#define GZ_CLIENT_INBUF (2 * GZ_MAX_FRAME)

/* The largest command is cal_apply carrying a full 4096-byte blob, which is
 * 4101 bytes on the wire. Two of them fit, so a command can always be queued
 * behind a subscribe that has not drained yet. */
#define GZ_CLIENT_OUTBUF (2 * (GZ_HEADER_SIZE + GZ_CAL_BLOB_MAX))

struct gz_client {
    int fd;                        /* -1 when not connected */
    int link_broken;               /* sticky: set by desync, EOF or write error */

    unsigned char in[GZ_CLIENT_INBUF];
    size_t in_len;

    unsigned char out[GZ_CLIENT_OUTBUF];
    size_t out_len, out_off;

    /* Armed by any gaze frame, valid or not. The device streams at 33.2 Hz
     * whether or not it sees eyes, so silence means the stream died while
     * validity != 0 only means the user looked away. */
    uint64_t last_gaze_ns;
    uint64_t last_valid_gaze_ns;

    struct gz_gaze_sample latest;
    int have_latest;

    struct gz_status status;
    int have_status;
    int version_mismatch;          /* daemon speaks a protocol version we do not */

    /* Copied out of the accumulator, because a parsed body points into it and
     * dangles the moment the next feed compacts. */
    /* One slot, so a caller that pipelines commands must read each reply
     * before the next arrives. gz_client_request does exactly that. */
    int have_response;
    uint8_t resp_cmd;
    size_t resp_len;
    unsigned char resp[GZ_MAX_RESPONSE_PAYLOAD];
    uint32_t resp_mismatch;        /* replies discarded without being read */

    int have_error;
    uint32_t err_code;

    /* Per-connection, reset on every connect. NOT because the counter
     * restarts: measured across a full daemon kill of 15 s and a restart it
     * ran 530932 to 531802, monotonic, and nothing observed resets it, not a
     * daemon restart and not a USB re-enumeration (500952 to 500969 across an
     * 11 s outage). The reset is load-bearing for the opposite reason. The
     * counter keeps advancing while the client is away, so carrying
     * prev_counter across a reconnect charges the entire absence to dropped:
     * a measured reconnect gap of 870 counts is 217 samples that would have
     * been reported as lost. Never compare dropped across a reconnect. */
    uint32_t prev_counter;
    int have_prev_counter;
    uint64_t dropped;
    uint64_t gaze_frames;
};

/* CLOCK_MONOTONIC in nanoseconds. Monotonic, not wall time, so a clock step
 * cannot fire the watchdog. */
uint64_t gz_now_ns(void);

/* Mirrors daemon_protocol.zig socketPath(): $XDG_RUNTIME_DIR or /tmp, then
 * /tobiifreed/gaze.sock. Returns 0, or -1 if it does not fit. */
int gz_socket_path(char *buf, size_t cap);

/* Zeroes the client and queues the mandatory subscribe. Does not touch fd
 * ownership, so call gz_client_close first if one is open. */
void gz_client_init(struct gz_client *c);

/* Connects, then sends subscribe immediately. A client that does not subscribe
 * receives zero gaze samples and no error at all, which is the single most
 * common way to make this daemon look broken, so it is not left to the caller.
 * Returns 0, or -1 with errno set and fd left at -1.
 *
 * Overwrites the client completely and reads nothing beforehand, so it is safe
 * on a freshly declared stack variable. For the same reason it does NOT close
 * a previous connection, because it cannot tell a live fd from stack garbage.
 * Use gz_client_reconnect when there may be one. */
int gz_client_connect(struct gz_client *c, const char *path);

/* Closes, then connects. Requires a client that has already been through
 * gz_client_init or gz_client_connect, since it reads c->fd. */
int gz_client_reconnect(struct gz_client *c, const char *path);

/* Same as connect, on a socket the caller already has. Takes ownership of fd,
 * and like connect leaves any previous fd alone. This is the seam
 * gz_client_connect is built on, and the one the tests drive. */
int gz_client_adopt(struct gz_client *c, int fd);

void gz_client_close(struct gz_client *c);

/* Copies out queued command bytes for a caller doing its own I/O. Returns the
 * number copied, 0 when nothing is queued. */
size_t gz_client_take_outbound(struct gz_client *c, unsigned char *buf, size_t cap);

/* Writes queued bytes. Returns 0 when the queue is empty, 1 when the socket
 * would block and bytes remain, GZ_CLIENT_RECONNECT on a dead peer. */
int gz_client_flush(struct gz_client *c);

/* Appends bytes and parses every whole frame in them, updating latest, status,
 * resp, err and the gap counters. Returns the number of gaze frames consumed,
 * or GZ_CLIENT_RECONNECT if the stream desynced. Accepts any n: the buffer is
 * drained as it fills. Frames parsed before a desync still take effect.
 *
 * gz_client_feed stamps arrival with gz_now_ns(); gz_client_feed_at takes the
 * timestamp, which is what makes the watchdog testable. */
int gz_client_feed_at(struct gz_client *c, const unsigned char *b, size_t n, uint64_t now_ns);
int gz_client_feed(struct gz_client *c, const unsigned char *b, size_t n);

/* Waits up to timeout_ms, flushes anything queued, reads what is there and
 * feeds it. Returns gaze frames consumed, 0 on timeout, or GZ_CLIENT_RECONNECT
 * on EOF, desync or socket error. Never blocks longer than timeout_ms. */
int gz_client_poll(struct gz_client *c, int timeout_ms);

/* Encodes a command and tries to write it. Returns 0 on success, -1 on a bad
 * argument, GZ_CLIENT_FULL if the queue is backed up, GZ_CLIENT_RECONNECT on a
 * dead peer. */
int gz_client_send(struct gz_client *c, uint8_t cmd, const void *payload, size_t n);

/* Sends, then polls until the daemon answers. Returns 0 with the reply in
 * c->resp / c->resp_len, GZ_CLIENT_REMOTE with c->err_code set, or one of
 * GZ_CLIENT_TIMEOUT, GZ_CLIENT_RECONNECT, GZ_CLIENT_FULL, -1. Gaze frames that
 * arrive while waiting are consumed normally, not dropped.
 *
 * One command at a time. On GZ_CLIENT_TIMEOUT, reconnect: do not send the next
 * command on the same connection. The daemon may answer late, and an err frame
 * carries no cmd_type, so a late err lands on whichever command follows. */
int gz_client_request(struct gz_client *c, uint8_t cmd, const void *payload, size_t n,
                      int timeout_ms);

/* GZ_LINK_OK, GZ_LINK_STALE when the daemon has told us the device is gone, or
 * GZ_CLIENT_RECONNECT when gaze stopped with no explanation. Pass an absolute
 * gz_now_ns(); the client holds the other side of the comparison.
 *
 * The interval is a parameter rather than client state so that a reconnect,
 * which reinitialises everything else, cannot silently restore the default
 * under a caller that chose otherwise. gz_client_watchdog uses GZ_WATCHDOG_NS;
 * read that constant's note before picking a different one. */
int gz_client_watchdog_for(const struct gz_client *c, uint64_t now_ns, uint64_t interval_ns);
int gz_client_watchdog(const struct gz_client *c, uint64_t now_ns);

#ifdef __cplusplus
}
#endif

#endif
