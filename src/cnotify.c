/*
 * cnotify.c - Recursive inotify file system watcher
 *
 * Copyright (c) 2026
 * Licensed under MIT License
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../include/cnotify.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

/* Compile-time configuration */
#define INOTIFY_EVENT_SIZE  (sizeof(struct inotify_event))
#define INOTIFY_BUF_LEN     (64 * (INOTIFY_EVENT_SIZE + NAME_MAX + 1))
#define WATCH_HASH_SIZE     1024
#define DEFAULT_DEBOUNCE_MS 100

/* Watch flags we care about */
#define WATCH_MASK                                                                                                 \
    (IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB | \
     IN_CLOSE_WRITE | IN_ONLYDIR | IN_EXCL_UNLINK)

/**
 * Hash table node for watch descriptor -> path mapping
 */
typedef struct watch_node {
    int wd;                  /* inotify watch descriptor */
    char* path;              /* allocated path string */
    struct watch_node* next; /* hash collision chain */
} watch_node_t;

/**
 * Main context structure
 */
struct cnotify {
    int fd;                               /* inotify file descriptor */
    watch_node_t* table[WATCH_HASH_SIZE]; /* hash table for wd->path */
    unsigned int debounce_ms;             /* debounce interval */
    char** exclude_dirs;                  /* borrowed pointer to exclusion list */
    char* event_buf;                      /* reusable event buffer */
    size_t event_buf_size;                /* buffer size */
};

/* ========================================================================
 * Hash table operations
 * ======================================================================== */

static inline unsigned int hash_wd(int wd) {
    /* Simple modulo hash - wd values are already fairly random */
    return ((unsigned int)wd) % WATCH_HASH_SIZE;
}

static watch_node_t* watch_node_create(int wd, const char* path) {
    watch_node_t* node;

    node = malloc(sizeof(*node));
    if (!node) return NULL;

    node->path = strdup(path);
    if (!node->path) {
        free(node);
        return NULL;
    }

    node->wd = wd;
    node->next = NULL;
    return node;
}

static void watch_node_destroy(watch_node_t* node) {
    if (node) {
        free(node->path);
        free(node);
    }
}

static int hash_insert(cnotify_t* cn, int wd, const char* path) {
    unsigned int idx;
    watch_node_t* node;

    node = watch_node_create(wd, path);
    if (!node) return -1;

    idx = hash_wd(wd);
    node->next = cn->table[idx];
    cn->table[idx] = node;

    return 0;
}

static const char* hash_lookup(cnotify_t* cn, int wd) {
    unsigned int idx = hash_wd(wd);
    watch_node_t* node;

    for (node = cn->table[idx]; node; node = node->next) {
        if (node->wd == wd) return node->path;
    }

    return NULL;
}

static int hash_remove(cnotify_t* cn, int wd) {
    unsigned int idx = hash_wd(wd);
    watch_node_t *node, *prev = NULL;

    for (node = cn->table[idx]; node; prev = node, node = node->next) {
        if (node->wd == wd) {
            if (prev)
                prev->next = node->next;
            else
                cn->table[idx] = node->next;

            watch_node_destroy(node);
            return 0;
        }
    }

    return -1; /* not found */
}

static void hash_clear(cnotify_t* cn) {
    unsigned int i;
    watch_node_t *node, *next;

    for (i = 0; i < WATCH_HASH_SIZE; i++) {
        for (node = cn->table[i]; node; node = next) {
            next = node->next;
            watch_node_destroy(node);
        }
        cn->table[i] = NULL;
    }
}

/* ========================================================================
 * Path operations
 * ======================================================================== */

/**
 * Build full path safely with overflow checking
 * Returns allocated string or NULL on error
 */
static char* path_join(const char* dir, const char* name) {
    size_t dir_len, name_len, total_len;
    char* path;
    int needs_slash;

    dir_len = strlen(dir);
    name_len = strlen(name);

    /* Check for overflow before allocation */
    if (dir_len > PATH_MAX - name_len - 2) return NULL;

    needs_slash = (dir_len > 0 && dir[dir_len - 1] != '/');
    total_len = dir_len + (unsigned)needs_slash + name_len + 1;

    path = malloc(total_len);
    if (!path) return NULL;

    memcpy(path, dir, dir_len);
    if (needs_slash) path[dir_len] = '/';
    memcpy(path + dir_len + needs_slash, name, name_len + 1);

    return path;
}

/**
 * Check if directory name should be excluded
 */
static int is_excluded(cnotify_t* cn, const char* name) {
    char** exclude;

    if (!cn->exclude_dirs) return 0;

    for (exclude = cn->exclude_dirs; *exclude; exclude++) {
        if (strcmp(name, *exclude) == 0) return 1;
    }

    return 0;
}

/* ========================================================================
 * Watch management
 * ======================================================================== */

/**
 * Add inotify watch for a single directory
 * Returns watch descriptor on success, -1 on error
 */
static int add_watch_single(cnotify_t* cn, const char* path) {
    int wd;

    wd = inotify_add_watch(cn->fd, path, WATCH_MASK);
    if (wd < 0) return -1;

    if (hash_insert(cn, wd, path) < 0) {
        inotify_rm_watch(cn->fd, wd);
        errno = ENOMEM;
        return -1;
    }

    return wd;
}

/**
 * Recursively add watches to directory tree
 * Returns 0 on success (even with some failures), -1 on catastrophic error
 */
static int add_watch_recursive(cnotify_t* cn, const char* path) {
    DIR* dir;
    struct dirent* entry;
    char* child_path;
    int saved_errno;
    int ret = 0;

    /* Add watch for this directory first */
    if (add_watch_single(cn, path) < 0) {
        /*
         * Permission denied is common and not fatal.
         * ENOSPC means we've hit the watch limit - that IS fatal.
         */
        if (errno == ENOSPC) return -1;
        /* Otherwise warn but continue */
        return 0;
    }

    /* Now recurse into subdirectories */
    dir = opendir(path);
    if (!dir) {
        /* If we can't open it, we can't recurse, but the watch is still added */
        return 0;
    }

    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;

        /* Only recurse into directories */
        if (entry->d_type != DT_DIR) continue;

        /* Check exclusion list */
        if (is_excluded(cn, entry->d_name)) continue;

        /* Build full path */
        child_path = path_join(path, entry->d_name);
        if (!child_path) {
            saved_errno = errno;
            closedir(dir);
            errno = saved_errno;
            return -1;
        }

        /* Recurse */
        if (add_watch_recursive(cn, child_path) < 0) {
            saved_errno = errno;
            free(child_path);
            closedir(dir);
            errno = saved_errno;
            return -1;
        }

        free(child_path);
        errno = 0;
    }

    /* Check if readdir failed */
    saved_errno = errno;
    closedir(dir);

    if (saved_errno != 0) {
        errno = saved_errno;
        return -1;
    }

    return ret;
}

/* ========================================================================
 * Event processing
 * ======================================================================== */

static cnotify_event_type_t mask_to_event_type(uint32_t mask) {
    if (mask & IN_MODIFY) return CNOTIFY_EVENT_MODIFY;
    if (mask & IN_CREATE) return CNOTIFY_EVENT_CREATE;
    if (mask & IN_DELETE) return CNOTIFY_EVENT_DELETE;
    if (mask & (IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF)) return CNOTIFY_EVENT_MOVE;
    if (mask & IN_ATTRIB) return CNOTIFY_EVENT_ATTRIB;
    if (mask & IN_CLOSE_WRITE) return CNOTIFY_EVENT_CLOSE_WRITE;

    return CNOTIFY_EVENT_MODIFY; /* fallback */
}

/**
 * Process a single inotify event and invoke callback
 */
static int handle_event(cnotify_t* cn, struct inotify_event* ie, cnotify_callback_t callback, void* userdata) {
    const char* dir_path;
    cnotify_event_t event;
    char* new_path;

    /* Ignore events without a name */
    if (ie->len == 0) return 0;

    /* Check if the file/directory name is excluded */
    if (is_excluded(cn, ie->name)) return 0;

    dir_path = hash_lookup(cn, ie->wd);
    if (!dir_path) {
        /*
         * This can happen if we removed the watch but there were
         * queued events. Not an error.
         */
        return 0;
    }

    /*
     * Handle directory creation - add watch recursively
     * Do this BEFORE invoking the callback so the watch is active
     */
    if ((ie->mask & IN_CREATE) && (ie->mask & IN_ISDIR)) {
        if (!is_excluded(cn, ie->name)) {
            new_path = path_join(dir_path, ie->name);
            if (new_path) {
                add_watch_recursive(cn, new_path);
                free(new_path);
            }
        }
    }

    /*
     * Handle watch removal events
     * IN_IGNORED means the watch was removed (either by us or by kernel)
     * IN_DELETE_SELF means the watched directory was deleted
     * IN_MOVE_SELF means the watched directory was moved
     */
    if (ie->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
        hash_remove(cn, ie->wd);
    }

    /* Fill in event structure */
    memset(&event, 0, sizeof(event));
    event.type = mask_to_event_type(ie->mask);
    event.path = dir_path;
    event.name = ie->name;
    event.cookie = ie->cookie;
    event.is_dir = !!(ie->mask & IN_ISDIR);

    /* Invoke callback */
    return callback(&event, userdata);
}

/**
 * Read and process events from inotify fd
 * Returns number of events processed, or -1 on error
 */
static int process_events_once(cnotify_t* cn, cnotify_callback_t callback, void* userdata, int* stop_flag) {
    struct inotify_event* ie;
    ssize_t len;
    char* ptr;
    int processed = 0;
    int ret;

    len = read(cn->fd, cn->event_buf, cn->event_buf_size);
    if (len < 0) {
        if (errno == EINTR || errno == EAGAIN) return 0;
        return -1;
    }

    if (len == 0) {
        /* This shouldn't happen with inotify, but handle it anyway */
        errno = EINVAL;
        return -1;
    }

    /* Process all events in buffer */
    for (ptr = cn->event_buf; ptr < cn->event_buf + len;) {
        ie = (struct inotify_event*)ptr;

        ret = handle_event(cn, ie, callback, userdata);
        if (ret != 0) {
            *stop_flag = 1;
            break;
        }

        processed++;
        ptr += INOTIFY_EVENT_SIZE + ie->len;
    }

    return processed;
}

static int process_events_drain(cnotify_t* cn, cnotify_callback_t callback, void* userdata, int* stop_flag) {
    int flags;
    int total_processed = 0;

    flags = fcntl(cn->fd, F_GETFL, 0);
    if (flags < 0) return -1;

    if (fcntl(cn->fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

    while (!*stop_flag) {
        int ret = process_events_once(cn, callback, userdata, stop_flag);
        if (ret < 0) {
            int saved_errno = errno;
            (void)fcntl(cn->fd, F_SETFL, flags);
            errno = saved_errno;
            return -1;
        }

        if (ret == 0) break;
        total_processed += ret;
    }

    if (fcntl(cn->fd, F_SETFL, flags) < 0) return -1;
    return total_processed;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

cnotify_t* cnotify_init(void) {
    cnotify_t* cn;
    int fd;

    fd = inotify_init1(IN_CLOEXEC);
    if (fd < 0) return NULL;

    cn = calloc(1, sizeof(*cn));
    if (!cn) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }

    cn->event_buf = malloc(INOTIFY_BUF_LEN);
    if (!cn->event_buf) {
        close(fd);
        free(cn);
        errno = ENOMEM;
        return NULL;
    }

    cn->fd = fd;
    cn->event_buf_size = INOTIFY_BUF_LEN;
    cn->debounce_ms = DEFAULT_DEBOUNCE_MS;

    return cn;
}

void cnotify_destroy(cnotify_t* cn) {
    if (!cn) return;

    if (cn->fd >= 0) close(cn->fd);

    hash_clear(cn);
    free(cn->event_buf);
    free(cn);
}

int cnotify_add_watch(cnotify_t* cn, const char* path, const char* const* exclude_dirs) {
    struct stat st;

    if (!cn || !path) {
        errno = EINVAL;
        return -1;
    }

    /* Verify path exists and is a directory */
    if (stat(path, &st) < 0) return -1;

    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    /* Store exclusion list (we don't own it, caller manages lifetime) */
    cn->exclude_dirs = (char**)exclude_dirs;

    return add_watch_recursive(cn, path);
}

int cnotify_remove_watch(cnotify_t* cn, const char* path) {
    unsigned int i;
    watch_node_t *node, *prev;
    int removed = 0;

    if (!cn || !path) {
        errno = EINVAL;
        return -1;
    }

    /* Linear search through hash table for matching path */
    for (i = 0; i < WATCH_HASH_SIZE; i++) {
        prev = NULL;
        for (node = cn->table[i]; node;) {
            if (strcmp(node->path, path) == 0) {
                /* Found it - remove the inotify watch */
                inotify_rm_watch(cn->fd, node->wd);

                /* Remove from hash table */
                if (prev)
                    prev->next = node->next;
                else
                    cn->table[i] = node->next;

                watch_node_t* to_free = node;
                node = node->next;
                watch_node_destroy(to_free);
                removed = 1;
                /* Don't break - there might be duplicates */
            } else {
                prev = node;
                node = node->next;
            }
        }
    }

    return removed ? 0 : -1;
}

void cnotify_set_debounce(cnotify_t* cn, unsigned int milliseconds) {
    if (cn) cn->debounce_ms = milliseconds;
}

int cnotify_get_fd(cnotify_t* cn) { return cn ? cn->fd : -1; }

int cnotify_process_events(cnotify_t* cn, cnotify_callback_t callback, void* userdata) {
    int stop = 0;

    if (!cn || !callback) {
        errno = EINVAL;
        return -1;
    }

    return process_events_drain(cn, callback, userdata, &stop);
}

int cnotify_start_loop(cnotify_t* cn, cnotify_callback_t callback, void* userdata) {
    fd_set fds;
    int ret, stop = 0;
    struct timeval debounce_tv;

    if (!cn || !callback) {
        errno = EINVAL;
        return -1;
    }

    while (!stop) {
        /* Wait for events */
        FD_ZERO(&fds);
        FD_SET(cn->fd, &fds);

        ret = select(cn->fd + 1, &fds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        /*
         * Debounce: sleep to let bursty events accumulate in the kernel
         * buffer, then drain them all at once firing the callback once.
         * We use select(0,...) as a portable sleep (no fd monitoring).
         */
        if (cn->debounce_ms > 0) {
            debounce_tv.tv_sec = cn->debounce_ms / 1000;
            debounce_tv.tv_usec = (cn->debounce_ms % 1000) * 1000;
            select(0, NULL, NULL, NULL, &debounce_tv);
        }

        /* Process all currently queued events as one cycle. */
        ret = process_events_drain(cn, callback, userdata, &stop);
        if (ret < 0 && errno != EINTR) return -1;
    }

    return 0;
}

const char* cnotify_event_type_name(cnotify_event_type_t type) {
    switch (type) {
        case CNOTIFY_EVENT_MODIFY:
            return "MODIFY";
        case CNOTIFY_EVENT_CREATE:
            return "CREATE";
        case CNOTIFY_EVENT_DELETE:
            return "DELETE";
        case CNOTIFY_EVENT_MOVE:
            return "MOVE";
        case CNOTIFY_EVENT_ATTRIB:
            return "ATTRIB";
        case CNOTIFY_EVENT_CLOSE_WRITE:
            return "CLOSE_WRITE";
        default:
            return "UNKNOWN";
    }
}
