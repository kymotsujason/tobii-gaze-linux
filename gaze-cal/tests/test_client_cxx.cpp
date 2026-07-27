/* gaze-cal/tests/test_client_cxx.cpp
 *
 * Plan 2's OBS filter plugin is C++ and reuses the connection layer, not just
 * the parser. Compiled with a C++ compiler and linked against a C-compiled
 * client.o and proto.o, so it proves the header parses as C++ and that the
 * declarations do not mangle. Every exported symbol is called, so all of them
 * must resolve at the link.
 */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/client.h"

#ifdef NDEBUG
#error "test_client_cxx.cpp relies on assert(); do not build it with NDEBUG"
#endif

static_assert(GZ_CLIENT_INBUF >= GZ_MAX_FRAME, "the accumulator must hold a whole frame");
static_assert(GZ_CLIENT_OUTBUF >= GZ_HEADER_SIZE + GZ_CAL_BLOB_MAX,
              "the outbound queue must hold a full cal_apply");

int main() {
    char path[512];
    assert(gz_socket_path(path, sizeof path) == 0);
    assert(std::strstr(path, "/tobiifreed/gaze.sock") != nullptr);

    assert(gz_now_ns() > 0);

    struct gz_client c;
    gz_client_init(&c);
    assert(c.fd == -1);

    unsigned char sub[8];
    assert(gz_client_take_outbound(&c, sub, sizeof sub) == 5);
    assert(sub[0] == GZ_CMD_SUBSCRIBE);

    /* connect failure path, then the socketpair seam */
    assert(gz_client_connect(&c, "/tmp/gaze-cal-no-such-socket-cxx") == -1);

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(gz_client_adopt(&c, sv[0]) == 0);
    assert(gz_client_flush(&c) == 0);

    unsigned char got[5] = {0};
    assert(read(sv[1], got, sizeof got) == 5);
    assert(got[0] == GZ_CMD_SUBSCRIBE);

    /* a status frame, fed through both feed entry points */
    unsigned char st[GZ_HEADER_SIZE + GZ_STATUS_SIZE] = {
        GZ_SRV_STATUS, GZ_STATUS_SIZE, 0, 0, 0, 1, 0, GZ_PROTOCOL_VERSION};
    assert(gz_client_feed(&c, st, sizeof st) == 0);
    assert(c.have_status == 1 && c.status.device_present == 1);
    assert(gz_client_feed_at(&c, st, sizeof st, gz_now_ns()) == 0);

    assert(gz_client_send(&c, GZ_CMD_GET_DISPLAY_AREA, nullptr, 0) == 0);
    assert(gz_client_poll(&c, 10) == 0);
    assert(gz_client_request(&c, GZ_CMD_GET_DISPLAY_AREA, nullptr, 0, 20) == GZ_CLIENT_TIMEOUT);
    assert(gz_client_watchdog(&c, gz_now_ns()) == GZ_LINK_OK);
    assert(gz_client_watchdog_for(&c, gz_now_ns(), 4000000000ULL) == GZ_LINK_OK);

    /* The call the OBS plugin needs most: its recovery path is reconnect, not
     * connect, and it must close the fd it already holds. */
    assert(gz_client_reconnect(&c, "/tmp/gaze-cal-no-such-socket-cxx") == -1);
    assert(c.fd == -1);

    gz_client_close(&c);
    close(sv[1]);

    std::printf("all client C++ interop tests passed\n");
    return 0;
}
