// Force a USB port reset on the Tobii ET5 from outside the daemon.
//
// The daemon under test is the shipped binary and is not modified: this opens
// the same device from a second process (open, not claim) and issues
// USBDEVFS_RESET via libusb_reset_device. The kernel re-enumerates the device,
// so the daemon's in-flight and subsequent bulk transfers on its old handle
// fail with LIBUSB_ERROR_NO_DEVICE, and coming back requires a fresh open,
// claim, session-open and handshake against a new device address.

#include <libusb.h>
#include <stdio.h>

int main(void) {
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) {
        fprintf(stderr, "libusb_init failed\n");
        return 1;
    }
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, 0x2104, 0x0313);
    if (!h) {
        fprintf(stderr, "device 2104:0313 not found\n");
        libusb_exit(ctx);
        return 2;
    }
    libusb_device *d = libusb_get_device(h);
    printf("before reset: bus %d addr %d\n",
           libusb_get_bus_number(d), libusb_get_device_address(d));
    int r = libusb_reset_device(h);
    printf("libusb_reset_device -> %d (%s)\n", r, libusb_error_name(r));
    libusb_close(h);
    libusb_exit(ctx);
    return r == 0 || r == LIBUSB_ERROR_NOT_FOUND ? 0 : 3;
}
