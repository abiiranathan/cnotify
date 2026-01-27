#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../include/cnotify.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define EVENT_SIZE (sizeof(struct inotify_event))
#define BUF_LEN    (1024 * (EVENT_SIZE + 16))
#define HASH_SIZE  1024

typedef struct WatchNode {
    int wd;
    char* path;
    struct WatchNode* next;
} WatchNode;

struct CNotify {
    int fd;
    WatchNode* table[HASH_SIZE];
    int debounce_ms;
    char** exclude_dirs;
};

static unsigned int hash_wd(int wd) { return (unsigned)wd % HASH_SIZE; }

static int add_watch_map(CNotify* cn, int wd, const char* path) {
    unsigned int idx = hash_wd(wd);
    WatchNode* node = malloc(sizeof(WatchNode));
    if (!node) return -1;

    node->path = strdup(path);
    if (!node->path) {
        free(node);
        return -1;
    }

    node->wd = wd;
    node->next = cn->table[idx];
    cn->table[idx] = node;
    return 0;
}

static char* get_watch_path(CNotify* cn, int wd) {
    unsigned int idx = hash_wd(wd);
    WatchNode* curr = cn->table[idx];
    while (curr) {
        if (curr->wd == wd) return curr->path;
        curr = curr->next;
    }
    return NULL;
}

static void remove_watch_map(CNotify* cn, int wd) {
    unsigned int idx = hash_wd(wd);
    WatchNode* curr = cn->table[idx];
    WatchNode* prev = NULL;
    while (curr) {
        if (curr->wd == wd) {
            if (prev)
                prev->next = curr->next;
            else
                cn->table[idx] = curr->next;
            free(curr->path);
            free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

CNotify* cnotify_init(void) {
    CNotify* cn = malloc(sizeof(CNotify));
    if (!cn) return NULL;
    memset(cn, 0, sizeof(CNotify));

    cn->fd = inotify_init();
    if (cn->fd < 0) {
        free(cn);
        return NULL;
    }
    cn->debounce_ms = 100;  // default
    return cn;
}

void cnotify_free(CNotify* cn) {
    if (!cn) return;
    close(cn->fd);
    for (int i = 0; i < HASH_SIZE; i++) {
        WatchNode* curr = cn->table[i];
        while (curr) {
            WatchNode* next = curr->next;
            free(curr->path);
            free(curr);
            curr = next;
        }
    }
    free(cn);
}

void cnotify_set_debounce(CNotify* cn, int ms) { cn->debounce_ms = ms; }

static int is_excluded(CNotify* cn, const char* name) {
    if (!cn->exclude_dirs) return 0;
    for (int i = 0; cn->exclude_dirs[i]; i++) {
        if (strcmp(name, cn->exclude_dirs[i]) == 0) return 1;
    }
    return 0;
}

// Recursive function to add watch
static int add_watch_recursive(CNotify* cn, const char* path) {
    // Add watch for current dir
    int wd = inotify_add_watch(
        cn->fd, path, IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF);

    if (wd < 0) {
        // Warn but continue if it's just one permission denied or similar
        // fprintf(stderr, "Warning: Failed to watch %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (add_watch_map(cn, wd, path) < 0) {
        fprintf(stderr, "Error: Failed to allocate memory for watch node: %s\n", path);
        // Clean up the inotify watch since we can't track it
        inotify_rm_watch(cn->fd, wd);
        return -1;
    }

    DIR* dir = opendir(path);
    if (!dir) return 0;  // Not a directory or can't open, strictly speaking not a fatal error for the watcher root

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            if (is_excluded(cn, entry->d_name)) continue;

            char fullpath[PATH_MAX];
            int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
            if (n < 0 || n >= (int)sizeof(fullpath)) {
                fprintf(stderr, "Warning: Path too long, skipping: %s/%s\n", path, entry->d_name);
                continue;
            }

            add_watch_recursive(cn, fullpath);
        }
    }
    closedir(dir);
    return 0;
}

int cnotify_add_watch(CNotify* cn, const char* path, const char** exclude_dirs) {
    cn->exclude_dirs = (char**)exclude_dirs;  // Cast away const, but we treat it as const
    return add_watch_recursive(cn, path);
}

void cnotify_start_loop(CNotify* cn, CNotifyCallback cb, void* user_data) {
    char buf[BUF_LEN];
    fd_set fds;

    while (1) {
        FD_ZERO(&fds);
        FD_SET(cn->fd, &fds);

        // Wait indefinitely for first event
        int ret = select(cn->fd + 1, &fds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        // Read events
        int len = read(cn->fd, buf, BUF_LEN);
        if (len < 0) {
            perror("read");
            break;
        }

        // Debounce handling:
        // We got some events. Let's wait 'debounce_ms' to see if more come in,
        // and drain them.

        // Sleep for debounce time
        usleep((unsigned int)cn->debounce_ms * 1000);

        // Drain any pending data
        struct timeval tv = {0, 0};
        fd_set drain_fds;
        FD_ZERO(&drain_fds);
        FD_SET(cn->fd, &drain_fds);
        int drain_ret = select(cn->fd + 1, &drain_fds, NULL, NULL, &tv);
        if (drain_ret > 0) {
            // Read and discard to clear buffer
            char tmp[BUF_LEN];
            while (read(cn->fd, tmp, BUF_LEN) > 0) {
                // Keep reading until done or block (but read is blocking unless O_NONBLOCK)
                // Since select said ready, at least one read is safe.
                // We do one drain pass.
                // NOTE: Strictly this might block if we read exactly all data and call read again.
                // Ideally set non-blocking or use FIONREAD.
                // For safety in this simple loop, we'll assume the kernel buffer is drained enough
                // or just accept we processed the first batch.
                // To be safer let's just break after one read to clear what select saw.
                break;
            }
        }

        int i = 0;
        int callback_result = 0;
        int triggered = 0;  // Trigger once per batch?

        while (i < len) {
            struct inotify_event* event = (struct inotify_event*)&buf[i];

            if (event->len) {
                char* dir_path = get_watch_path(cn, event->wd);
                if (dir_path) {
                    // Handle new directories dynamically
                    if ((event->mask & IN_CREATE) && (event->mask & IN_ISDIR)) {
                        char new_path[PATH_MAX];
                        int n = snprintf(new_path, sizeof(new_path), "%s/%s", dir_path, event->name);
                        if (n >= 0 && n < (int)sizeof(new_path)) {
                            // Check exclusions
                            if (!is_excluded(cn, event->name)) {
                                add_watch_recursive(cn, new_path);
                            }
                        }
                    }

                    // Handle directory removal
                    if ((event->mask & IN_DELETE_SELF) || (event->mask & IN_MOVE_SELF)) {
                        remove_watch_map(cn, event->wd);
                    }

                    // Construct CNotifyEvent
                    if (!triggered) {
                        CNotifyEvent cne;
                        cne.path = dir_path;
                        cne.filename = event->name;

                        if (event->mask & IN_CREATE)
                            cne.type = CNOTIFY_EVENT_CREATE;
                        else if (event->mask & IN_DELETE)
                            cne.type = CNOTIFY_EVENT_DELETE;
                        else if (event->mask & IN_MODIFY)
                            cne.type = CNOTIFY_EVENT_MODIFY;
                        else if (event->mask & IN_MOVED_FROM || event->mask & IN_MOVED_TO)
                            cne.type = CNOTIFY_EVENT_MOVE;
                        else
                            cne.type = CNOTIFY_EVENT_UNKNOWN;

                        // Only fire callback once per batch for the "hot reload" use case
                        callback_result = cb(&cne, user_data);
                        triggered = 1;
                    }
                }
            }
            i += EVENT_SIZE + event->len;
        }

        if (callback_result != 0) break;
    }
}
