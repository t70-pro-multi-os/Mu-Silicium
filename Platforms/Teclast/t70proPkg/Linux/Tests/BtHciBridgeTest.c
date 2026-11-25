#define BT_BRIDGE_TEST
#include "../BtHciBridge.c"
#include <assert.h>
#include <sys/socket.h>
#include <sys/wait.h>

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { perror(#x); abort(); } } while (0)
static void expect_frame(int fd, const uint8_t *bytes, size_t length) {
    uint8_t buffer[STP_MAX * 2]; struct pollfd p = {fd, POLLIN, 0};
    CHECK(poll(&p, 1, 2000) > 0);
    CHECK(recv(fd, buffer, sizeof(buffer), MSG_TRUNC) == (ssize_t)length);
    CHECK(!memcmp(buffer, bytes, length));
}
static void expect(int fd, const uint8_t *bytes, size_t length) {
    uint8_t buffer[8192]; size_t got = 0;
    CHECK(length <= sizeof(buffer));
    while (got < length) {
        struct pollfd p = {fd, POLLIN, 0};
        CHECK(poll(&p, 1, 2000) > 0);
        ssize_t n = read(fd, buffer + got, length - got);
        CHECK(n > 0); got += (size_t)n;
    }
    CHECK(!memcmp(buffer, bytes, length));
}
static pid_t start(int *host, int *device, int timeout) {
    int master = -1, slave = -1, pair[2], started[2];
    CHECK(!make_pty(&master, &slave));
    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, pair));
    CHECK(!pipe(started));
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        close(pair[1]); close(started[0]);
        struct sigaction sa = {.sa_handler = stop_bridge};
        sigemptyset(&sa.sa_mask); sigaction(SIGTERM, &sa, NULL); signal(SIGPIPE, SIG_IGN);
        if (write(started[1], "1", 1) != 1) _exit(9);
        close(started[1]);
        // Keep the raw slave open to model the kernel HCI UART endpoint.
        int rc = relay(master, pair[0], timeout);
        close(slave); close(master); close(pair[0]);
        _exit(rc ? (errno == ETIMEDOUT ? 2 : errno == EPROTO ? 3 : 4) : 0);
    }
    close(master); close(pair[0]); close(started[1]);
    char ready; CHECK(read(started[0], &ready, 1) == 1); close(started[0]);
    *host = slave; *device = pair[1]; return pid;
}
static void finish(pid_t pid, int host, int device, int status, int stop) {
    if (stop) CHECK(!kill(pid, SIGTERM));
    int actual; CHECK(waitpid(pid, &actual, 0) == pid);
    if (!WIFEXITED(actual) || WEXITSTATUS(actual) != status)
        fprintf(stderr, "child status=%d expected exit=%d after check=%u\n", actual, status, checks);
    CHECK(WIFEXITED(actual) && WEXITSTATUS(actual) == status);
    close(host); close(device);
}
static void parser(void) {
    uint8_t data[STP_MAX * 2] = {1, 3, 12, 0};
    CHECK(frame_length(data, 4) == 4);
    data[3] = 255;
    for (size_t n = 0; n < 259; n++) CHECK(frame_length(data, n) == 0);
    CHECK(frame_length(data, 259) == 259);
    data[0] = 2; data[3] = 0xfb; data[4] = 7;
    CHECK(frame_length(data, 2048) == 2048);
    data[3] = 0xfc; CHECK(frame_length(data, sizeof(data)) < 0 && errno == EMSGSIZE);
    data[0] = 3; data[3] = 7; CHECK(frame_length(data, 11) == 11);
    data[0] = 5; data[3] = 8; data[4] = 0xc0; CHECK(frame_length(data, 13) == 13);
    data[0] = 4; CHECK(frame_length(data, 8) < 0 && errno == EPROTO);
    for (int type = 0; type < 256; type++) {
        if (type == 1 || type == 2 || type == 3 || type == 5) continue;
        data[0] = (uint8_t)type;
        CHECK(frame_length(data, sizeof(data)) < 0 && errno == EPROTO);
    }
}
static void exclusive_bridge(void) {
    char directory[] = "/tmp/t70-h4-owner-XXXXXX", name[128], alias[128];
    CHECK(mkdtemp(directory) != NULL);
    CHECK(snprintf(name, sizeof(name), "%s/teclast-bt-hci.lock", directory) > 0);
    CHECK(snprintf(alias, sizeof(alias), "%s/other", directory) > 0);
    int held = claim_bridge(directory);
    CHECK(held >= 0);
    CHECK(fcntl(held, F_GETFD) & FD_CLOEXEC);
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        close(held);
        int second = claim_bridge(directory);
        _exit(second < 0 && errno == EWOULDBLOCK ? 0 : 1);
    }
    int status; CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    struct stat a, b;
    CHECK(fstat(held, &a) == 0 && close(held) == 0);
    int next = claim_bridge(directory);
    CHECK(next >= 0 && fstat(next, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino);
    CHECK(close(next) == 0);
    CHECK(link(name, alias) == 0);
    CHECK(claim_bridge(directory) == -1 && errno == EPERM);
    CHECK(unlink(alias) == 0 && unlink(name) == 0);
    CHECK(symlink("/dev/null", name) == 0);
    CHECK(claim_bridge(directory) == -1 && errno == ELOOP);
    CHECK(unlink(name) == 0);
    CHECK(mkfifo(name, 0600) == 0);
    CHECK(claim_bridge(directory) == -1 && errno == EPERM);
    CHECK(unlink(name) == 0);
    CHECK(chmod(directory, 0777) == 0);
    CHECK(claim_bridge(directory) == -1 && errno == EPERM);
    CHECK(access(name, F_OK) == -1 && errno == ENOENT);
    CHECK(rmdir(directory) == 0);
}
int main(void) {
    alarm(20);
    exclusive_bridge();
    CHECK(t70_wmt_ready() == -1);
    parser();
    int host, device;
    pid_t pid = start(&host, &device, 1500);
    const uint8_t command[] = {1, 3, 12, 0}, event[] = {4, 14, 4, 1, 3, 12, 0};
    for (size_t n = 0; n < sizeof(command); n++) {
        CHECK(write(host, command + n, 1) == 1);
        if (n < sizeof(command) - 1) {
            struct pollfd p = {device, POLLIN, 0}; CHECK(!poll(&p, 1, 15));
        }
    }
    expect_frame(device, command, sizeof(command));
    uint8_t batch[sizeof(command) * 50];
    for (size_t i = 0; i < 50; i++) memcpy(batch + i * sizeof(command), command, sizeof(command));
    CHECK(write(host, batch, sizeof(batch)) == sizeof(batch));
    // SOCK_SEQPACKET observes whether each STP write contains exactly one frame.
    for (size_t i = 0; i < 50; i++) expect_frame(device, command, sizeof(command));
    CHECK(write(device, event, 2) == 2);
    CHECK(write(device, event + 2, sizeof(event) - 2) == sizeof(event) - 2);
    expect(host, event, sizeof(event));
    const uint8_t reset[] = {4, 16, 1, 0};
    CHECK(write(device, reset, sizeof(reset)) == sizeof(reset)); expect(host, reset, sizeof(reset));
    finish(pid, host, device, 0, 1);
    pid = start(&host, &device, 1000);
    uint8_t backlog[500 * sizeof(command)];
    for (size_t i = 0; i < 500; i++) memcpy(backlog + i * sizeof(command), command, sizeof(command));
    CHECK(write(host, backlog, sizeof(backlog)) == sizeof(backlog));
    usleep(100000);
    for (size_t i = 0; i < 500; i++) expect_frame(device, command, sizeof(command));
    finish(pid, host, device, 0, 1);
    pid = start(&host, &device, 100);
    CHECK(write(host, backlog, sizeof(backlog)) == sizeof(backlog));
    finish(pid, host, device, 2, 0);
    pid = start(&host, &device, 100);
    CHECK(write(host, command, 1) == 1);
    finish(pid, host, device, 2, 0);
    // New fragments do not reset the deadline for an undelivered frame.
    pid = start(&host, &device, 1200);
    const uint8_t incomplete[] = {2, 1, 0, 0x10, 0, 8, 9};
    CHECK(write(host, incomplete, 5) == 5);
    usleep(400000);
    CHECK(write(host, incomplete + 5, 1) == 1);
    usleep(400000);
    CHECK(write(host, incomplete + 6, 1) == 1);
    struct pollfd expired = {device, POLLIN, 0};
    CHECK(poll(&expired, 1, 700) > 0 && (expired.revents & POLLHUP));
    finish(pid, host, device, 2, 0);
    pid = start(&host, &device, 1000);
    uint8_t invalid = 0xff; CHECK(write(host, &invalid, 1) == 1);
    finish(pid, host, device, 3, 0);
    pid = start(&host, &device, 1000);
    close(device); device = -1;
    finish(pid, host, device, 4, 0);
    pid = start(&host, &device, 1000);
    finish(pid, host, device, 0, 1);
    printf("BT H4 PTY bridge: %u checks passed\n", checks);
    return 0;
}
