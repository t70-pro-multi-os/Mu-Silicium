// SPDX-License-Identifier: BSD-2-Clause-Patent
// Probe one stock T70 Pro sensor slot before any other camera client starts.
#define _GNU_SOURCE
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static const char camera_path[] = "/dev/kd_camera_hw";
static const char lock_path[] = "/run/teclast-camera-probe.lock";

struct camera_selection {
    uint32_t catalog_id;
    char driver[32];
};

/*
 * The stock imgsensor_isp6s ARM64 FEATURECONTROL ABI. SET_DRIVER briefly
 * powers the selected slot and tests sensor IDs through the stock driver.
 * It returns the selected catalog record, or copies the input back unchanged
 * when no driver recognizes a sensor.
 */
struct camera_feature {
    uint32_t index;
    uint32_t feature;
    struct camera_selection *selection;
    uint32_t *length;
};

enum { CameraSetDriver = 0x0c23 };
#define CAMERA_FEATURE_CONTROL _IOWR('i', 15, struct camera_feature)
_Static_assert(sizeof(struct camera_selection) == 36, "stock selection response");
_Static_assert(sizeof(struct camera_feature) == 24 &&
               offsetof(struct camera_feature, selection) == 8 &&
               offsetof(struct camera_feature, length) == 16, "stock ARM64 ioctl layout");
_Static_assert(CAMERA_FEATURE_CONTROL == 0xc018690fUL, "stock ARM64 ioctl number");

static int select_sensor(uint32_t index, struct camera_selection *selection)
{
    uint32_t length = sizeof(*selection);
    struct camera_feature feature = {index, CameraSetDriver, selection, &length};
    int fd = open(camera_path, O_RDWR | O_CLOEXEC | O_NOCTTY | O_NOFOLLOW);
    if (fd < 0) {
        perror(camera_path);
        return 1;
    }

    int status = 1;
    struct stat state;
    if (fstat(fd, &state) < 0) {
        perror("Camera device type");
        goto done;
    }
    if (!S_ISCHR(state.st_mode)) {
        fputs("Camera node is not a character device\n", stderr);
        goto done;
    }
    // A failed call may already have powered the sensor. Do not retry it.
    int result = ioctl(fd, CAMERA_FEATURE_CONTROL, &feature);
    if (result < 0) {
        perror("Camera sensor selection");
    } else if (result != 0 || length != sizeof(*selection)) {
        fputs("Unexpected camera selection response\n", stderr);
    } else {
        status = 0;
    }

done:
    if (close(fd) < 0) {
        perror("Close camera device");
        return 1;
    }
    return status;
}

static int probe_locked(uint32_t index, struct camera_selection *selection)
{
    // The stock driver counts opens and releases shared resources at last
    // close. Serialize this tool before open and through the device close.
    int lock = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (lock < 0) {
        perror(lock_path);
        return 1;
    }
    struct stat state;
    int status = 1;
    if (fstat(lock, &state) < 0) {
        perror("Inspect camera probe lock");
    } else if (!S_ISREG(state.st_mode) || (state.st_mode & 07777) != 0600 ||
               state.st_nlink != 1 || state.st_uid != geteuid()) {
        fputs("Invalid camera probe lock file\n", stderr);
    } else if (flock(lock, LOCK_EX | LOCK_NB) < 0) {
        perror("Camera probe lock");
    } else {
        status = select_sensor(index, selection);
    }
    if (close(lock) < 0) {
        perror("Close camera probe lock");
        return 1;
    }
    return status;
}

static int valid_driver_name(const char name[32])
{
    const char *end = memchr(name, '\0', 32);
    if (end == NULL || end == name)
        return 0;
    for (const char *p = name; p != end; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'))
            return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "probe") ||
        argv[2][0] < '0' || argv[2][0] > '2' || argv[2][1] != '\0') {
        fputs("Usage: teclast-camera probe SLOT (0, 1 or 2)\n", stderr);
        return 64;
    }

    uint32_t index = (uint32_t)(argv[2][0] - '0');
    struct camera_selection selection = {0};
    if (probe_locked(index, &selection) != 0)
        return 1;
    // An unmatched request returns the original, entirely zeroed record.
    static const char empty_driver[sizeof(selection.driver)] = {0};
    if (selection.catalog_id == 0 &&
        memcmp(selection.driver, empty_driver, sizeof(empty_driver)) == 0) {
        fprintf(stderr, "No stock driver matched a camera sensor in slot %" PRIu32 "\n", index);
        return 2;
    }
    if (selection.catalog_id == 0 || selection.catalog_id == UINT32_MAX ||
        !valid_driver_name(selection.driver)) {
        fputs("Invalid stock camera driver record\n", stderr);
        return 1;
    }
    if (printf("index=%" PRIu32 " catalog_id=0x%08" PRIx32 " driver=%s\n",
               index, selection.catalog_id, selection.driver) < 0 ||
        fflush(stdout) == EOF)
        return 1;
    return 0;
}
