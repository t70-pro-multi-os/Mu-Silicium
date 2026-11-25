// Linux HCI UART H4 over the stock MediaTek /dev/stpbt transport.
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
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "WmtReady.h"

#define STP_MAX 2048U
#define HCIUARTSETPROTO _IOW('U', 200, int)
#define HCIUARTGETDEVICE _IOR('U', 202, int)
#define HCIUARTSETFLAGS _IOW('U', 203, int)
#define N_HCI 15
static volatile sig_atomic_t stopping;
static void stop_bridge(int signum) { (void)signum; stopping = 1; }
static int64_t milliseconds(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// PTY reads may split or combine commands; the STP driver needs one H4 frame/write.
static int frame_length(const uint8_t *data, size_t count) {
    size_t header, payload;
    if (!count) return 0;
    switch (data[0]) {
    case 1: case 3:
        header = 4;
        if (count < header) return 0;
        payload = data[3]; break;
    case 2: case 5:
        header = 5;
        if (count < header) return 0;
        payload = (unsigned)data[3] | (unsigned)data[4] << 8;
        if (data[0] == 5) payload &= 0x3fff;
        break;
    default: errno = EPROTO; return -1;
    }
    if (header + payload > STP_MAX) { errno = EMSGSIZE; return -1; }
    return count < header + payload ? 0 : (int)(header + payload);
}

static int relay(int master, int transport, int timeout_ms) {
    uint8_t tx[STP_MAX * 2], rx[STP_MAX];
    size_t ntx = 0, nrx = 0;
    int64_t tx_progress = milliseconds(), rx_progress = tx_progress, retry_at = 0;
    if (tx_progress < 0) return -1;
    while (!stopping) {
        int frame = frame_length(tx, ntx);
        int64_t now = milliseconds();
        if (frame < 0 || now < 0) return -1;
        if ((ntx && now - tx_progress >= timeout_ms) ||
            (nrx && now - rx_progress >= timeout_ms)) {
            errno = ETIMEDOUT; return -1;
        }
        struct pollfd p[2] = {
            {master, (short)((ntx < sizeof(tx) ? POLLIN : 0) | (nrx ? POLLOUT : 0)), 0},
            {transport, (short)((nrx < sizeof(rx) ? POLLIN : 0) |
                               (frame > 0 && now >= retry_at ? POLLOUT : 0)), 0}
        };
        int ready = poll(p, 2, 20);
        if (ready < 0) { if (errno == EINTR) continue; return -1; }
        for (int i = 0; i < 2; i++)
            if (p[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                errno = ENODEV; return -1;
            }
        if (p[0].revents & POLLIN) {
            ssize_t n = read(master, tx + ntx, sizeof(tx) - ntx);
            if (n > 0) {
                // Only the initial enqueue or a complete STP write advances
                // this clock. Fragment arrival is not transmission progress.
                if (!ntx && (tx_progress = milliseconds()) < 0)
                    return -1;
                ntx += (size_t)n;
            }
            else if (!n) { errno = ENODEV; return -1; }
            else if (errno != EAGAIN && errno != EINTR) return -1;
        }
        if (p[1].revents & POLLOUT) {
            ssize_t n = write(transport, tx, (size_t)frame);
            if (n == frame) {
                ntx -= (size_t)n; memmove(tx, tx + n, ntx);
                if ((tx_progress = milliseconds()) < 0)
                    return -1;
            } else if (n > 0) {
                // A positive short write has already broken STP frame boundaries.
                errno = EIO; return -1;
            } else if (!n || errno == EAGAIN || errno == EINTR) {
                retry_at = milliseconds() + 20;
            } else return -1;
        }
        if (p[1].revents & POLLIN) {
            ssize_t n = read(transport, rx + nrx, sizeof(rx) - nrx);
            if (n > 0) { nrx += (size_t)n; rx_progress = milliseconds(); }
            else if (!n) { errno = ENODEV; return -1; }
            else if (errno != EAGAIN && errno != EINTR) return -1;
        }
        if (p[0].revents & POLLOUT) {
            ssize_t n = write(master, rx, nrx);
            if (n > 0) { nrx -= (size_t)n; memmove(rx, rx + n, nrx); rx_progress = milliseconds(); }
            else if (!n) { errno = EIO; return -1; }
            else if (errno != EAGAIN && errno != EINTR) return -1;
        }
        if (!nrx) rx_progress = milliseconds();
    }
    return 0;
}

static int make_pty(int *master, int *slave) {
    char name[128]; struct termios t;
    *master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (*master < 0 || grantpt(*master) || unlockpt(*master)) return -1;
    int e = ptsname_r(*master, name, sizeof(name));
    if (e) { errno = e; return -1; }
    *slave = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (*slave < 0 || tcgetattr(*slave, &t)) return -1;
    cfmakeraw(&t); t.c_cflag |= CLOCAL | CREAD;
    if (tcsetattr(*slave, TCSANOW, &t) || ioctl(*slave, TIOCEXCL)) return -1;
    return 0;
}

/* The stock BT_open powers the function and BT_close shuts it down. Take
 * an independent process lock before calling open("/dev/stpbt"), not after.
 * Keep the inode across service restarts; unlink would split ownership. */
static int claim_bridge(const char *directory) {
    struct stat st;
    int fd = -1, saved;
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return -1;
    if (fstat(dir, &st)) goto done;
    if (st.st_uid != geteuid() || (st.st_mode & 0022)) { errno = EPERM; goto done; }
    fd = openat(dir, "teclast-bt-hci.lock",
                O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) goto done;
    if (fstat(fd, &st)) goto invalid;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() ||
        (st.st_mode & 0077)) { errno = EPERM; goto invalid; }
    if (flock(fd, LOCK_EX | LOCK_NB)) goto invalid;
    goto done;
invalid:
    saved = errno; close(fd); fd = -1; errno = saved;
done:
    saved = errno; close(dir); errno = saved;
    return fd;
}

#ifndef BT_BRIDGE_TEST
int main(int argc, char **argv) {
    int master = -1, slave = -1, transport = -1, lock = -1, result = 1, attached = 0;
    int discipline = N_HCI, normal = 0; struct stat st;
    const char *stage = "arguments";
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts("teclast-bt-hci 1: stock stpbt to Linux HCI UART H4"); return 0;
    }
    if (argc != 2 || strcmp(argv[1], "--run")) {
        fputs("usage: teclast-bt-hci --run | --version\n", stderr); return 64;
    }
    if (geteuid()) { fputs("error=root required\n", stderr); return 77; }
    struct sigaction sa = {.sa_handler = stop_bridge};
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) || sigaction(SIGTERM, &sa, NULL)) return 1;
    signal(SIGPIPE, SIG_IGN);
    stage = "claim the Bluetooth H4 bridge";
    lock = claim_bridge("/run");
    if (lock < 0) goto done;
    stage = "WMT ROM-patch service is not ready";
    if (t70_wmt_ready()) goto done;
    stage = "create HCI PTY";
    if (make_pty(&master, &slave)) goto done;
    stage = "open stock BT transport (WMT power and firmware must be ready)";
    transport = open("/dev/stpbt", O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (transport < 0) goto done;
    stage = "validate stock BT transport";
    if (fstat(transport, &st)) goto done;
    if (!S_ISCHR(st.st_mode)) { errno = ENODEV; goto done; }
    if (flock(transport, LOCK_EX | LOCK_NB)) goto done;
    stage = "attach HCI UART (load bluetooth and hci_uart first)";
    if (ioctl(slave, TIOCSETD, &discipline)) goto done;
    attached = 1;
    // Ask the HCI core to issue its standard initialization reset after attach.
    if (ioctl(slave, HCIUARTSETFLAGS, 1UL << 1) || ioctl(slave, HCIUARTSETPROTO, 0UL)) goto done;
    int index = ioctl(slave, HCIUARTGETDEVICE, 0UL);
    if (index < 0) goto done;
    stage = "verify WMT ROM-patch service after HCI registration";
    if (t70_wmt_ready()) goto done;
    printf("bt.hci=hci%d transport=stpbt protocol=H4\n", index); fflush(stdout);
    stage = "relay HCI transport";
    result = relay(master, transport, 5000) ? 1 : 0;
done:;
    int saved = errno;
    // Unregister HCI before BT_close powers the controller off.
    if (attached && ioctl(slave, TIOCSETD, &normal) && !result) { result = 1; saved = errno; }
    if (slave >= 0) close(slave);
    if (master >= 0) close(master);
    if (transport >= 0) close(transport);
    if (lock >= 0) close(lock);
    if (result) fprintf(stderr, "error=%s errno=%d\n", stage, saved);
    return result;
}
#endif
