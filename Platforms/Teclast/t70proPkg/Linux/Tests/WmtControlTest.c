#define WMT_CONTROL_TEST
#include "../WmtControl.c"
#include <sys/socket.h>
#include <sys/wait.h>

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { perror(#x); abort(); } } while (0)

static struct rom_patch published[WMT_PATCH_COUNT];
static int published_count, fail_publish, fake_chip, fake_adie, fake_revision;
static int init_calls, init_failure = -1, adie_queries;
static int fake_call(int fd, unsigned long request, unsigned long value) {
    if (request == WMT_CHIP_INFO) {
        CHECK(fd == 12); CHECK(value == 0 || value == 2);
        return value ? fake_revision : fake_chip;
    }
    if (request == WMT_DETECT_ADIE) { CHECK(fd == 13); CHECK(value == 0); adie_queries++; return fake_adie; }
    if (request == WMT_CONFIG_PATH || request == WMT_SET_MODE || request == WMT_SET_LAST_CLOSE) {
        CHECK(fd == 12);
        if (!init_calls) { CHECK(request == WMT_CONFIG_PATH); for (unsigned n = 0; n < 256; n++) CHECK(!((char *)value)[n]); }
        if (init_calls == 1) { CHECK(request == WMT_SET_MODE); CHECK(value == 0x23); }
        if (init_calls == 2) { CHECK(request == WMT_SET_LAST_CLOSE); CHECK(!value); }
        if (init_calls++ == init_failure) { errno = EIO; return -1; }
        return 0;
    }
    CHECK(fd == 12); CHECK(request == WMT_ROM_PATCH);
    CHECK(published_count < (int)WMT_PATCH_COUNT);
    if (published_count == fail_publish) { errno = EIO; return -1; }
    published[published_count++] = *(struct rom_patch *)value;
    return 0;
}
static void defaults(void) {
    fake_chip = 0x6789; fake_adie = 0x6631; fake_revision = 0;
    published_count = 0; fail_publish = -1; init_calls = 0; init_failure = -1; adie_queries = 0;
}
static unsigned platform_step;
static int platform_failure, platform_failure_value, platform_soc;
static int platform_call(int fd, unsigned long request, unsigned long value) {
    const unsigned long requests[] = {0x8004770aUL, 0x80047703UL, 0x40047701UL, 0x80047704UL};
    const unsigned long values[] = {0, 0, 0x6789, 0};
    CHECK(fd == 13); CHECK(platform_step < 4);
    CHECK(request == requests[platform_step]); CHECK(value == values[platform_step]);
    unsigned step = platform_step++;
    if ((int)step == platform_failure) { errno = EACCES; return platform_failure_value; }
    return step == 1 ? platform_soc : 0;
}
static void platform_test(void) {
    struct wmt_context ctx = {.fd = -1, .detect = 13, .call = platform_call};
    platform_step = 0; platform_failure = -1; platform_soc = 0x6789;
    CHECK(!initialize_platform(&ctx)); CHECK(platform_step == 4);
    for (int step = 0; step < 4; step++) {
        for (int result = -1; result <= 1; result += 2) {
            platform_step = 0; platform_failure = step; platform_failure_value = result;
            CHECK(initialize_platform(&ctx) == -1); CHECK(platform_step == (unsigned)step + 1);
            if (step != 1) CHECK(errno == (result < 0 ? EACCES : EIO));
        }
    }
    platform_step = 0; platform_failure = -1; platform_soc = 0x6788;
    CHECK(initialize_platform(&ctx) == -1); CHECK(errno == ENODEV); CHECK(platform_step == 2);
}
static void fixture(const char *directory, const char *radio, const char *variant,
                    uint8_t type, uint8_t revision, uint8_t address_high, size_t length) {
    char path[512];
    CHECK(snprintf(path, sizeof(path), "%s/soc2_2_ram_%s_%s_hdr.bin", directory, radio, variant) > 0);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600); CHECK(fd >= 0);
    uint8_t data[64] = {0}; data[23] = revision;
    data[24] = 0x11; data[25] = 0x12; data[26] = 0x34; data[27] = address_high; data[31] = type;
    CHECK(length <= sizeof(data)); CHECK(write(fd, data, length) == (ssize_t)length); CHECK(!close(fd));
}
static void valid_fixtures(const char *directory, const char *variant) {
    fixture(directory, "bt", variant, 0, 0, 0xf0, 64);
    fixture(directory, "wifi", variant, 3, 0, 0xf0, 64);
    fixture(directory, "mcu", variant, 4, 0, 0xf0, 64);
}
static void check_records(const struct rom_patch records[WMT_PATCH_COUNT], const char *variant) {
    const unsigned types[] = {0, 3, 4};
    const char *const radios[] = {"bt", "wifi", "mcu"};
    for (unsigned i = 0; i < WMT_PATCH_COUNT; i++) {
        char expected[256]; snprintf(expected, sizeof(expected), "soc2_2_ram_%s_%s_hdr.bin", radios[i], variant);
        CHECK(records[i].type == types[i]); CHECK(!strcmp(records[i].name, expected));
        CHECK(!records[i].address[0]); CHECK(records[i].address[3] >= 0x10);
        CHECK(records[i].name[255] == 0);
    }
}
static void catalog_test(const char *shipped) {
    char directory[] = "/tmp/t70-wmt-test-XXXXXX";
    CHECK(mkdtemp(directory));
    valid_fixtures(directory, "1d_1"); valid_fixtures(directory, "1e_1");
    struct rom_patch records[WMT_PATCH_COUNT];
    struct wmt_context ctx = {.fd = 12, .detect = 13, .firmware = directory, .call = fake_call, .adie = 0x6631};
    for (unsigned repeat = 0; repeat < 8; repeat++) {
        defaults(); ctx.adie = 0x6631; CHECK(!publish_catalog(&ctx)); CHECK(!adie_queries); CHECK(published_count == 3); check_records(published, "1d_1");
        defaults(); ctx.adie = 0x6635; CHECK(!publish_catalog(&ctx)); CHECK(!adie_queries); check_records(published, "1e_1");
    }
    CHECK(!load_catalog(shipped, 0x6631, 0, records)); check_records(records, "1d_1");
    CHECK(!load_catalog(shipped, 0x6635, 0, records)); check_records(records, "1e_1");
    defaults(); CHECK(!configure_control(&ctx)); CHECK(init_calls == 3); CHECK(adie_queries == 1); CHECK(ctx.adie == 0x6631);
    CHECK(!publish_catalog(&ctx)); CHECK(adie_queries == 1);
    for (int i = 0; i < 3; i++) {
        defaults(); init_failure = i; CHECK(configure_control(&ctx) == -1); CHECK(init_calls == i + 1);
    }
    defaults(); fake_chip = 0x1234; CHECK(configure_control(&ctx) == -1); CHECK(!init_calls);
    defaults(); fake_chip = 0x1234; CHECK(publish_catalog(&ctx) == -ENODEV); CHECK(!published_count);
    defaults(); fake_adie = -1; CHECK(configure_control(&ctx) == -1); CHECK(!published_count);
    defaults(); fake_adie = 0x6637; CHECK(configure_control(&ctx) == -1); CHECK(!published_count);
    ctx.adie = 0x6637; CHECK(publish_catalog(&ctx) == -ENODEV); ctx.adie = 0x6631;
    defaults(); fake_revision = -1; CHECK(publish_catalog(&ctx) == -EIO); CHECK(!published_count);
    defaults(); fake_revision = 1; CHECK(publish_catalog(&ctx) == -EINVAL); CHECK(!published_count);
    for (int i = 0; i < 3; i++) {
        defaults(); fail_publish = i; CHECK(publish_catalog(&ctx) == -EIO); CHECK(published_count == i);
        fail_publish = -1; published_count = 0; CHECK(!publish_catalog(&ctx)); CHECK(published_count == 3);
    }
    defaults(); fixture(directory, "mcu", "1d_1", 5, 0, 0xf0, 64);
    CHECK(publish_catalog(&ctx) == -EINVAL); CHECK(!published_count);
    fixture(directory, "mcu", "1d_1", 0, 0, 0xf0, 64);
    CHECK(publish_catalog(&ctx) == -EINVAL); CHECK(!published_count);
    fixture(directory, "mcu", "1d_1", 4, 0, 0x0f, 64);
    CHECK(publish_catalog(&ctx) == -EINVAL); CHECK(!published_count);
    fixture(directory, "mcu", "1d_1", 4, 0, 0xf0, 31);
    CHECK(publish_catalog(&ctx) == -EINVAL); CHECK(!published_count);
    char file[512]; snprintf(file, sizeof(file), "%s/soc2_2_ram_mcu_1d_1_hdr.bin", directory);
    CHECK(!unlink(file)); CHECK(publish_catalog(&ctx) == -ENOENT); CHECK(!published_count);
    CHECK(!symlink("soc2_2_ram_mcu_1e_1_hdr.bin", file));
    CHECK(publish_catalog(&ctx) == -ELOOP); CHECK(!published_count); CHECK(!unlink(file));
    CHECK(!mkfifo(file, 0600)); CHECK(publish_catalog(&ctx) == -EINVAL); CHECK(!published_count); CHECK(!unlink(file));
    valid_fixtures(directory, "1d_1");
    for (unsigned i = 0; i < 2; i++) for (unsigned n = 0; n < 3; n++) {
        const char *const radios[] = {"bt", "wifi", "mcu"};
        snprintf(file, sizeof(file), "%s/soc2_2_ram_%s_%s_hdr.bin", directory, radios[n], i ? "1e_1" : "1d_1");
        CHECK(!unlink(file));
    }
    CHECK(!rmdir(directory));
}

static int catalog(void *context) { return *(int *)context; }
static pid_t start_service(int *peer, int result) {
    int pair[2], started[2];
    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair));
    CHECK(!pipe(started));
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        close(pair[1]); close(started[0]);
        struct sigaction sa = {.sa_handler = stop_control};
        sigemptyset(&sa.sa_mask); sigaction(SIGTERM, &sa, NULL); signal(SIGPIPE, SIG_IGN);
        int ready_fd = t70_wmt_ready_server();
        if (ready_fd < 0) _exit(8);
        if (write(started[1], "1", 1) != 1) _exit(9);
        close(started[1]);
        int rc = serve(pair[0], ready_fd, catalog, &result);
        close(ready_fd); close(pair[0]); _exit(rc ? 2 : 0);
    }
    close(pair[0]); close(started[1]); char ready;
    CHECK(read(started[0], &ready, 1) == 1); close(started[0]);
    *peer = pair[1]; return pid;
}
static void finish_service(pid_t pid, int peer, int result, int signal_stop) {
    if (signal_stop) CHECK(!kill(pid, SIGTERM));
    int status; CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == result);
    if (peer >= 0) close(peer);
}
static void drop_ready_request(void) {
    struct sockaddr_un address; socklen_t length;
    CHECK(!t70_wmt_ready_address(&address, &length));
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0); CHECK(fd >= 0);
    CHECK(!connect(fd, (struct sockaddr *)&address, length));
    struct linger reset = {.l_onoff = 1, .l_linger = 0};
    CHECK(!setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)));
    CHECK(send(fd, T70_WMT_READY_REQUEST, sizeof(T70_WMT_READY_REQUEST) - 1, MSG_NOSIGNAL) ==
          (ssize_t)(sizeof(T70_WMT_READY_REQUEST) - 1));
    close(fd);
}
static void oversized_ready_request(void) {
    struct sockaddr_un address; socklen_t length;
    CHECK(!t70_wmt_ready_address(&address, &length));
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0); CHECK(fd >= 0);
    CHECK(!connect(fd, (struct sockaddr *)&address, length));
    CHECK(send(fd, "PINGx", 5, MSG_NOSIGNAL) == 5);
    struct pollfd p = {fd, POLLIN, 0}; CHECK(poll(&p, 1, 500) == 1);
    char reply[16]; CHECK(recv(fd, reply, sizeof(reply), MSG_TRUNC) == 0);
    close(fd);
}
static void oversized_ready_reply_test(void) {
    struct sockaddr_un address; socklen_t length;
    CHECK(!t70_wmt_ready_address(&address, &length));
    int started[2]; CHECK(!pipe(started));
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        close(started[0]);
        int server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (server < 0 || bind(server, (struct sockaddr *)&address, length) || listen(server, 1)) _exit(2);
        if (write(started[1], "1", 1) != 1) _exit(3);
        close(started[1]);
        int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) _exit(4);
        char request[8]; ssize_t count = recv(client, request, sizeof(request), MSG_TRUNC);
        if (count != 4 || memcmp(request, "PING", 4)) _exit(5);
        if (send(client, "READYx", 6, MSG_NOSIGNAL) != 6) _exit(6);
        close(client); close(server); _exit(0);
    }
    close(started[1]); char ready;
    CHECK(read(started[0], &ready, 1) == 1); close(started[0]);
    CHECK(t70_wmt_ready() == -1);
    int status; CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void readiness_backlog_test(void) {
    int server = t70_wmt_ready_server(); CHECK(server >= 0);
    struct sockaddr_un address; socklen_t length;
    CHECK(!t70_wmt_ready_address(&address, &length));
    int clients[64], used = 0, full = 0;
    for (unsigned attempt = 0; attempt < 64 && !full; attempt++) {
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0); CHECK(fd >= 0);
        if (!connect(fd, (struct sockaddr *)&address, length)) clients[used++] = fd;
        else if (errno == EINPROGRESS) {
            struct pollfd p = {fd, POLLOUT, 0};
            if (poll(&p, 1, 20) > 0) {
                int error = 0; socklen_t size = sizeof(error);
                CHECK(!getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size));
                if (!error) clients[used++] = fd;
                else if (error == EAGAIN || error == ECONNREFUSED) { close(fd); full = 1; }
                else { errno = error; CHECK(0); }
            } else { close(fd); full = 1; }
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) { close(fd); full = 1; }
        else { perror("readiness backlog connect"); CHECK(0); }
    }
    CHECK(full); CHECK(used > 0);
    int result_pipe[2]; CHECK(!pipe(result_pipe));
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        close(result_pipe[0]); int result = t70_wmt_ready() ? 1 : 0;
        ssize_t written = write(result_pipe[1], &result, sizeof(result));
        _exit(written == (ssize_t)sizeof(result) ? 0 : 10);
    }
    close(result_pipe[1]);
    struct pollfd ready = {result_pipe[0], POLLIN, 0};
    int available = poll(&ready, 1, 1000);
    if (available <= 0) { kill(child, SIGKILL); waitpid(child, NULL, 0); }
    CHECK(available == 1);
    int result = 0; CHECK(read(result_pipe[0], &result, sizeof(result)) == sizeof(result));
    close(result_pipe[0]); int status; CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0); CHECK(result == 1);
    for (int i = 0; i < used; i++) close(clients[i]);
    close(server);
}
static void request(int fd, const void *command, size_t length, const char *expected) {
    CHECK(write(fd, command, length) == (ssize_t)length);
    struct pollfd p = {fd, POLLIN, 0}; CHECK(poll(&p, 1, 1500) == 1);
    char answer[128]; ssize_t n = recv(fd, answer, sizeof(answer), MSG_TRUNC);
    CHECK(n == (ssize_t)strlen(expected)); CHECK(!memcmp(answer, expected, (size_t)n));
}
static void command_priority_test(void) {
    int peer, clients[4];
    pid_t pid = start_service(&peer, 0);
    struct sockaddr_un address; socklen_t length;
    CHECK(!t70_wmt_ready_address(&address, &length));
    /* Queue slow diagnostics before letting the worker run. They must not
     * be serviced as one blocking batch ahead of the WMT command socket. */
    CHECK(!kill(pid, SIGSTOP));
    int stopped_status;
    CHECK(waitpid(pid, &stopped_status, WUNTRACED) == pid && WIFSTOPPED(stopped_status));
    for (unsigned i = 0; i < 4; i++) {
        clients[i] = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        CHECK(clients[i] >= 0 && !connect(clients[i], (struct sockaddr *)&address, length));
    }
    CHECK(!kill(pid, SIGCONT));
    usleep(45000);
    CHECK(write(peer, "srh_rom_patch", 13) == 13);
    struct pollfd available = {peer, POLLIN, 0};
    int ready = poll(&available, 1, 240);
    for (unsigned i = 0; i < 4; i++) close(clients[i]);
    char reply[8];
    int accepted = ready == 1 && recv(peer, reply, sizeof(reply), MSG_TRUNC) == 2 &&
                   !memcmp(reply, "ok", 2);
    finish_service(pid, peer, 0, 1);
    if (!accepted) fputs("WMT command delayed behind readiness clients\n", stderr);
    CHECK(accepted);
}
static void control_test(void) {
    CHECK(t70_wmt_ready() == -1);
    int peer; pid_t pid = start_service(&peer, 0);
    CHECK(!t70_wmt_ready());
    drop_ready_request();
    CHECK(!t70_wmt_ready());
    oversized_ready_request();
    CHECK(!t70_wmt_ready());
    request(peer, "srh_rom_patch", 13, "ok");
    for (int i = 0; i < 32; i++) request(peer, "srh_rom_patch", 13, "ok");
    request(peer, "srh_rom_patch 0", 15, "ok");
    request(peer, "unknown", 7, "cmd not found");
    request(peer, "srh_rom_patchx", 14, "cmd not found");
    request(peer, "srh_rom_patch\0extra", 19, "resp_-1");
    char oversized[255]; memset(oversized, 'x', sizeof(oversized));
    memcpy(oversized, "srh_rom_patch ", 14);
    request(peer, oversized, sizeof(oversized), "resp_-1");
    finish_service(pid, peer, 0, 1);
    CHECK(t70_wmt_ready() == -1);
    pid = start_service(&peer, -7);
    CHECK(!t70_wmt_ready());
    request(peer, "srh_rom_patch", 13, "resp_-7");
    finish_service(pid, peer, 0, 1);
    CHECK(t70_wmt_ready() == -1);
    pid = start_service(&peer, 0); close(peer);
    finish_service(pid, -1, 2, 0);
}
int main(int argc, char **argv) {
    alarm(30);
    CHECK(argc == 2);
    platform_test();
    catalog_test(argv[1]);
    int pair[2]; CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair));
    CHECK(!set_wifi_power(pair[0], 1)); char power; CHECK(read(pair[1], &power, 1) == 1); CHECK(power == '1');
    CHECK(!set_wifi_power(pair[0], 0)); CHECK(read(pair[1], &power, 1) == 1); CHECK(power == '0');
    close(pair[0]); close(pair[1]);
    readiness_backlog_test();
    oversized_ready_reply_test();
    control_test();
    command_priority_test();
    printf("WMT control: %u checks passed\n", checks);
    return 0;
}
