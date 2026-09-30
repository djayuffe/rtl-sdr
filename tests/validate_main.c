/* Runs rtl_validate against the fake bus: initialise the emulated dongle first. */
#include "fake_libusb.h"
static void __attribute__((constructor)) init(void) { fake_reset(); }
