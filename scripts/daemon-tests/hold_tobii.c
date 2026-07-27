// Hold interface 0 of the Tobii ET5 so the daemon cannot reclaim it.
//
// Used with reset_tobii to simulate a tracker that stays away: the daemon's
// reopen keeps failing at libusb_claim_interface, so its retry loop and the
// exponential backoff run for as long as this process holds the interface.
// argv[1] is how many seconds to hold once the claim succeeds.

#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    double hold = argc > 1 ? atof(argv[1]) : 10.0;
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) return 1;

    double deadline = now() + 20.0;
    libusb_device_handle *h = NULL;
    int claimed = 0;
    while (now() < deadline && !claimed) {
        if (!h) h = libusb_open_device_with_vid_pid(ctx, 0x2104, 0x0313);
        if (h) {
            int r = libusb_claim_interface(h, 0);
            if (r == 0) {
                claimed = 1;
                break;
            }
            if (r == LIBUSB_ERROR_NO_DEVICE) {
                libusb_close(h);
                h = NULL;
            }
        }
        usleep(2000);
    }
    if (!claimed) {
        fprintf(stderr, "never claimed interface 0\n");
        if (h) libusb_close(h);
        libusb_exit(ctx);
        return 2;
    }
    printf("HELD interface 0 at %.3f, holding %.1fs\n", now(), hold);
    fflush(stdout);
    usleep((useconds_t)(hold * 1e6));
    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(ctx);
    printf("RELEASED at %.3f\n", now());
    return 0;
}
