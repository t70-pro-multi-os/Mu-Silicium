// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the command-line entry point with the stock per-file ABI mocked.
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int mock_open(const char *path, int flags);
static int mock_fstat(int fd, struct stat *state);
static int mock_close(int fd);
static int mock_ioctl(int fd, unsigned long op, void *packet);
static ssize_t mock_write(int fd, const void *buffer, size_t bytes);
static ssize_t mock_read(int fd, void *buffer, size_t bytes);
static int mock_poll(struct pollfd *fds, nfds_t count, int timeout);
static int mock_clock_gettime(clockid_t clock, struct timespec *time);

#define main sensor_main
#define open mock_open
#define fstat mock_fstat
#define close mock_close
#define ioctl mock_ioctl
#define write mock_write
#define read mock_read
#define poll mock_poll
#define clock_gettime mock_clock_gettime
#include "../HfSensors.c"
#undef main
#undef open
#undef fstat
#undef close
#undef ioctl
#undef write
#undef read
#undef poll
#undef clock_gettime

enum Scenario {
    Inventory, Empty, Samples, Timeout, LateReady, LateRead, Removed, Cancel, ShortEvent, AbiError,
    EnableFailed, DisableFailed, Unregistered, WrongNode, BadArguments,
};

enum { SensorFd = 99, SensorType = 5 };
static enum Scenario scenario;
static unsigned opens, closes, status_calls, info_calls, enables, disables, polls, reads;
static int64_t milliseconds;

static int mock_open(const char *path, int flags)
{
    assert(!strcmp(path, "/dev/hf_manager"));
    assert(flags == ((scenario == Inventory || scenario == Empty ? O_RDONLY : O_RDWR) |
                     O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW | O_NOCTTY));
    assert(opens++ == 0);
    return SensorFd;
}

static int mock_fstat(int fd, struct stat *state)
{
    assert(fd == SensorFd);
    state->st_mode = scenario == WrongNode ? S_IFREG : S_IFCHR;
    return 0;
}

static int mock_close(int fd)
{
    assert(fd == SensorFd && closes++ == 0);
    return 0;
}

static int mock_ioctl(int fd, unsigned long op, void *payload)
{
    assert(fd == SensorFd);
    uint8_t *packet = payload;
    if (op == 0xc0086101) {
        unsigned type = scenario == Inventory || scenario == Empty ? status_calls : SensorType;
        const uint8_t input[8] = { type };
        assert(!memcmp(input, packet, sizeof(input)));
        ++status_calls;
        packet[4] = (scenario != Empty && scenario != Unregistered &&
                     (type == SensorType || type == 68));
        return 0;
    }
    assert(scenario == Inventory && op == 0xc02c6106);
    unsigned type = info_calls++ ? 68 : SensorType;
    uint8_t expected[44] = { type };
    assert(!memcmp(expected, packet, sizeof(expected)));
    packet[4] = type;
    packet[8] = 16;
    memcpy(packet + 12, "fixture\033sensor", 15);
    memcpy(packet + 28, "0123456789012345", 16);
    return 0;
}

static ssize_t mock_write(int fd, const void *buffer, size_t bytes)
{
    assert(fd == SensorFd && bytes == 52);
    const uint8_t *command = buffer;
    if (enables == 0) {
        const uint8_t expected[52] = { SensorType, 1, 16, 0, 0x00, 0xe1, 0xf5, 0x05 };
        assert(!memcmp(command, expected, sizeof(expected)));
        ++enables;
        if (scenario == AbiError)
            return (ssize_t)bytes;
        if (scenario == EnableFailed) {
            errno = EIO;
            return -1;
        }
        return 0;
    }
    const uint8_t expected[52] = { SensorType };
    assert(!memcmp(command, expected, sizeof(expected)) && disables++ == 0);
    if (scenario == DisableFailed) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int mock_clock_gettime(clockid_t clock, struct timespec *time)
{
    assert(clock == CLOCK_MONOTONIC);
    time->tv_sec = milliseconds / 1000;
    time->tv_nsec = (milliseconds % 1000) * 1000000;
    return 0;
}

static int mock_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    assert(count == 1 && fds[0].fd == SensorFd && fds[0].events == POLLIN);
    assert(enables == 1 && disables == 0 && timeout > 0 && timeout <= 200);
    ++polls;
    if (scenario == Timeout) {
        milliseconds += timeout;
        return 0;
    }
    if (scenario == LateReady)
        milliseconds += 502;
    if (scenario == Cancel) {
        assert(raise(SIGTERM) == 0);
        errno = EINTR;
        return -1;
    }
    fds[0].revents = scenario == Removed ? POLLHUP : POLLIN;
    return 1;
}

static ssize_t mock_read(int fd, void *buffer, size_t bytes)
{
    assert(fd == SensorFd && bytes == 8 * 76);
    assert(enables == 1 && disables == 0);
    ++reads;
    if (scenario == LateRead) {
        milliseconds += 502;
        struct HfEvent event = { .type = SensorType };
        memcpy(buffer, &event, sizeof(event));
        return sizeof(event);
    }
    if (scenario == ShortEvent)
        return 75;
    assert(scenario == Samples || scenario == DisableFailed);
    if (reads == 1)
        return 0; // Stock read returns zero when the FIFO has emptied.
    if (reads == 2) {
        errno = EAGAIN;
        return -1;
    }
    // Two unrelated events around two data events for our registered sensor.
    uint8_t data[4][76] = {
        { [8] = 68 },
        { [0] = 10, [8] = SensorType, [9] = 3, [12] = 1 },
        { [8] = SensorType, [10] = 1 },
        { [0] = 11, [8] = SensorType, [9] = 2, [12] = 0xff, [13] = 0xff,
          [14] = 0xff, [15] = 0xff, [72] = 7 },
    };
    memcpy(buffer, data, sizeof(data));
    return sizeof(data);
}

static void run(enum Scenario test)
{
    char *list[] = { "teclast-sensors", "list", NULL };
    char *capture[] = { "teclast-sensors", "read", "5", "2", "501", NULL };
    char *invalid[] = { "teclast-sensors", "read", "69", NULL };
    scenario = test;
    opens = closes = status_calls = info_calls = enables = disables = polls = reads = 0;
    milliseconds = 0;
    stopped = 0;

    FILE *saved = stdout;
    char *output = NULL;
    size_t length = 0;
    stdout = open_memstream(&output, &length);
    assert(stdout);
    int result;
    if (test == Inventory || test == Empty)
        result = sensor_main(2, list);
    else if (test == BadArguments)
        result = sensor_main(3, invalid);
    else
        result = sensor_main(5, capture);
    assert(!fclose(stdout));
    stdout = saved;

    assert(result == (test <= Samples ? 0 : test == BadArguments ? 2 : 1));
    assert(opens == (test != BadArguments) && closes == opens);
    if (test == Inventory || test == Empty) {
        assert(status_calls == 69 && info_calls == (test == Inventory ? 2U : 0));
        assert(!enables && !disables && !polls && !reads);
        const char *expected = test == Inventory ?
            "5 gain=16 name=fixture?sensor vendor=0123456789012345\n"
            "68 gain=16 name=fixture?sensor vendor=0123456789012345\n" :
            "No sensors registered\n";
        assert(!strcmp(output, expected));
    } else if (test <= DisableFailed) {
        assert(status_calls == 1 && !info_calls && enables == 1);
        assert(disables == (test != EnableFailed && test != AbiError));
        if (test == Samples || test == DisableFailed) {
            assert(polls == 3 && reads == 3);
            assert(!strcmp(output,
               "5 timestamp_ns=10 accuracy=3 data=1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n"
               "5 timestamp_ns=11 accuracy=2 data=-1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,7\n"));
        } else {
            assert(!length && reads == (test == ShortEvent || test == LateRead));
            assert(polls == (test == Timeout ? 3U :
                             test == AbiError || test == EnableFailed ? 0U : 1U));
        }
    } else {
        assert(status_calls == (test == Unregistered));
        assert(!enables && !disables && !polls && !reads && !length);
    }
    free(output);
}

int main(void)
{
    for (enum Scenario test = Inventory; test <= BadArguments; ++test)
        run(test);
    puts("PASS: 15 HF sensor cases using the exact T70 Pro request and event byte layouts");
    return 0;
}
