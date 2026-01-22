#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/user.h>


#define IOVEC_ENTRY(x) {x ? (char *)x : 0, x ? strlen(x) + 1 : 0}
#define IOVEC_SIZE(x) (sizeof(x) / sizeof(struct iovec))

#define PAYLOAD_NAME "backpork.elf"

typedef struct notify_request {
    char unused[45];
    char message[3075];
} notify_request_t;

typedef struct app_info {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c];
} app_info_t;

extern int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
extern int sceKernelGetAppInfo(pid_t pid, app_info_t *info);

static void notify(const char* fmt, ...) {
    notify_request_t req = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(req.message, sizeof(req.message) - 1, fmt, args);
    va_end(args);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

// from john-tornblom
static pid_t find_pid(const char *name) {
    int mib[4] = {1, 14, 8, 0};
    pid_t mypid = getpid();
    pid_t pid = -1;
    size_t buf_size;
    uint8_t *buf;

    if (sysctl(mib, 4, 0, &buf_size, 0, 0)) {
        return -1;
    }

    if (!(buf = malloc(buf_size))) {
        return -1;
    }

    if (sysctl(mib, 4, buf, &buf_size, 0, 0)) {
        free(buf);
        return -1;
    }

    for (uint8_t *ptr = buf; ptr < (buf + buf_size);) {
        int ki_structsize = *(int *)ptr;
        pid_t ki_pid = *(pid_t *)&ptr[72];
        char *ki_tdname = (char *)&ptr[447];

        ptr += ki_structsize;
        if (!strcmp(name, ki_tdname) && ki_pid != mypid) {
            pid = ki_pid;
        }
    }

    free(buf);

    return pid;
}

static int cleanup_directory(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        return -1;
    }

    int result = 0;
    struct dirent *entry;

    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[PATH_MAX];
        int path_len = snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        if (path_len < 0 || path_len >= sizeof(full_path)) {
            result = -1;
            break;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            result = -1;
            break;
        }

        if (S_ISDIR(st.st_mode)) {
            if (cleanup_directory(full_path) != 0) {
                result = -1;
                break;
            }
        }
        // Skip files (don't delete them)
    }

    closedir(d);

    if (result == 0) {
        result = rmdir(path);
    }

    return result;
}

static int mount2(const char *src, const char *dst, const char *type) {
    struct iovec iov[] = {
        IOVEC_ENTRY("fstype"),
        IOVEC_ENTRY(type),
        IOVEC_ENTRY("from"),
        IOVEC_ENTRY(src),
        IOVEC_ENTRY("fspath"),
        IOVEC_ENTRY(dst),
    };

    return nmount(iov, IOVEC_SIZE(iov), 0);
}


static char *mount_fakelibs(const char *sandbox_id, const char *cwd, pid_t pid, char *random_folder) {
    char fake_path[PATH_MAX + 1];
    snprintf(fake_path, sizeof(fake_path), "%s/fakelib", cwd);

    struct stat st;
    if (stat(fake_path, &st) != 0) {
        printf("[WARNING] stat on %s failed (errno: %d, %s)\n", fake_path, errno, strerror(errno));
        return NULL;
    }

    char *fake_mount_path = (char *)malloc(PATH_MAX + 1);
    if (!fake_mount_path) {
        return NULL;
    }

    snprintf(fake_mount_path, PATH_MAX + 1, "/mnt/sandbox/%s/%s/common/lib", sandbox_id, random_folder);

    int res = mount2(fake_path, fake_mount_path, "unionfs");
    if (res != 0) {
        printf("[WARNING] mount_unionfs failed: %d (errno: %d, %s)\n", res, errno, strerror(errno));
        unmount(fake_mount_path, MNT_FORCE);
        free(fake_mount_path);
        return NULL;
    }

    printf("[INFO] Mounted fakelibs from %s to %s\n", fake_path, fake_mount_path);
    return fake_mount_path;
}

static void wait_for_pid_exit(pid_t pid) {
    int kq = kqueue();
    if (kq == -1) {
        perror("kqueue");
        return;
    }

    struct kevent kev;
    EV_SET(&kev, pid, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_CLEAR, NOTE_EXIT, 0, NULL);

    int ret = kevent(kq, &kev, 1, NULL, 0, NULL);
    if (ret == -1) {
        printf("[WARNING] kevent registration failed for pid %d: %s\n", pid, strerror(errno));
        close(kq);
        sleep(3);
        return;
    }

    printf("[INFO] Waiting for pid %d to exit...\n", pid);

    while (1) {
        struct kevent event;
        int nev = kevent(kq, NULL, 0, &event, 1, NULL);

        if (nev < 0) {
            printf("[WARNING] kevent wait failed: %s\n", strerror(errno));
            close(kq);
            return;
        }

        if (nev > 0 && event.fflags & NOTE_EXIT) {
            printf("[INFO] Process %d exited\n", pid);
            break;
        }
    }

    close(kq);
}

static int find_highest_sandbox_number(const char* title_id) {
    char base_path[PATH_MAX];
    int highest = -1;

    for (int i = 0; i < 1000; i++) {
        snprintf(base_path, sizeof(base_path), "/mnt/sandbox/%s_%03d", title_id, i);

        struct stat st;
        if (stat(base_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            highest = i;
        } else {
            break;
        }
    }

    return highest;
}

static char* find_random_folder(const char* title_id, int sandbox_num) {
    char base_path[PATH_MAX];
    snprintf(base_path, sizeof(base_path), "/mnt/sandbox/%s_%03d", title_id, sandbox_num);

    DIR* dir = opendir(base_path);
    if (!dir) {
        printf("[WARNING] Failed to open directory: %s\n", base_path);
        return NULL;
    }

    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s/common/lib", base_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            closedir(dir);
            printf("[DEBUG] Found random folder: %s in sandbox %03d\n", entry->d_name, sandbox_num);
            return strdup(entry->d_name);
        }
    }

    closedir(dir);
    printf("[WARNING] No random folder found in %s\n", base_path);
    return NULL;
}

static void cleanup_game(pid_t pid, const char *sandbox_id, char *fake_mount_path) {
    // Wait for sandbox to be cleaned up by the system
    char sandbox_app0[PATH_MAX];
    snprintf(sandbox_app0, sizeof(sandbox_app0), "/mnt/sandbox/%s/app0", sandbox_id);
    printf("[INFO] Waiting for sandbox cleanup: %s\n", sandbox_app0);

    int wait_count = 0;
    struct stat sandbox_st;
    while (stat(sandbox_app0, &sandbox_st) == 0 && wait_count < 30) {
        sleep(1);
        wait_count++;
        if (wait_count % 5 == 0) {
            printf("[DEBUG] Still waiting for sandbox cleanup... (%d seconds)\n", wait_count);
        }
    }

    if (stat(sandbox_app0, &sandbox_st) == 0) {
        printf("[WARNING] Sandbox still exists after 30 seconds, proceeding anyway\n");
    } else {
        printf("[INFO] Sandbox cleaned up after %d seconds\n", wait_count);
    }

    // Unmount the fakelibs
    printf("[INFO] Unmounting %s\n", fake_mount_path);
    unmount(fake_mount_path, 0);

    // Remove the entire sandbox directory
    char sandbox_dir[PATH_MAX];
    snprintf(sandbox_dir, sizeof(sandbox_dir), "/mnt/sandbox/%s", sandbox_id);
    printf("[INFO] Removing directory %s\n", sandbox_dir);
    if (cleanup_directory(sandbox_dir) == 0) {
		notify("Directory removed successfully.");
        printf("[INFO] Directory removed successfully\n");
    } else {
        printf("[WARNING] Failed to remove directory: %s\n", strerror(errno));
    }

    free(fake_mount_path);
    printf("[INFO] Cleanup finished.\n");
}

static void patch_game(pid_t child_pid, const char *title_id) {
    // Find highest sandbox number
    int sandbox_num = find_highest_sandbox_number(title_id);
    if (sandbox_num == -1) {
        printf("[WARNING] No sandbox found for %s\n", title_id);
        return;
    }

    // Build sandbox_id
    char sandbox_id[14];
    snprintf(sandbox_id, sizeof(sandbox_id), "%s_%03d", title_id, sandbox_num);

    // Check if fakelib exists
    char fakelib_src_path[PATH_MAX];
    snprintf(fakelib_src_path, sizeof(fakelib_src_path), "/mnt/sandbox/%s/app0/fakelib", sandbox_id);
    struct stat st;
    if (stat(fakelib_src_path, &st) != 0) {
        return;
    }

    // Find random folder
    char* random_folder = find_random_folder(title_id, sandbox_num);
    if (!random_folder) {
        printf("[WARNING] Failed to find random folder for %s\n", title_id);
        return;
    }

	notify("Detected game %s. Patching...", title_id);
    printf("[INFO] Detected game %s (pid %d) in sandbox %s. Patching...\n", title_id, child_pid, sandbox_id);

    char src_path[PATH_MAX];
    snprintf(src_path, sizeof(src_path), "/mnt/sandbox/%s/app0", sandbox_id);

    char* fake_mount_path = mount_fakelibs(sandbox_id, src_path, child_pid, random_folder);

    if (fake_mount_path) {
		notify("Patch successful. Waiting for game to exit...");
        printf("[INFO] Patch successful. Waiting for game to exit...\n");
        wait_for_pid_exit(child_pid);
        cleanup_game(child_pid, sandbox_id, fake_mount_path);
    }

    free(random_folder);
}

int main() {
    syscall(SYS_thr_set_name, -1, PAYLOAD_NAME);

    int pid;
    while ((pid = find_pid(PAYLOAD_NAME)) > 0) {
        if (kill(pid, SIGKILL)) {
            return -1;
        }
        printf("[INFO] Killed old instance\n");
        sleep(1);
    }

    pid_t syscore_pid = find_pid("SceSysCore.elf");
    if (syscore_pid == -1) {
        printf("[WARNING] Failed to find SceSysCore.elf pid\n");
        return -1;
    }

    int kq = kqueue();
    if (kq == -1) {
        perror("kqueue");
        return -1;
    }

    struct kevent kev;
    EV_SET(&kev, syscore_pid, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_CLEAR,
           NOTE_FORK | NOTE_EXEC | NOTE_TRACK, 0, NULL);

    int ret = kevent(kq, &kev, 1, NULL, 0, NULL);
    if (ret == -1) {
        perror("kevent");
        close(kq);
        return -1;
    }

	notify("Welcome To BackPork 0.1 By BestPig 🐷");
    printf("[INFO] Monitoring SceSysCore.elf (pid %d) for game launches...\n", syscore_pid);

    pid_t child_pid = -1;

    while (1) {
        struct kevent event;
        int nev = kevent(kq, NULL, 0, &event, 1, NULL);

        if (nev < 0) {
            perror("kevent");
            continue;
        }

        if (nev == 0) continue;

        if (event.fflags & NOTE_CHILD) {
            child_pid = event.ident;
        }

        if (event.fflags & NOTE_EXEC && child_pid != -1 && event.ident == child_pid) {
            app_info_t appinfo = {0};
            if (sceKernelGetAppInfo(child_pid, &appinfo) != 0) {
                child_pid = -1;
                continue;
            }

            char title_id[10] = {0};
            memcpy(title_id, appinfo.title_id, 9);

            // Check if it's a PPSA/CUSA game
            if (strncmp(title_id, "PPSA", 4) != 0 && strncmp(title_id, "CUSA", 4) != 0) {
                child_pid = -1;
                continue;
            }

            patch_game(child_pid, title_id);
            child_pid = -1;
        }
    }

    close(kq);
    return 0;
}

