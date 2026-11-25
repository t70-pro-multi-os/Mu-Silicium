#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PATH_CAP 4096

static const char *const required_modules[] = {
    "ccci_md_all", "ccci_ccif", "ccci_dpmaif", "ccci_util_lib", "ccmni"
};

static int make_path(char *out, size_t cap, const char *root, const char *rel)
{
    int n;
    if (!root || !*root || strcmp(root, "/") == 0)
        n = snprintf(out, cap, "/%s", rel);
    else
        n = snprintf(out, cap, "%s/%s", root, rel);
    return n >= 0 && (size_t)n < cap ? 0 : -1;
}

static bool exists(const char *root, const char *rel)
{
    char path[PATH_CAP];
    struct stat st;
    return make_path(path, sizeof(path), root, rel) == 0 && stat(path, &st) == 0;
}

static bool read_complete_value(const char *root, const char *rel, char *out, size_t cap)
{
    char path[PATH_CAP];
    FILE *fp;
    size_t n;
    if (make_path(path, sizeof(path), root, rel) != 0)
        return false;
    fp = fopen(path, "r");
    if (!fp)
        return false;
    n = fread(out, 1, cap, fp);
    bool complete = !ferror(fp) && feof(fp);
    fclose(fp);
    if (!complete || !n || n >= cap || memchr(out, '\0', n))
        return false;
    // proc comm and these sysfs values end in one newline. Internal spaces
    // and newlines are part of the value, not a valid matching token prefix.
    if (out[n - 1] == '\n') --n;
    out[n] = '\0';
    return n != 0;
}

static bool is_digits(const char *s)
{
    if (!s || !*s)
        return false;
    for (; *s; ++s)
        if (*s < '0' || *s > '9')
            return false;
    return true;
}


/* Stock ccci_util_lib.ko creates kernel_kobj/ccci/boot; boot_status_show
 * calls boot_md_show from ccci_md_all.ko and wraps its md1:%d in one line.
 * boot_md_show only reads the current FSM state. Do not use the write side
 * of this attribute or the /dev monitor endpoint. Values are raw states. */
static bool read_modem_fsm(const char *root, int *out)
{
    static const char rel[] = "sys/kernel/ccci/boot";
    char path[PATH_CAP], value[64];
    struct stat node;
    if (make_path(path, sizeof(path), root, rel) ||
        lstat(path, &node) || !S_ISREG(node.st_mode) ||
        !read_complete_value(root, rel, value, sizeof(value)) ||
        strncmp(value, "md1:", 4) || !is_digits(value + 4))
        return false;
    errno = 0;
    long state = strtol(value + 4, NULL, 10);
    if (errno || state < 0 || state > INT_MAX)
        return false;
    *out = (int)state;
    return true;
}

static bool process_named(const char *root, const char *wanted)
{
    char proc_path[PATH_CAP];
    DIR *proc;
    struct dirent *entry;
    if (make_path(proc_path, sizeof(proc_path), root, "proc") != 0)
        return false;
    proc = opendir(proc_path);
    if (!proc)
        return false;
    while ((entry = readdir(proc)) != NULL) {
        char rel[PATH_CAP], comm[256];
        if (!is_digits(entry->d_name))
            continue;
        if (snprintf(rel, sizeof(rel), "proc/%s/comm", entry->d_name) >= (int)sizeof(rel))
            continue;
        if (read_complete_value(root, rel, comm, sizeof(comm)) && strcmp(comm, wanted) == 0) {
            closedir(proc);
            return true;
        }
    }
    closedir(proc);
    return false;
}

static bool has_prefix(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void inspect_netdevs(const char *root, unsigned *count, unsigned *up,
                            unsigned *down, unsigned *unknown)
{
    char path[PATH_CAP];
    DIR *dir;
    struct dirent *entry;
    *count = *up = *down = *unknown = 0;
    if (make_path(path, sizeof(path), root, "sys/class/net") != 0)
        return;
    dir = opendir(path);
    if (!dir)
        return;
    while ((entry = readdir(dir)) != NULL) {
        char rel[PATH_CAP], state[64], carrier[64];
        bool have_state, have_carrier;
        if (!has_prefix(entry->d_name, "ccmni"))
            continue;
        ++*count;
        if (snprintf(rel, sizeof(rel), "sys/class/net/%s/operstate", entry->d_name) >= (int)sizeof(rel)) {
            ++*unknown;
            continue;
        }
        have_state = read_complete_value(root, rel, state, sizeof(state));
        if (snprintf(rel, sizeof(rel), "sys/class/net/%s/carrier", entry->d_name) >= (int)sizeof(rel)) {
            ++*unknown;
            continue;
        }
        have_carrier = read_complete_value(root, rel, carrier, sizeof(carrier));
        if (have_state && have_carrier && strcmp(state, "up") == 0 && strcmp(carrier, "1") == 0)
            ++*up;
        else if ((have_state && (strcmp(state, "down") == 0 ||
                                 strcmp(state, "lowerlayerdown") == 0)) ||
                 (have_carrier && strcmp(carrier, "0") == 0))
            ++*down;
        else
            ++*unknown;
    }
    closedir(dir);
}

static int inspect(const char *root)
{
    size_t i;
    unsigned loaded = 0, net_count, net_up, net_down, net_unknown;
    bool endpoint, boot_helper, ril;
    int modem_state;

    for (i = 0; i < sizeof(required_modules) / sizeof(required_modules[0]); ++i) {
        char rel[PATH_CAP];
        if (snprintf(rel, sizeof(rel), "sys/module/%s", required_modules[i]) < (int)sizeof(rel) && exists(root, rel))
            ++loaded;
    }
    endpoint = exists(root, "dev/ccci_monitor");
    boot_helper = process_named(root, "ccci_mdinit");
    ril = process_named(root, "rild");
    inspect_netdevs(root, &net_count, &net_up, &net_down, &net_unknown);

    printf("T70 LTE inventory (read-only; no modem ioctl, power, SIM, or attach operation)\n");
    printf("CCCI core modules: %u/%zu present in sysfs\n", loaded,
           sizeof(required_modules) / sizeof(required_modules[0]));
    printf("CCCI monitor endpoint: %s (stat only; never opened)\n", endpoint ? "present" : "absent");
    printf("ccci_mdinit process: %s\n", boot_helper ? "present" : "absent");
    printf("rild process: %s\n", ril ? "present" : "absent");
    printf("CCMNI netdevs: %u registered; %u link-up, %u link-down, %u unknown\n",
           net_count, net_up, net_down, net_unknown);
    if (read_modem_fsm(root, &modem_state))
        printf("CCCI modem FSM state: %d (raw stock sysfs value)\n", modem_state);
    else
        printf("CCCI modem FSM state: UNKNOWN (no usable stock sysfs value; monitor not opened)\n");
    printf("LTE data session / SIM registration: NOT TESTED\n");
    printf("Result: inventory only; presence does not establish modem boot, network attach, or IP data\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *root = "/";
    if (argc == 3 && strcmp(argv[1], "--root") == 0) {
        root = argv[2];
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [--root DIR]\n", argv[0]);
        return 2;
    }
    if (root[0] != '/') {
        fprintf(stderr, "--root must be an absolute path\n");
        return 2;
    }
    return inspect(root);
}
