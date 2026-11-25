// SPDX-License-Identifier: BSD-2-Clause-Patent
// Run the actual CLI while replacing only its device operations.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static int mock_open(const char *path, int flags, ...);
static int mock_fstat(int fd, struct stat *state);
static int mock_flock(int fd, int operation);
static int mock_ioctl(int fd, unsigned long command, void *argument);
static int mock_close(int fd);

#define main camera_main
#define open mock_open
#define fstat mock_fstat
#define flock mock_flock
#define ioctl mock_ioctl
#define close mock_close
#include "../CameraProbe.c"
#undef main
#undef open
#undef fstat
#undef flock
#undef ioctl
#undef close

enum scenario {
    Match, NoMatch, OpenFailed, StatFailed, WrongNode, LockBusy, Removed,
    Interrupted, DriverBusy, WrongLength, MissingTerminator, BadName, BadId, CloseFailed,
    PartialNoMatch, BadLockNode,
};

enum { FakeFd = 45, LockFd = 46 };
static enum scenario scenario;
static uint32_t expected_index;
static unsigned calls;
static char steps[12];

static void record(char step)
{
    assert(calls < sizeof(steps) - 1);
    steps[calls++] = step;
}

static int mock_open(const char *path, int flags, ...)
{
    if (!strcmp(path, "/run/teclast-camera-probe.lock")) {
        record('A');
        assert(flags == (O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        va_list args;
        va_start(args, flags);
        assert(va_arg(args, int) == 0600);
        va_end(args);
        return LockFd;
    }
    assert(!strcmp(steps, "AVL") && "camera device opened without the separate probe lock");
    record('O');
    assert(!strcmp(path, "/dev/kd_camera_hw"));
    assert(flags == (O_RDWR | O_CLOEXEC | O_NOCTTY | O_NOFOLLOW));
    if (scenario == OpenFailed) {
        errno = ENOENT;
        return -1;
    }
    return FakeFd;
}

static int mock_fstat(int fd, struct stat *state)
{
    if (fd == LockFd) {
        record('V');
        state->st_mode = (scenario == BadLockNode ? S_IFIFO : S_IFREG) | 0600;
        state->st_uid = geteuid();
        state->st_nlink = 1;
        return 0;
    }
    record('S');
    assert(fd == FakeFd);
    if (scenario == StatFailed) {
        errno = EIO;
        return -1;
    }
    state->st_mode = scenario == WrongNode ? S_IFREG : S_IFCHR;
    return 0;
}

static int mock_flock(int fd, int operation)
{
    record('L');
    assert(fd == LockFd && operation == (LOCK_EX | LOCK_NB));
    if (scenario == LockBusy) {
        errno = EWOULDBLOCK;
        return -1;
    }
    return 0;
}

static int mock_ioctl(int fd, unsigned long command, void *argument)
{
    record('I');
    assert(fd == FakeFd && command == 0xc018690fUL);

    // Decode the stock offsets independently of the client's struct types.
    const char *packet = argument;
    uint32_t index, feature, *length;
    char *response;
    memcpy(&index, packet, 4);
    memcpy(&feature, packet + 4, 4);
    memcpy(&response, packet + 8, 8);
    memcpy(&length, packet + 16, 8);
    const char empty[36] = {0};
    assert(index == expected_index && feature == 0xc23 && *length == sizeof(empty));
    assert(memcmp(response, empty, sizeof(empty)) == 0);

    if (scenario == PartialNoMatch) {
        response[5] = 'x';
        return 0;
    }
    if (scenario == NoMatch)
        return 0;
    if (scenario == Removed || scenario == Interrupted) {
        errno = scenario == Removed ? ENODEV : EINTR;
        return -1;
    }
    if (scenario == DriverBusy)
        return 1;

    const uint32_t id = scenario == BadId ? UINT32_MAX : 0x886a;
    memcpy(response, &id, sizeof(id));
    const char name[] = "ov8856_mipi_raw";
    memcpy(response + 4, name, sizeof(name));
    if (scenario == WrongLength)
        *length = 4;
    if (scenario == MissingTerminator)
        memset(response + 4, 'a', 32);
    if (scenario == BadName)
        response[4] = '\n';
    return 0;
}

static int mock_close(int fd)
{
    if (fd == LockFd) {
        record('U');
        return 0;
    }
    record('C');
    assert(fd == FakeFd);
    if (scenario == CloseFailed) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static void run(enum scenario mode, char *slot, uint32_t index, int status, const char *actions)
{
    scenario = mode;
    expected_index = index;
    calls = 0;
    memset(steps, 0, sizeof(steps));

    FILE *stream = tmpfile();
    int saved_stdout = dup(STDOUT_FILENO);
    assert(stream && saved_stdout >= 0);
    assert(dup2(fileno(stream), STDOUT_FILENO) == STDOUT_FILENO);
    char *argv[] = {"teclast-camera", "probe", slot, NULL};
    assert(camera_main(3, argv) == status);
    assert(fflush(stdout) == 0 && dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
    assert(close(saved_stdout) == 0);

    rewind(stream);
    char output[128];
    if (status == 0) {
        char expected[128];
        snprintf(expected, sizeof(expected),
                 "index=%" PRIu32 " catalog_id=0x0000886a driver=ov8856_mipi_raw\n", index);
        assert(fgets(output, sizeof(output), stream) && strcmp(output, expected) == 0);
    }
    assert(fgetc(stream) == EOF && fclose(stream) == 0);
    assert(strcmp(steps, actions) == 0);
}

int main(void)
{
    run(Match, "0", 0, 0, "AVLOSICU");
    run(Match, "1", 1, 0, "AVLOSICU");
    run(Match, "2", 2, 0, "AVLOSICU");
    run(NoMatch, "2", 2, 2, "AVLOSICU");
    run(PartialNoMatch, "2", 2, 1, "AVLOSICU");
    run(OpenFailed, "0", 0, 1, "AVLOU");
    run(StatFailed, "0", 0, 1, "AVLOSCU");
    run(WrongNode, "0", 0, 1, "AVLOSCU");
    run(LockBusy, "1", 1, 1, "AVLU");
    run(BadLockNode, "0", 0, 1, "AVU");
    run(Removed, "1", 1, 1, "AVLOSICU");
    run(Interrupted, "0", 0, 1, "AVLOSICU");
    run(DriverBusy, "0", 0, 1, "AVLOSICU");
    run(WrongLength, "0", 0, 1, "AVLOSICU");
    run(MissingTerminator, "0", 0, 1, "AVLOSICU");
    run(BadName, "0", 0, 1, "AVLOSICU");
    run(BadId, "0", 0, 1, "AVLOSICU");
    run(CloseFailed, "0", 0, 1, "AVLOSICU");
    run(Match, "", 0, 64, "");
    run(Match, "3", 0, 64, "");
    run(Match, "02", 0, 64, "");
    puts("21 stock camera probe cases passed");
    return 0;
}
