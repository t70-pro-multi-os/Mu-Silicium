// Linux control service for the shipped MT6789 WMT driver.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "WmtReady.h"

#define WMT_SET_MODE 0x4004a005UL
#define WMT_SET_LAST_CLOSE 0x4004a00dUL
#define WMT_CHIP_INFO 0x8004a00cUL
#define WMT_CONFIG_PATH 0xc008a015UL
#define WMT_ROM_PATCH 0x4008a01fUL
#define WMT_COMMAND_MAX 255U
struct rom_patch { uint32_t type; uint8_t address[4]; char name[256]; };
_Static_assert(sizeof(struct rom_patch) == 264, "shipped WMT ROM patch ioctl payload");

#define WMT_DETECT_ADIE 0x80047709UL
#define WMT_DETECT_HW_INIT 0x8004770aUL
#define WMT_DETECT_SOC 0x80047703UL
#define WMT_DETECT_SET_CHIP 0x40047701UL
#define WMT_DETECT_COMMON_INIT 0x80047704UL
#define WMT_PATCH_COUNT 3U
struct wmt_context {
    int fd, detect;
    const char *firmware;
    int (*call)(int, unsigned long, unsigned long);
    unsigned adie;
};
#ifndef WMT_CONTROL_TEST
static int device_call(int fd, unsigned long request, unsigned long value) {
    return ioctl(fd, request, value);
}
#endif

// Offsets and the 264-byte ioctl layout are from this board's shipped launcher.
// Select the RF variant from the driver, never from a guessed firmware filename.
static int load_catalog(const char *directory, unsigned adie, unsigned revision,
                        struct rom_patch records[WMT_PATCH_COUNT]) {
    const char *variant;
    if (adie == 0x6631) variant = "1d_1";
    else if (adie == 0x6635) variant = "1e_1";
    else { errno = ENODEV; return -1; }
    const char *const names[] = {"bt", "wifi", "mcu"};
    const unsigned types[] = {0, 3, 4};
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) return -1;
    memset(records, 0, sizeof(*records) * WMT_PATCH_COUNT);
    int result = -1;
    for (unsigned i = 0; i < WMT_PATCH_COUNT; i++) {
        int n = snprintf(records[i].name, sizeof(records[i].name),
                         "soc2_2_ram_%s_%s_hdr.bin", names[i], variant);
        if (n < 0 || (size_t)n >= sizeof(records[i].name)) { errno = ENAMETOOLONG; goto done; }
        int fd = openat(dir, records[i].name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) goto done;
        struct stat st;
        uint8_t header[32];
        int rc = fstat(fd, &st);
        if (!rc && (!S_ISREG(st.st_mode) || st.st_size < (off_t)sizeof(header))) { errno = EINVAL; rc = -1; }
        size_t have = 0;
        while (!rc && have < sizeof(header)) {
            ssize_t count = pread(fd, header + have, sizeof(header) - have, (off_t)have);
            if (count > 0) have += (size_t)count;
            else if (count < 0 && errno == EINTR) continue;
            else { if (!count) errno = EIO; rc = -1; }
        }
        int saved = errno; close(fd); errno = saved;
        if (rc) goto done;
        if (header[23] != (revision & 0xffU) || header[27] < 0x10 || header[31] != types[i]) {
            errno = EINVAL; goto done;
        }
        records[i].type = header[31];
        memcpy(records[i].address, header + 24, sizeof(records[i].address));
        records[i].address[0] = 0;
    }
    result = 0;
done:;
    int saved = errno; close(dir); errno = saved;
    return result;
}
static int publish_catalog(void *opaque) {
    struct wmt_context *ctx = opaque;
    int chip = ctx->call(ctx->fd, WMT_CHIP_INFO, 0);
    if (chip != 0x6789) return -ENODEV;
    if (ctx->adie != 0x6631 && ctx->adie != 0x6635) return -ENODEV;
    int revision = ctx->call(ctx->fd, WMT_CHIP_INFO, 2);
    if (revision < 0) return -EIO;
    struct rom_patch records[WMT_PATCH_COUNT];
    if (load_catalog(ctx->firmware, ctx->adie, (unsigned)revision, records)) return -errno;
    // Validate every required record before changing the driver's catalog.
    for (unsigned i = 0; i < WMT_PATCH_COUNT; i++) {
        if (ctx->call(ctx->fd, WMT_ROM_PATCH, (unsigned long)&records[i])) return -EIO;
    }
    return 0;
}

static volatile sig_atomic_t stopping;
static void stop_control(int s) { (void)s; stopping = 1; }
typedef int (*catalog_handler)(void *);

static int call_zero(struct wmt_context *ctx, int fd, unsigned long request, unsigned long value) {
    int result = ctx->call(fd, request, value);
    if (!result) return 0;
    if (result > 0 || !errno) errno = EIO;
    return -1;
}

// wmt_drv's module init creates only wmtdetect. The SoC loader must initialize
// the platform, identify the SoC, set its chip type and create the WMT devices.
// The exact shipped ioctl dispatcher ignores COMMON_INIT's argument.
static int initialize_platform(struct wmt_context *ctx) {
    if (call_zero(ctx, ctx->detect, WMT_DETECT_HW_INIT, 0)) return -1;
    if (ctx->call(ctx->detect, WMT_DETECT_SOC, 0) != 0x6789) { errno = ENODEV; return -1; }
    if (call_zero(ctx, ctx->detect, WMT_DETECT_SET_CHIP, 0x6789) ||
        call_zero(ctx, ctx->detect, WMT_DETECT_COMMON_INIT, 0)) return -1;
    return 0;
}

// The driver exposes one command per read and expects one complete text reply.
// Keep both requests and firmware contents out of diagnostic logs.
static int serve(int fd, int ready_fd, catalog_handler handler, void *context) {
    while (!stopping) {
        struct pollfd p[2] = {{fd, POLLIN, 0}, {ready_fd, POLLIN, 0}};
        nfds_t poll_count = ready_fd >= 0 ? 2 : 1;
        int ready = poll(p, poll_count, 250);
        if (ready < 0) { if (errno == EINTR) continue; return -1; }
        if (p[0].revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = ENODEV; return -1; }
        if (p[0].revents & POLLIN) {
        char command[WMT_COMMAND_MAX + 1], response[32];
        ssize_t length = read(fd, command, WMT_COMMAND_MAX);
        if (length < 0) { if (errno == EINTR || errno == EAGAIN) continue; return -1; }
        if (!length) { errno = ENODEV; return -1; }
        int result;
        if (length == WMT_COMMAND_MAX || memchr(command, 0, (size_t)length)) result = -1;
        else {
            command[length] = 0;
            if (!strncmp(command, "srh_rom_patch", 13) &&
                (!command[13] || command[13] == ' ')) result = handler(context);
            else result = 1;
        }
        if (!result) strcpy(response, "ok");
        else if (result == 1) strcpy(response, "cmd not found");
        else snprintf(response, sizeof(response), "resp_%d", result);
        size_t count = strlen(response);
        for (;;) {
            ssize_t n = write(fd, response, count);
            if (n == (ssize_t)count) break;
            if (n < 0 && errno == EINTR && !stopping) continue;
            if (n >= 0) errno = EIO;
            return -1;
        }
        }
        // Never let diagnostics delay a ROM-patch request already waiting on STP.
        if (ready_fd >= 0) {
            if (p[1].revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = ENODEV; return -1; }
            if ((p[1].revents & POLLIN) && t70_wmt_ready_accept(ready_fd)) return -1;
        }
    }
    return 0;
}

static int configure_control(struct wmt_context *ctx) {
    if (ctx->call(ctx->fd, WMT_CHIP_INFO, 0) != 0x6789) { errno = ENODEV; return -1; }
    // This SoC uses the driver's WMT_SOC.cfg default; match the stock launcher.
    char config[256] = {0};
    if (call_zero(ctx, ctx->fd, WMT_CONFIG_PATH, (unsigned long)config) ||
        call_zero(ctx, ctx->fd, WMT_SET_MODE, 0x23) ||
        call_zero(ctx, ctx->fd, WMT_SET_LAST_CLOSE, 0)) return -1;
    // A cache miss in this ioctl can power the RF chip. Resolve it before
    // serving srh_rom_patch, while no driver operation is waiting on us.
    int adie = ctx->call(ctx->detect, WMT_DETECT_ADIE, 0);
    if (adie != 0x6631 && adie != 0x6635) { errno = ENODEV; return -1; }
    ctx->adie = (unsigned)adie;
    return 0;
}
static int set_wifi_power(int fd, int enabled) {
    char value = enabled ? '1' : '0';
    for (;;) {
        ssize_t count = write(fd, &value, 1);
        if (count == 1) return 0;
        if (count < 0 && errno == EINTR && !stopping) continue;
        if (count >= 0) errno = EIO;
        return -1;
    }
}

#ifndef WMT_CONTROL_TEST
static int open_device(const char *name) {
    int fd = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st)) { int saved = errno; close(fd); errno = saved; return -1; }
    if (!S_ISCHR(st.st_mode)) { close(fd); errno = ENODEV; return -1; }
    if (flock(fd, LOCK_EX | LOCK_NB)) { int saved = errno; close(fd); errno = saved; return -1; }
    return fd;
}
static int wait_command_device(void) {
    // The exact kernel has no devtmpfs; mdev receives the new WMT device event.
    for (unsigned attempt = 0; attempt < 50 && !stopping; attempt++) {
        int fd = open_device("/dev/stpwmt");
        if (fd >= 0) return fd;
        if (errno != ENOENT && errno != ENODEV && errno != ENXIO) return -1;
        struct timespec pause = {.tv_nsec = 100000000};
        while (nanosleep(&pause, &pause) && errno == EINTR && !stopping) {}
    }
    errno = stopping ? EINTR : ETIMEDOUT;
    return -1;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts("teclast-wmt-control 1 (MT6789 ROM catalog / BTIF)"); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--ready")) {
        if (t70_wmt_ready()) { fputs("WMT: ROM-patch service is not ready\n", stderr); return 69; }
        return 0;
    }
    if (argc != 2 || (strcmp(argv[1], "--run") && strcmp(argv[1], "--wifi-on") && strcmp(argv[1], "--wifi-off"))) {
        fputs("usage: teclast-wmt-control --run | --ready | --wifi-on | --wifi-off | --version\n", stderr); return 64;
    }
    if (geteuid()) { fputs("WMT: root is required\n", stderr); return 77; }
    struct sigaction sa = {.sa_handler = stop_control};
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTERM, &sa, NULL) || sigaction(SIGINT, &sa, NULL) || signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("WMT: signals"); return 71;
    }
    if (strcmp(argv[1], "--run")) {
        int enabled = !strcmp(argv[1], "--wifi-on");
        if (enabled && t70_wmt_ready()) { fputs("WMT: ROM-patch service is not ready\n", stderr); return 69; }
        int fd = open_device("/dev/wmtWifi");
        if (fd < 0) { perror("WMT: Wi-Fi control device"); return 69; }
        int rc = set_wifi_power(fd, enabled);
        if (rc) perror("WMT: Wi-Fi power request");
        close(fd);
        if (!rc && enabled && t70_wmt_ready()) {
            fputs("WMT: ROM-patch service stopped during Wi-Fi power-on\n", stderr);
            return 69;
        }
        return rc ? 74 : 0;
    }
    struct wmt_context ctx = {.fd = -1, .detect = -1, .firmware = "/lib/firmware", .call = device_call};
    ctx.detect = open_device("/dev/wmtdetect");
    if (ctx.detect < 0) { perror("WMT: chip query device"); return 69; }
    ctx.fd = open_device("/dev/stpwmt");
    if (ctx.fd < 0) {
        if ((errno != ENOENT && errno != ENODEV && errno != ENXIO) || initialize_platform(&ctx)) {
            perror("WMT: platform initialization"); close(ctx.detect); return 74;
        }
        ctx.fd = wait_command_device();
        if (ctx.fd < 0) { perror("WMT: command device"); close(ctx.detect); return 69; }
    }
    int rc = configure_control(&ctx);
    if (rc) perror("WMT: control initialization");
    else {
        int ready_fd = t70_wmt_ready_server();
        if (ready_fd < 0) {
            perror("WMT: readiness endpoint"); rc = -1;
        } else {
        puts("WMT control ready; ROM catalog is provided on driver request"); fflush(stdout);
        rc = serve(ctx.fd, ready_fd, publish_catalog, &ctx);
        if (rc) perror("WMT: command service");
            close(ready_fd);
        }
    }
    close(ctx.detect); close(ctx.fd);
    return rc ? 74 : 0;
}
#endif
