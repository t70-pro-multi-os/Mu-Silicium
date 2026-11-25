// SPDX-License-Identifier: BSD-2-Clause-Patent
// Sensor subscriptions over the T70 Pro stock Linux hf_manager.ko interface.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { HfLastType = 68, HfDisable = 0, HfEnable = 1, HfData = 0 };

struct HfStatus {
    uint8_t type, reserved[3], registered, tail[3];
};

struct HfInfo {
    uint8_t type, reserved[3], sensor_type, padding[3];
    uint32_t gain;
    char name[16], vendor[16];
};

struct HfCommand {
    uint8_t type, action, length, padding, data[48];
};

struct HfEvent {
    uint8_t timestamp[8], type, accuracy, action, padding;
    int32_t words[16];
};

/* Layouts checked against hf_manager_ioctl, hf_manager_write and hf_manager_read
 * in the original board module. The two ioctls use different packet lengths. */
_Static_assert(sizeof(struct HfStatus) == 8 && offsetof(struct HfStatus, registered) == 4, "status ABI");
_Static_assert(sizeof(struct HfInfo) == 44 && offsetof(struct HfInfo, gain) == 8, "info ABI");
_Static_assert(sizeof(struct HfCommand) == 52 && offsetof(struct HfCommand, data) == 4, "command ABI");
_Static_assert(sizeof(struct HfEvent) == 76 && offsetof(struct HfEvent, words) == 12, "event ABI");

static volatile sig_atomic_t stopped;

static void stop_read(int signal_number)
{
    (void)signal_number;
    stopped = 1;
}

static int registered(int fd, uint8_t type)
{
    struct HfStatus packet = { .type = type };
    if (ioctl(fd, _IOWR('a', 1, struct HfStatus), &packet) < 0)
        return -1;
    if (packet.type != type || packet.registered > 1) {
        errno = EPROTO;
        return -1;
    }
    return packet.registered;
}

static void print_field(const char *field, size_t size)
{
    for (size_t i = 0; i < size && field[i]; ++i)
        putchar(field[i] >= ' ' && field[i] <= '~' ? field[i] : '?');
}

static int list_sensors(int fd)
{
    unsigned count = 0;
    for (unsigned type = 0; type <= HfLastType; ++type) {
        int found = registered(fd, type);
        if (found < 0)
            return -1;
        if (!found)
            continue;
        struct HfInfo packet = { .type = type };
        if (ioctl(fd, _IOWR('a', 6, struct HfInfo), &packet) < 0)
            return -1;
        if (packet.type != type || packet.sensor_type != type || !packet.gain) {
            errno = EPROTO;
            return -1;
        }
        printf("%u gain=%" PRIu32 " name=", type, packet.gain);
        print_field(packet.name, sizeof(packet.name));
        fputs(" vendor=", stdout);
        print_field(packet.vendor, sizeof(packet.vendor));
        putchar('\n');
        ++count;
    }
    if (!count)
        puts("No sensors registered");
    return ferror(stdout) ? -1 : fflush(stdout);
}

static int command_sensor(int fd, uint8_t type, uint8_t action)
{
    struct HfCommand command = { .type = type, .action = action };
    if (action == HfEnable) {
        const int64_t batch[] = { 100000000, 0 }; // 100 ms interval, no batching delay
        command.length = sizeof(batch);
        memcpy(command.data, batch, sizeof(batch));
    }
    // The stock write handler returns the operation status (0 on success).
    ssize_t result = write(fd, &command, sizeof(command));
    if (result > 0)
        errno = EPROTO;
    return result == 0 ? 0 : -1;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int print_sample(const struct HfEvent *event)
{
    int64_t timestamp;
    memcpy(&timestamp, event->timestamp, sizeof(timestamp));
    printf("%u timestamp_ns=%" PRId64 " accuracy=%u data=", (unsigned)event->type,
           timestamp, (unsigned)event->accuracy);
    for (size_t i = 0; i < sizeof(event->words) / sizeof(event->words[0]); ++i)
        printf("%s%" PRId32, i ? "," : "", event->words[i]);
    putchar('\n');
    return ferror(stdout) ? -1 : fflush(stdout);
}

static int read_samples(int fd, uint8_t type, unsigned count, unsigned timeout_ms)
{
    int64_t now = monotonic_ms();
    if (now < 0)
        return -1;
    int64_t deadline = now + timeout_ms;
    while (count && !stopped) {
        now = monotonic_ms();
        if (now < 0)
            return -1;
        int64_t remaining = deadline - now;
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }

        struct pollfd sensor = { .fd = fd, .events = POLLIN };
        // Limit the wait if a signal arrived just before entering poll.
        int ready = poll(&sensor, 1, remaining > 200 ? 200 : (int)remaining);
        if (ready < 0 && errno != EINTR)
            return -1;
        if (ready <= 0 || stopped)
            continue;
        if (sensor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = ENODEV;
            return -1;
        }
        if (!(sensor.revents & POLLIN))
            continue;

        // A ready poll can return after the deadline. Do not consume FIFO
        // entries if this capture has already expired.
        now = monotonic_ms();
        if (now < 0)
            return -1;
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }

        struct HfEvent events[8];
        ssize_t bytes = read(fd, events, sizeof(events));
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            return -1;
        }
        if ((size_t)bytes > sizeof(events) || (size_t)bytes % sizeof(events[0])) {
            errno = EPROTO;
            return -1;
        }
        for (size_t i = 0; i < (size_t)bytes / sizeof(events[0]) && count && !stopped; ++i) {
            if (events[i].type != type || events[i].action != HfData)
                continue;
            // Reading or processing a batch can cross the deadline too.
            // Never print a sample after that point.
            now = monotonic_ms();
            if (now < 0)
                return -1;
            if (now >= deadline) {
                errno = ETIMEDOUT;
                return -1;
            }
            if (print_sample(&events[i]))
                return -1;
            --count;
        }
    }
    if (!count)
        return 0;
    errno = ECANCELED;
    return -1;
}

static int capture(int fd, uint8_t type, unsigned count, unsigned timeout_ms)
{
    int found = registered(fd, type);
    if (found <= 0) {
        if (!found)
            errno = ENOENT;
        return -1;
    }

    struct sigaction cancel = { .sa_handler = stop_read };
    struct sigaction ignore_pipe = { .sa_handler = SIG_IGN };
    if (sigemptyset(&cancel.sa_mask) || sigemptyset(&ignore_pipe.sa_mask) ||
        sigaction(SIGINT, &cancel, NULL) || sigaction(SIGTERM, &cancel, NULL) ||
        sigaction(SIGPIPE, &ignore_pipe, NULL))
        return -1;
    if (stopped) {
        errno = ECANCELED;
        return -1;
    }
    if (command_sensor(fd, type, HfEnable))
        return -1;

    int result = read_samples(fd, type, count, timeout_ms);
    int read_error = errno;
    int disabled = command_sensor(fd, type, HfDisable);
    if (result)
        errno = read_error;
    return result ? result : disabled;
}

static int decimal(const char *text, unsigned lower, unsigned upper, unsigned *value)
{
    char *end;
    errno = 0;
    unsigned long number = strtoul(text, &end, 10);
    if (*text < '0' || *text > '9' || errno || *end || number < lower || number > upper)
        return -1;
    *value = (unsigned)number;
    return 0;
}

int main(int argc, char **argv)
{
    unsigned type = 0, count = 1, timeout_ms = 3000;
    int list = argc == 2 && !strcmp(argv[1], "list");
    if (!list && (argc < 3 || argc > 5 || strcmp(argv[1], "read") ||
        decimal(argv[2], 0, HfLastType, &type) ||
        (argc >= 4 && decimal(argv[3], 1, 1024, &count)) ||
        (argc >= 5 && decimal(argv[4], 1, 60000, &timeout_ms)))) {
        fputs("Usage: teclast-sensors list | read TYPE [COUNT [TIMEOUT_MS]]\n", stderr);
        return 2;
    }

    int fd = open("/dev/hf_manager", (list ? O_RDONLY : O_RDWR) |
                  O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW | O_NOCTTY);
    if (fd < 0) {
        perror("/dev/hf_manager");
        return 1;
    }

    struct stat info;
    int result = fstat(fd, &info);
    if (result == 0 && !S_ISCHR(info.st_mode)) {
        errno = ENODEV;
        result = -1;
    }
    if (result == 0)
        result = list ? list_sensors(fd) : capture(fd, type, count, timeout_ms);
    int operation_error = errno;
    // Last-close also destroys this file's client, including on disable failure.
    int closed = close(fd);
    if (result) {
        errno = operation_error;
        perror(list ? "list sensors" : "read sensor");
    } else if (closed) {
        perror("close /dev/hf_manager");
    }
    return result || closed ? 1 : 0;
}
