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
#include <poll.h>     /* for poll, struct pollfd */
#include <stdalign.h> /* for alignof */
#include <stdbool.h>  /* for bool */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

/* ========================================================================
 * Compile-time configuration
 * ======================================================================== */

#define INOTIFY_EVENT_SIZE  (sizeof(struct inotify_event))
#define INOTIFY_BUF_LEN     (64 * (INOTIFY_EVENT_SIZE + NAME_MAX + 1))
#define WATCH_HASH_SIZE     1024 /* must be a power of two */
#define WATCH_HASH_BITS     10   /* log2(WATCH_HASH_SIZE) -- keep in sync */
#define DEFAULT_DEBOUNCE_MS 100

_Static_assert(WATCH_HASH_SIZE == (1u << WATCH_HASH_BITS), "WATCH_HASH_SIZE and WATCH_HASH_BITS have drifted apart");

/*
 * Watch flags:
 *   IN_ONLYDIR    - fail if path is not a directory (race-free check)
 *   IN_EXCL_UNLINK - suppress events for files unlinked from the directory
 *                    (e.g. temporary files already deleted by the time we
 *                    read the event), reducing noise significantly.
 */
#define WATCH_MASK                                                                                                 \
    (IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB | \
     IN_CLOSE_WRITE | IN_ONLYDIR | IN_EXCL_UNLINK)

/* Poll slot indices into the pollfd array used by cnotify_start_loop(). */
#define POLLFD_INOTIFY  0 /* inotify fd: real filesystem events              */
#define POLLFD_STOPPIPE 1 /* read end of the self-pipe: async stop request  */
#define POLLFD_COUNT    2

/* ========================================================================
 * Internal types
 * ======================================================================== */

/**
 * Hash table node for watch descriptor -> path mapping.
 *
 * The path is stored inline via a flexible array member so that creating a
 * node costs a single allocation instead of a malloc() for the node plus a
 * separate strdup() for the path.
 */
typedef struct watch_node {
    int wd;                       /* inotify watch descriptor */
    struct watch_node* next;      /* wd-hash collision chain */
    struct watch_node* path_next; /* path-hash collision chain (secondary index) */
    char path[];                  /* flexible array: path stored inline */
} watch_node_t;

/**
 * Per-file record stored in the file-metadata cache.
 *
 * Metadata (size, mtime, inode) is checked first on every event.  The
 * content hash is only recomputed when the metadata indicates a likely
 * change, making the common no-change path O(1) instead of O(file_size).
 */
typedef struct file_record {
    unsigned long content_hash; /* djb2 hash of file bytes */
    off_t size;                 /* st_size at last check */
    time_t mtime;               /* st_mtime at last check */
    ino_t ino;                  /* st_ino — detects replace-by-rename */
    struct file_record* next;   /* hash collision chain */
    char path[];                /* flexible array: path stored inline */
} file_record_t;

#define FILE_HASH_SIZE 4096 /* larger than watch table; one slot per file */

/*
 * Secondary index: path -> watch_node, mirroring WATCH_HASH_SIZE/BITS.
 * Kept the same size as the wd-keyed table since the two always hold the
 * same set of entries, just addressed by a different key.
 */
#define WATCH_PATH_HASH_SIZE WATCH_HASH_SIZE
#define WATCH_PATH_HASH_BITS WATCH_HASH_BITS

/**
 * Single entry in the excluded-basename set.
 *
 * Populated once (from the caller's NULL-terminated exclude_dirs array) at
 * cnotify_add_watch() time into a hash set, so a per-entry exclusion check
 * during tree walks and event handling is an O(1) average-case hash lookup
 * instead of an O(k) linear scan of the original array.
 */
typedef struct exclude_node {
    const char* name;          /* borrowed pointer into caller's exclude_dirs strings */
    struct exclude_node* next; /* hash collision chain */
} exclude_node_t;

#define EXCLUDE_HASH_SIZE 64 /* small: exclude lists are typically short */

/**
 * Main watcher context.
 */
struct cnotify {
    int fd;                                         /* inotify file descriptor */
    int stop_pipe[2];                               /* [0]=read, [1]=write; self-pipe for async stop */
    watch_node_t* table[WATCH_HASH_SIZE];           /* wd -> watch_node hash table */
    watch_node_t* path_table[WATCH_PATH_HASH_SIZE]; /* path -> watch_node hash table (secondary index) */
    file_record_t* fcache[FILE_HASH_SIZE];          /* path -> file_record cache */
    exclude_node_t* exclude_set[EXCLUDE_HASH_SIZE]; /* excluded basenames, built from exclude_dirs */
    unsigned int debounce_ms;                       /* debounce interval in ms */
    char** exclude_dirs;                            /* borrowed; caller owns lifetime; source for exclude_set */
    char* event_buf;                                /* aligned, reusable read buffer */
    size_t event_buf_size;                          /* size of event_buf in bytes */
};

/* ========================================================================
 * Watch-descriptor hash table
 *
 * inotify allocates watch descriptors sequentially from 1.  A plain modulo
 * hash clusters all entries in the low buckets.  The Knuth multiplicative
 * hash spreads sequential integers uniformly across all buckets.
 * ======================================================================== */

static inline unsigned int hash_wd(int wd) {
    /*
     * Fibonacci / Knuth multiplicative hash.
     * The magic constant is 2^32 / phi, chosen so that sequential integers
     * map to well-separated bucket indices.
     * Right-shift by (32 - WATCH_HASH_BITS) to get a WATCH_HASH_BITS-wide
     * index; tied to WATCH_HASH_SIZE via the static_assert above so the two
     * can't silently drift apart.
     */
    return ((unsigned int)wd * 2654435761u) >> (32 - WATCH_HASH_BITS);
}

/* Forward declaration: str_hash is defined below in the file cache section,
 * but the watch table's path index (added for O(1) path lookup/removal)
 * needs it here too. Declared once at file scope to avoid a duplicate
 * implementation. */
static unsigned long str_hash(const char* str);

static inline unsigned int hash_path(const char* path) {
    return (unsigned int)(str_hash(path) & (WATCH_PATH_HASH_SIZE - 1));
}

static watch_node_t* watch_node_create(int wd, const char* path) {
    size_t path_len = strlen(path);
    watch_node_t* node = malloc(sizeof(*node) + path_len + 1);
    if (!node) return NULL;

    node->wd = wd;
    node->next = NULL;
    node->path_next = NULL;
    memcpy(node->path, path, path_len + 1);
    return node;
}

static void watch_node_destroy(watch_node_t* node) { free(node); }

/**
 * Look up a watch's path by watch descriptor. O(1) average case via the
 * wd-keyed table.
 */
static const char* hash_lookup(cnotify_t* cn, int wd) {
    unsigned int idx = hash_wd(wd);
    watch_node_t* node = cn->table[idx];

    for (; node; node = node->next) {
        if (node->wd == wd) return node->path;
    }
    return NULL;
}

static int hash_remove(cnotify_t* cn, int wd);

/**
 * Insert a new watch_node into both the wd-keyed table and the path-keyed
 * secondary index. The two tables share the same set of node pointers, so
 * this is one allocation threaded onto two chains, not two allocations.
 */
static int hash_insert(cnotify_t* cn, int wd, const char* path) {
    const char* existing = hash_lookup(cn, wd);
    if (existing) {
        if (strcmp(existing, path) == 0) return 0;
        hash_remove(cn, wd);
    }

    watch_node_t* node = watch_node_create(wd, path);
    if (!node) return -1;

    unsigned int wd_idx = hash_wd(wd);
    node->next = cn->table[wd_idx];
    cn->table[wd_idx] = node;

    unsigned int path_idx = hash_path(path);
    node->path_next = cn->path_table[path_idx];
    cn->path_table[path_idx] = node;
    return 0;
}

/**
 * Unlink node from the wd-keyed table's collision chain at wd_idx.
 * Does not free the node or touch the path-keyed chain.
 */
static void wd_chain_unlink(cnotify_t* cn, unsigned int wd_idx, watch_node_t* node) {
    watch_node_t* curr = cn->table[wd_idx];
    watch_node_t* prev = NULL;
    for (; curr; prev = curr, curr = curr->next) {
        if (curr == node) {
            if (prev)
                prev->next = curr->next;
            else
                cn->table[wd_idx] = curr->next;
            return;
        }
    }
}

/**
 * Unlink node from the path-keyed table's collision chain at path_idx.
 * Does not free the node or touch the wd-keyed chain.
 */
static void path_chain_unlink(cnotify_t* cn, unsigned int path_idx, watch_node_t* node) {
    watch_node_t* curr = cn->path_table[path_idx];
    watch_node_t* prev = NULL;
    for (; curr; prev = curr, curr = curr->next) {
        if (curr == node) {
            if (prev)
                prev->path_next = curr->path_next;
            else
                cn->path_table[path_idx] = curr->path_next;
            return;
        }
    }
}

/**
 * Remove and free the watch_node for the given watch descriptor.
 * Removes the node from both the wd-keyed and path-keyed tables so the two
 * indices never fall out of sync.
 *
 * @return 0 if found and removed, -1 if not found (callers treat this as
 *         benign — the kernel queues events asynchronously, so a stale wd
 *         referring to an already-removed watch is normal).
 */
static int hash_remove(cnotify_t* cn, int wd) {
    unsigned int wd_idx = hash_wd(wd);
    watch_node_t* node = cn->table[wd_idx];

    for (; node; node = node->next) {
        if (node->wd == wd) break;
    }
    if (!node) return -1;

    wd_chain_unlink(cn, wd_idx, node);
    path_chain_unlink(cn, hash_path(node->path), node);
    watch_node_destroy(node);
    return 0;
}

/**
 * Remove and free every watch_node whose path matches path exactly.
 *
 * This is the path-keyed counterpart to hash_remove(): it uses
 * WATCH_PATH_HASH_SIZE's index instead of scanning all WATCH_HASH_SIZE
 * buckets of the wd-keyed table, turning what was an O(total watches) scan
 * into an O(matches) operation (average O(1) bucket lookup plus the length
 * of any hash collision chain for that path).
 *
 * Duplicate watches for the same path are handled: every node in the
 * bucket's chain that matches by path is removed, mirroring the previous
 * linear-scan behavior of cnotify_remove_watch().
 *
 * @return Number of watch_nodes removed (0 if none matched).
 */
static int hash_remove_by_path(cnotify_t* cn, const char* path) {
    unsigned int path_idx = hash_path(path);
    watch_node_t* node = cn->path_table[path_idx];
    int removed = 0;

    while (node) {
        watch_node_t* next = node->path_next;
        if (strcmp(node->path, path) == 0) {
            inotify_rm_watch(cn->fd, node->wd);
            path_chain_unlink(cn, path_idx, node);
            wd_chain_unlink(cn, hash_wd(node->wd), node);
            watch_node_destroy(node);
            removed++;
        }
        node = next;
    }
    return removed;
}

static void hash_clear(cnotify_t* cn) {
    for (unsigned int i = 0; i < WATCH_HASH_SIZE; i++) {
        watch_node_t* node = cn->table[i];
        while (node) {
            watch_node_t* next = node->next;
            watch_node_destroy(node);
            node = next;
        }
        cn->table[i] = NULL;
    }
    /* Nodes are already freed via the wd-keyed table above; just clear the
     * path-keyed table's now-dangling pointers without double-freeing. */
    for (unsigned int i = 0; i < WATCH_PATH_HASH_SIZE; i++) {
        cn->path_table[i] = NULL;
    }
}

/* ========================================================================
 * File metadata + content-hash cache
 *
 * On every relevant event we first compare st_size / st_mtime / st_ino.
 * Only when those differ do we open the file and compute a full djb2 hash.
 * This keeps the common "file touched but not actually changed" path cheap.
 * ======================================================================== */

/*
 * djb2 hash over a NUL-terminated string.
 * Using (unsigned char) avoids sign-extension for bytes > 127, making the
 * hash platform-independent regardless of whether char is signed or unsigned.
 */
static unsigned long str_hash(const char* str) {
    unsigned long hash = 5381;
    unsigned char c;
    while ((c = (unsigned char)*str++)) hash = ((hash << 5) + hash) + (unsigned long)c;
    return hash;
}

static inline unsigned int fcache_idx(const char* path) { return (unsigned int)(str_hash(path) % FILE_HASH_SIZE); }

/**
 * Compute djb2 content hash for the file at path.
 *
 * Uses a raw fd + read() rather than stdio: stdio's internal buffering
 * would add a second, redundant copy on top of the 64 KiB stack buffer
 * already used here, and read() lets us retry cleanly on EINTR instead of
 * treating a signal-interrupted read as a hard I/O error.
 *
 * Returns 0 on success, -1 on error (errno set).
 */
static int content_hash(const char* path, unsigned long* out) {
    unsigned char buf[65536]; /* 64 KiB read buffer — sweet spot for page cache */
    unsigned long hash = 5381;
    ssize_t n;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    while ((n = read(fd, buf, sizeof(buf))) != 0) {
        if (n < 0) {
            if (errno == EINTR) continue; /* interrupted by a signal — retry */
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
        for (ssize_t i = 0; i < n; i++) hash = ((hash << 5) + hash) + (unsigned long)buf[i];
    }

    close(fd);
    *out = hash;
    return 0;
}

/**
 * Look up a file_record_t by path.  Returns NULL if not cached.
 */
static file_record_t* fcache_lookup(cnotify_t* cn, const char* path) {
    file_record_t* rec = cn->fcache[fcache_idx(path)];
    for (; rec; rec = rec->next) {
        if (strcmp(rec->path, path) == 0) return rec;
    }
    return NULL;
}

/**
 * Insert a fully populated file_record_t into the cache.
 * The record stores the path inline via a flexible array member to avoid a
 * second allocation and improve cache locality.
 */
static file_record_t* fcache_insert(cnotify_t* cn, const char* path, unsigned long hash, const struct stat* st) {
    size_t path_len = strlen(path);
    file_record_t* rec = malloc(sizeof(*rec) + path_len + 1);
    if (!rec) return NULL;

    rec->content_hash = hash;
    rec->size = st->st_size;
    rec->mtime = st->st_mtime;
    rec->ino = st->st_ino;
    memcpy(rec->path, path, path_len + 1);

    unsigned int idx = fcache_idx(path);
    rec->next = cn->fcache[idx];
    cn->fcache[idx] = rec;
    return rec;
}

/**
 * Remove a file_record_t from the cache.
 * Returns 1 if a record was found and removed, 0 otherwise.
 */
static int fcache_remove(cnotify_t* cn, const char* path) {
    if (!cn || !path) return 0;
    unsigned int idx = fcache_idx(path);
    file_record_t* curr = cn->fcache[idx];
    file_record_t* prev = NULL;

    for (; curr; prev = curr, curr = curr->next) {
        if (strcmp(curr->path, path) == 0) {
            if (prev)
                prev->next = curr->next;
            else
                cn->fcache[idx] = curr->next;
            free(curr);
            return 1;
        }
    }
    return 0;
}

static void fcache_clear(cnotify_t* cn) {
    for (unsigned int i = 0; i < FILE_HASH_SIZE; i++) {
        file_record_t* rec = cn->fcache[i];
        while (rec) {
            file_record_t* next = rec->next;
            free(rec);
            rec = next;
        }
        cn->fcache[i] = NULL;
    }
}

/**
 * Check whether path has changed since it was last seen.
 *
 * Algorithm:
 *   1. stat(path) — fast syscall.
 *   2. If not in cache: seed the record, return false (no reload on first sight).
 *   3. If size/mtime/ino are identical: return false without opening the file.
 *   4. Only if metadata differs: compute content hash.
 *   5. If hash matches (e.g. editor touching mtime): update metadata, return false.
 *   6. Otherwise: update record, return true (genuine change).
 *
 * Returns 1 if the file genuinely changed, 0 if unchanged or on error.
 */
int cnotify_file_changed(cnotify_t* cn, const char* path) {
    if (!cn || !path) return 0;

    struct stat st;
    unsigned long new_hash;

    if (stat(path, &st) < 0) {
        /*
         * File transiently unavailable (e.g. mid-atomic-save rename).
         * Don't treat as a change — the paired MOVED_TO event will follow.
         */
        return 0;
    }

    file_record_t* rec = fcache_lookup(cn, path);

    if (!rec) {
        /* First encounter: seed hash so future changes are detectable. */
        if (content_hash(path, &new_hash) < 0) return 0;
        fcache_insert(cn, path, new_hash, &st);
        return 0; /* don't trigger reload on first sight */
    }

    /*
     * Fast path: all three metadata fields match — high confidence the
     * file is unchanged.  Skip content hashing entirely.
     */
    if (rec->size == st.st_size && rec->mtime == st.st_mtime && rec->ino == st.st_ino) return 0;

    /*
     * Metadata changed.  Compute content hash to confirm a real change.
     * This filters out editors that rewrite mtime without changing bytes
     * (e.g. `touch`), and atomic-rename workflows where the temp file
     * happened to have the same content.
     */
    if (content_hash(path, &new_hash) < 0) return 0;

    /* Update cached metadata regardless of hash result. */
    rec->size = st.st_size;
    rec->mtime = st.st_mtime;
    rec->ino = st.st_ino;

    if (rec->content_hash == new_hash) return 0; /* content identical */

    rec->content_hash = new_hash;
    return 1; /* genuine change */
}

/**
 * Remove a file from the change-detection cache.
 * Called on DELETE/MOVE_FROM events so stale entries don't accumulate.
 * Returns 1 if a record was found and removed, 0 otherwise.
 */
int cnotify_file_remove(cnotify_t* cn, const char* path) { return fcache_remove(cn, path); }

/* ========================================================================
 * Path utilities
 * ======================================================================== */

/**
 * Allocate and return "dir/name" with overflow checking.
 *
 * Both operands are validated against PATH_MAX individually before the
 * combined-length arithmetic runs, so the size_t subtraction below can
 * never wrap around (which the naive "dir_len > PATH_MAX - name_len - 2"
 * check does when name_len alone exceeds PATH_MAX - 2).
 *
 * Returns NULL on allocation failure or path too long.
 */
static char* path_join(const char* dir, const char* name) {
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);

    /* Reject oversized operands up front so the subtraction below is safe. */
    if (dir_len >= PATH_MAX || name_len >= PATH_MAX) return NULL;
    if (dir_len > PATH_MAX - name_len - 2) return NULL; /* overflow guard */

    int needs_slash = (dir_len > 0 && dir[dir_len - 1] != '/');
    size_t total = dir_len + (size_t)needs_slash + name_len + 1;

    char* path = malloc(total);
    if (!path) return NULL;

    memcpy(path, dir, dir_len);
    if (needs_slash) path[dir_len] = '/';
    memcpy(path + dir_len + needs_slash, name, name_len + 1);
    return path;
}

static inline unsigned int exclude_idx(const char* name) { return (unsigned int)(str_hash(name) % EXCLUDE_HASH_SIZE); }

/**
 * Return true if name appears in the exclude set.
 * O(1) average case via exclude_idx(), versus a linear scan of the
 * original exclude_dirs array.
 */
static bool is_excluded(cnotify_t* cn, const char* name) {
    exclude_node_t* node = cn->exclude_set[exclude_idx(name)];
    for (; node; node = node->next) {
        if (strcmp(node->name, name) == 0) return true;
    }
    return false;
}

/**
 * Build cn->exclude_set from cn->exclude_dirs (a NULL-terminated array of
 * borrowed strings). Called once per cnotify_add_watch(), turning what
 * would otherwise be an O(k) linear scan per directory entry — across
 * every entry in every directory walked, for both watch setup and
 * pre-seeding — into an O(1) average-case hash lookup per entry.
 *
 * The exclude_node_t entries only borrow pointers into exclude_dirs, which
 * the caller guarantees stays valid for the lifetime of cn (see
 * cnotify_add_watch()'s doc comment), so no string duplication is needed.
 *
 * @return 0 on success, -1 on allocation failure.
 */
static int exclude_set_build(cnotify_t* cn, const char* const* exclude_dirs) {
    if (!exclude_dirs) return 0;

    for (const char* const* ex = exclude_dirs; *ex; ex++) {
        if (is_excluded(cn, *ex)) continue;

        exclude_node_t* node = malloc(sizeof(*node));
        if (!node) return -1; /* caller (cnotify_add_watch) tears down on failure */

        node->name = *ex;
        unsigned int idx = exclude_idx(*ex);
        node->next = cn->exclude_set[idx];
        cn->exclude_set[idx] = node;
    }
    return 0;
}

/**
 * Free every exclude_node_t in cn->exclude_set. Does not free the
 * borrowed name strings themselves — those belong to the caller.
 */
static void exclude_set_clear(cnotify_t* cn) {
    for (unsigned int i = 0; i < EXCLUDE_HASH_SIZE; i++) {
        exclude_node_t* node = cn->exclude_set[i];
        while (node) {
            exclude_node_t* next = node->next;
            free(node);
            node = next;
        }
        cn->exclude_set[i] = NULL;
    }
}

/* ========================================================================
 * Watch management
 * ======================================================================== */

/**
 * Add an inotify watch for a single directory.
 * Returns the watch descriptor on success, -1 on error.
 */
static int add_watch_single(cnotify_t* cn, const char* path) {
    int wd = inotify_add_watch(cn->fd, path, WATCH_MASK);
    if (wd < 0) return -1;

    if (hash_insert(cn, wd, path) < 0) {
        inotify_rm_watch(cn->fd, wd);
        errno = ENOMEM;
        return -1;
    }
    return wd;
}

/**
 * Determine whether a directory entry is a directory.
 *
 * Prefers d_type for speed (avoids a stat call on most Linux filesystems).
 * Falls back to lstat() when d_type is DT_UNKNOWN (network mounts, older
 * kernels).
 *
 * Symlinks (DT_LNK) are deliberately NOT resolved and NOT treated as
 * directories here: recursing through a symlink can walk back into an
 * ancestor directory (a symlink cycle), which would otherwise cause
 * add_watch_recursive() / preseed_recurse() to recurse without bound and
 * exhaust the stack. Watching the *contents* of symlinked directories is
 * not supported; only real directories are recursed into.
 *
 * @param parent  Parent directory path.
 * @param entry   Directory entry from readdir().
 * @return true if the entry resolves to a real (non-symlink) directory.
 */
static bool entry_is_dir(const char* parent, const struct dirent* entry) {
    if (entry->d_type == DT_DIR) return true;
    if (entry->d_type != DT_UNKNOWN) return false; /* excludes DT_LNK on purpose */

    /* Uncertain — fall back to lstat so a symlink is never reported as a dir. */
    char* full = path_join(parent, entry->d_name);
    if (!full) return false;

    struct stat st;
    bool is_dir = (lstat(full, &st) == 0 && S_ISDIR(st.st_mode));
    free(full);
    return is_dir;
}

/**
 * Determine whether a directory entry is a regular file.
 *
 * Same DT_UNKNOWN fallback as entry_is_dir(); symlinks are likewise not
 * resolved, for consistency with the "don't follow symlinks" policy above.
 */
static bool entry_is_reg(const char* parent, const struct dirent* entry) {
    if (entry->d_type == DT_REG) return true;
    if (entry->d_type != DT_UNKNOWN) return false;

    char* full = path_join(parent, entry->d_name);
    if (!full) return false;

    struct stat st;
    bool is_reg = (lstat(full, &st) == 0 && S_ISREG(st.st_mode));
    free(full);
    return is_reg;
}

/**
 * Recursively add inotify watches to the directory tree rooted at path.
 *
 * @return 0 on success (partial failures due to permissions are tolerated),
 *        -1 on a fatal error (ENOSPC: kernel watch limit exceeded, or OOM).
 */
static int add_watch_recursive(cnotify_t* cn, const char* path) {
    if (add_watch_single(cn, path) < 0) {
        /*
         * ENOSPC: hit /proc/sys/fs/inotify/max_user_watches — fatal, the
         * watcher would be silently incomplete. ENOMEM/EMFILE are also fatal.
         * All other errors (EACCES, ENOENT) are non-fatal; log and continue.
         */
        if (errno == ENOSPC || errno == ENOMEM || errno == EMFILE) return -1;
        return 0;
    }

    DIR* dir = opendir(path);
    if (!dir) return 0; /* watch added; can't recurse — acceptable */

    struct dirent* entry;
    while (1) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) break;

        /* Skip . and .. */
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;

        if (is_excluded(cn, entry->d_name)) continue;

        if (!entry_is_dir(path, entry)) continue;

        char* child = path_join(path, entry->d_name);
        if (!child) {
            int saved = errno;
            closedir(dir);
            errno = saved;
            return -1; /* OOM is fatal */
        }

        int ret = add_watch_recursive(cn, child);
        free(child);

        if (ret < 0) {
            int saved = errno;
            closedir(dir);
            errno = saved;
            return -1;
        }
    }

    int saved = errno; /* non-zero only if readdir itself failed */
    closedir(dir);
    if (saved != 0) {
        errno = saved;
        return -1;
    }
    return 0;
}

/* ========================================================================
 * File cache pre-seeding
 *
 * Walk the directory tree before entering the event loop and hash every
 * regular file.  Without this, the first modification to any file after
 * startup would always appear as a "new" file, returning 0 from
 * cnotify_file_changed() and suppressing the reload.
 * ======================================================================== */

static void preseed_recurse(cnotify_t* cn, const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        if (is_excluded(cn, ent->d_name)) continue;

        char* child = path_join(dir, ent->d_name);
        if (!child) continue;

        if (entry_is_dir(dir, ent)) {
            preseed_recurse(cn, child);
        } else if (entry_is_reg(dir, ent)) {
            /*
             * Seed content hash + metadata.  Errors (permission denied,
             * file disappeared) are silently skipped — the next event on
             * that file will seed it at that point.
             */
            struct stat st;
            unsigned long hash;
            if (stat(child, &st) == 0 && content_hash(child, &hash) == 0) fcache_insert(cn, child, hash, &st);
        }
        free(child);
    }
    closedir(d);
}

/* ========================================================================
 * Event processing
 * ======================================================================== */

static cnotify_event_type_t mask_to_event_type(uint32_t mask) {
    if (mask & IN_MODIFY) return CNOTIFY_EVENT_MODIFY;
    if (mask & IN_CREATE) return CNOTIFY_EVENT_CREATE;
    if (mask & (IN_DELETE | IN_DELETE_SELF)) return CNOTIFY_EVENT_DELETE;
    if (mask & (IN_MOVED_FROM | IN_MOVED_TO | IN_MOVE_SELF)) return CNOTIFY_EVENT_MOVE;
    if (mask & IN_ATTRIB) return CNOTIFY_EVENT_ATTRIB;
    if (mask & IN_CLOSE_WRITE) return CNOTIFY_EVENT_CLOSE_WRITE;
    return CNOTIFY_EVENT_MODIFY; /* fallback */
}

/**
 * Process one inotify event and invoke the user callback.
 *
 * @param ie  Pointer to the event header, aligned and located directly in
 *            cn->event_buf.  ie->name (if ie->len > 0) points into the same
 *            buffer, immediately following the fixed-size header. The
 *            caller (process_events_once) has already verified that both
 *            the header and the full name field lie within the bytes
 *            actually returned by read(), so this function does not need
 *            to re-validate ie->len against the buffer bounds.
 * @return The callback's return value, or 0 if the event was suppressed.
 */
static int handle_event(cnotify_t* cn, const struct inotify_event* ie, cnotify_callback_t callback, void* userdata) {
    /*
     * IN_IGNORED fires when the kernel confirms a watch removal (either
     * because we called inotify_rm_watch(), or because IN_DELETE_SELF
     * was processed).
     */
    if (ie->mask & IN_IGNORED) {
        hash_remove(cn, ie->wd);
        return 0;
    }

    if (ie->len > 0 && is_excluded(cn, ie->name)) return 0;

    const char* dir_path = hash_lookup(cn, ie->wd);
    if (!dir_path) {
        /*
         * Stale event for a watch that was already removed.  This is
         * normal: the kernel queues events asynchronously.
         */
        return 0;
    }

    /*
     * New subdirectory created: add watches recursively *before* invoking
     * the callback so the user sees a fully-watched tree on return.
     */
    if ((ie->mask & IN_CREATE) && (ie->mask & IN_ISDIR) && ie->len > 0) {
        char* new_path = path_join(dir_path, ie->name);
        if (new_path) {
            add_watch_recursive(cn, new_path);
            free(new_path);
        }
    }

    cnotify_event_t event = {
        .type = mask_to_event_type(ie->mask),
        .path = dir_path,
        .name = (ie->len > 0) ? ie->name : "",
        .cookie = ie->cookie,
        .is_dir = !!(ie->mask & IN_ISDIR),
    };

    /*
     * IN_DELETE_SELF / IN_MOVE_SELF mean the watched directory itself is gone or moved.
     * Clean up from hash tables after constructing the event so dir_path remains valid.
     */
    if (ie->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) hash_remove(cn, ie->wd);

    return callback(&event, userdata);
}

/**
 * Read one buffer's worth of events from the inotify fd and dispatch them.
 *
 * struct inotify_event has a flexible array member (name[]), so events are
 * read directly out of cn->event_buf without copying: the buffer was
 * allocated with aligned_alloc() using the type's required alignment, and
 * the kernel guarantees each successive event within the buffer starts at
 * a correctly aligned offset. Casting the buffer pointer to
 * struct inotify_event* is therefore well-defined, not a strict-aliasing
 * violation.
 *
 * Each event's fixed header and its trailing name field are bounds-checked
 * against the number of bytes actually returned by read() before being
 * dereferenced, so a short/truncated record at the tail of the buffer is
 * safely rejected instead of walking past `end`.
 *
 * @return 1 on successful buffer read and process, 0 if queue is empty (EAGAIN),
 *         or -1 on read error.
 */
static int process_events_once(cnotify_t* cn, cnotify_callback_t callback, void* userdata, int* stop_flag,
                               int* events_processed) {
    ssize_t len = read(cn->fd, cn->event_buf, cn->event_buf_size);
    if (len < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    if (len == 0) {
        /* Should not happen with inotify. */
        errno = EINVAL;
        return -1;
    }

    int processed = 0;
    char* ptr = cn->event_buf;
    char* end = cn->event_buf + len;

    while (ptr < end && !*stop_flag) {
        /* Guard against a truncated header at the tail of the buffer. */
        if ((size_t)(end - ptr) < INOTIFY_EVENT_SIZE) break;

        struct inotify_event* iep = (struct inotify_event*)ptr;

        /* Guard against a truncated name field before touching iep->name. */
        if ((size_t)(end - ptr) < INOTIFY_EVENT_SIZE + iep->len) break;

        int ret = handle_event(cn, iep, callback, userdata);
        if (ret != 0) {
            *stop_flag = 1;
            break;
        }

        processed++;
        ptr += INOTIFY_EVENT_SIZE + iep->len;
    }

    if (events_processed) *events_processed += processed;
    return 1;
}

/**
 * Set the inotify fd non-blocking, drain all available events, then restore
 * the original flags.
 *
 * @return Total events processed (>= 0), or -1 on a fatal error.
 */
static int process_events_drain(cnotify_t* cn, cnotify_callback_t callback, void* userdata, int* stop_flag) {
    int flags = fcntl(cn->fd, F_GETFL, 0);
    if (flags < 0) return -1;

    if (fcntl(cn->fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

    int total = 0;
    while (!*stop_flag) {
        int count = 0;
        int ret = process_events_once(cn, callback, userdata, stop_flag, &count);
        if (ret < 0) {
            int saved = errno;
            (void)fcntl(cn->fd, F_SETFL, flags); /* best-effort restore */
            errno = saved;
            return -1;
        }
        total += count;
        if (ret == 0) break; /* EAGAIN: queue empty */
    }

    /*
     * Restore blocking mode.  If this fails the fd is left non-blocking,
     * which breaks the next poll()/read() cycle in the event loop.  This is
     * an extremely unlikely OS error but we propagate it rather than
     * silently corrupting the loop's blocking semantics.
     */
    if (fcntl(cn->fd, F_SETFL, flags) < 0) return -1;
    return total;
}

/**
 * Drain and discard whatever bytes are sitting in the stop self-pipe.
 *
 * Only ever needs to consume the sentinel byte(s) written by
 * cnotify_request_stop(); the actual value is irrelevant.  The pipe's read
 * end is non-blocking (set once at creation time), so this returns promptly
 * even if, in principle, nothing were available.
 */
static void drain_stop_pipe(cnotify_t* cn) {
    char buf[64];
    ssize_t n;
    do {
        n = read(cn->stop_pipe[0], buf, sizeof(buf));
    } while (n > 0 || (n < 0 && errno == EINTR));
}

/* ========================================================================
 * Public API
 * ======================================================================== */

/**
 * Allocate and initialise a cnotify context.
 *
 * Uses aligned_alloc() for the event buffer so that casting char* to
 * struct inotify_event* is strictly conforming (the kernel guarantees
 * events within the buffer are themselves aligned).
 *
 * @return Pointer to a new context, or NULL on error (errno set).
 */
cnotify_t* cnotify_init(void) {
    int fd = inotify_init1(IN_CLOEXEC);
    if (fd < 0) return NULL;

    cnotify_t* cn = calloc(1, sizeof(*cn));
    if (!cn) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }

    /*
     * aligned_alloc requires size to be a multiple of alignment.
     * Round INOTIFY_BUF_LEN up to the next multiple of the alignment
     * of struct inotify_event.
     */
    size_t align = alignof(struct inotify_event);
    size_t buf_size = (INOTIFY_BUF_LEN + align - 1) & ~(align - 1);
    cn->event_buf = aligned_alloc(align, buf_size);
    if (!cn->event_buf) {
        close(fd);
        free(cn);
        errno = ENOMEM;
        return NULL;
    }

    /*
     * Self-pipe for async-signal-safe shutdown requests.
     *
     * cnotify_request_stop() only calls write(), which is async-signal-safe
     * per POSIX, so it can be invoked directly from a signal handler.  The
     * blocking loop then observes the pipe becoming readable via poll() and
     * exits its own stack frame normally instead of the handler calling
     * exit() out from under partially-run cleanup code.
     */
    if (pipe(cn->stop_pipe) < 0) {
        int saved = errno;
        close(fd);
        free(cn->event_buf);
        free(cn);
        errno = saved;
        return NULL;
    }

    /*
     * Both ends non-blocking:
     *   - read end: so drain_stop_pipe() can't block if called speculatively.
     *   - write end: so a signal handler calling cnotify_request_stop()
     *     never blocks even in the (extremely unlikely) case the pipe
     *     buffer is full; losing a redundant wakeup byte is harmless
     *     because one byte is all that's ever needed.
     */
    for (int i = 0; i < 2; i++) {
        int pflags = fcntl(cn->stop_pipe[i], F_GETFL, 0);
        if (pflags >= 0) fcntl(cn->stop_pipe[i], F_SETFL, pflags | O_NONBLOCK);

        int fdflags = fcntl(cn->stop_pipe[i], F_GETFD, 0);
        if (fdflags >= 0) fcntl(cn->stop_pipe[i], F_SETFD, fdflags | FD_CLOEXEC);
    }

    cn->fd = fd;
    cn->event_buf_size = buf_size;
    cn->debounce_ms = DEFAULT_DEBOUNCE_MS;

    return cn;
}

/**
 * Destroy a cnotify context and release all associated resources.
 * Safe to call with NULL.
 */
void cnotify_destroy(cnotify_t* cn) {
    if (!cn) return;
    if (cn->fd >= 0) close(cn->fd);
    if (cn->stop_pipe[0] >= 0) close(cn->stop_pipe[0]);
    if (cn->stop_pipe[1] >= 0) close(cn->stop_pipe[1]);
    hash_clear(cn);
    fcache_clear(cn);
    exclude_set_clear(cn);
    free(cn->event_buf);
    free(cn);
}

/**
 * Recursively watch path and its subdirectories.
 *
 * exclude_dirs is a NULL-terminated array of directory base names to skip
 * (e.g. {"node_modules", ".git", NULL}).  The caller retains ownership;
 * the pointer must remain valid for the lifetime of cn (exclude_set_build()
 * only borrows the individual name pointers, it does not duplicate them).
 *
 * The exclude list is hashed into cn->exclude_set once here rather than
 * scanned linearly on every directory entry encountered during the walk
 * below, in pre-seeding, and in every subsequent event.
 *
 * Also pre-seeds the file content cache so that the first modification to
 * any existing file is correctly detected as a change.
 *
 * @return 0 on success, -1 on error (errno set).
 */
int cnotify_add_watch(cnotify_t* cn, const char* path, const char* const* exclude_dirs) {
    if (!cn || !path) {
        errno = EINVAL;
        return -1;
    }

    struct stat st;
    if (stat(path, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    cn->exclude_dirs = (char**)exclude_dirs;

    if (exclude_set_build(cn, exclude_dirs) < 0) {
        exclude_set_clear(cn); /* partial build: leave cn in a clean state */
        errno = ENOMEM;
        return -1;
    }

    if (add_watch_recursive(cn, path) < 0) return -1;

    preseed_recurse(cn, path);
    return 0;
}

/**
 * Remove all watches whose path matches path exactly.
 *
 * Uses the path-keyed secondary index (hash_remove_by_path()) rather than
 * scanning every bucket of the wd-keyed table, so cost is proportional to
 * the number of matching watches (average O(1) plus collision-chain
 * length) instead of the total number of watches held.
 *
 * @return 0 if at least one watch was removed, -1 if none found (errno=ENOENT).
 */
int cnotify_remove_watch(cnotify_t* cn, const char* path) {
    if (!cn || !path) {
        errno = EINVAL;
        return -1;
    }

    if (hash_remove_by_path(cn, path) == 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

/**
 * Set the debounce interval.
 * After an event fires, cnotify_start_loop sleeps for this many milliseconds
 * to allow bursty writes to coalesce before draining the queue.
 */
void cnotify_set_debounce(cnotify_t* cn, unsigned int milliseconds) {
    if (cn) cn->debounce_ms = milliseconds;
}

/**
 * Return the underlying inotify file descriptor.
 * Useful for integrating into an existing event loop (poll/epoll/select).
 * Returns -1 if cn is NULL.
 */
int cnotify_get_fd(cnotify_t* cn) { return cn ? cn->fd : -1; }

/**
 * Drain and process all currently pending events without blocking.
 *
 * @return Number of events processed, or -1 on error (errno set).
 */
int cnotify_process_events(cnotify_t* cn, cnotify_callback_t callback, void* userdata) {
    if (!cn || !callback) {
        errno = EINVAL;
        return -1;
    }
    int stop = 0;
    return process_events_drain(cn, callback, userdata, &stop);
}

/**
 * Request that a running cnotify_start_loop() stop as soon as possible.
 *
 * Implemented as a single non-blocking write() to an internal pipe, both of
 * which are async-signal-safe operations per POSIX (unlike, say, calling
 * exit() or free() directly from a handler). cnotify_start_loop() wakes from
 * poll() when the pipe's read end becomes readable, drains it, and returns 0
 * from its own stack frame — so ordinary cleanup code around the call site
 * still runs normally.
 */
void cnotify_request_stop(cnotify_t* cn) {
    if (!cn) return;
    /* Value written is irrelevant; only presence of a byte matters. */
    ssize_t ret = write(cn->stop_pipe[1], "x", 1);
    (void)ret; /* Best-effort: EAGAIN (pipe full) just means a wakeup is already pending. */
}

/**
 * Run a blocking event loop until the callback returns non-zero or
 * cnotify_request_stop() is called.
 *
 * Uses poll(2) to block on two file descriptors at once: the inotify fd
 * (real filesystem events) and the read end of the internal stop pipe
 * (async shutdown requests). A single timeout parameter on that same
 * poll() call implements the post-event debounce wait, so waiting for more
 * data and waiting out the debounce period are unified into one blocking
 * call instead of two.
 *
 * The net effect of the debounce step is that a rapid burst of writes
 * (e.g. compiler output) fires the callback once rather than once per file.
 *
 * @return 0 when the loop stopped cleanly (callback request or stop
 *         request), -1 on a fatal error (errno set).
 */
int cnotify_start_loop(cnotify_t* cn, cnotify_callback_t callback, void* userdata) {
    if (!cn || !callback) {
        errno = EINVAL;
        return -1;
    }

    struct pollfd fds[POLLFD_COUNT];
    fds[POLLFD_INOTIFY].fd = cn->fd;
    fds[POLLFD_INOTIFY].events = POLLIN;
    fds[POLLFD_STOPPIPE].fd = cn->stop_pipe[0];
    fds[POLLFD_STOPPIPE].events = POLLIN;

    int stop = 0;
    while (!stop) {
        fds[POLLFD_INOTIFY].revents = 0;
        fds[POLLFD_STOPPIPE].revents = 0;

        int ret = poll(fds, POLLFD_COUNT, -1 /* block indefinitely */);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        if (fds[POLLFD_STOPPIPE].revents & (POLLIN | POLLHUP | POLLERR)) {
            drain_stop_pipe(cn);
            break;
        }

        if (!(fds[POLLFD_INOTIFY].revents & POLLIN)) {
            /* Spurious wakeup or POLLERR/POLLHUP on the inotify fd. */
            if (fds[POLLFD_INOTIFY].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                errno = EBADF;
                return -1;
            }
            continue;
        }

        /*
         * Debounce wait: give the kernel a chance to accumulate more events
         * from a burst write before we drain the queue.  This second poll()
         * call blocks with a timeout on the *same* two descriptors, so a
         * stop request arriving mid-debounce is honoured immediately rather
         * than waiting out the full debounce period first.
         */
        if (cn->debounce_ms > 0) {
            fds[POLLFD_STOPPIPE].revents = 0;
            int dret = poll(&fds[POLLFD_STOPPIPE], 1, (int)cn->debounce_ms);
            if (dret < 0 && errno != EINTR) return -1;
            if (dret > 0 && (fds[POLLFD_STOPPIPE].revents & (POLLIN | POLLHUP | POLLERR))) {
                drain_stop_pipe(cn);
                break;
            }
        }

        ret = process_events_drain(cn, callback, userdata, &stop);
        if (ret < 0 && errno != EINTR) return -1;
    }

    return 0;
}

/**
 * Return a human-readable name for a cnotify_event_type_t value.
 * The returned string is a string literal; do not free it.
 */
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
