/* Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
 *
 * libusb's config.h for the Linux build (the part libusb's configure script
 * would write): glibc 2.35+ on x86-64, the usbfs backend with netlink hot-plug
 * events (no libudev). Only what libusb's core, the POSIX event/thread code and
 * the Linux backend test for is defined. */
#ifndef PIXELU_LIBUSB_LINUX_CONFIG_H
#define PIXELU_LIBUSB_LINUX_CONFIG_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#define DEFAULT_VISIBILITY __attribute__((visibility("default")))
#define PRINTF_FORMAT(a, b) __attribute__((__format__(__printf__, a, b)))
#define ENABLE_LOGGING 1

#define PLATFORM_POSIX 1
#define HAVE_OS_TIMER 1
#define HAVE_TIMERFD 1
#define HAVE_EVENTFD 1
#define HAVE_PIPE2 1
#define HAVE_NFDS_T 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_PTHREAD_CONDATTR_SETCLOCK 1
#define HAVE_PTHREAD_SETNAME_NP 1
#define HAVE_SYS_TIME_H 1
#define HAVE_ASM_TYPES_H 1
#define HAVE_SYSLOG 1

#endif
