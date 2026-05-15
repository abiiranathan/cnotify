#ifndef CNOTIFY_H
#define CNOTIFY_H

#include <stddef.h>
#include <stdint.h>

/**
 * @file cnotify.h
 * @brief Recursive file system watcher library for Linux using inotify
 *
 * This library provides a simple, efficient interface for monitoring directory
 * trees for file system events. It handles recursive watching, dynamic directory
 * creation/deletion, and event debouncing.
 *
 * Thread safety: This library is NOT thread-safe. Each CNotify instance should
 * be used from a single thread only.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cnotify cnotify_t;

/**
 * @enum cnotify_event_type
 * @brief File system event types
 */
typedef enum {
    CNOTIFY_EVENT_MODIFY = 1,  /**< File was modified */
    CNOTIFY_EVENT_CREATE,      /**< File/directory was created */
    CNOTIFY_EVENT_DELETE,      /**< File/directory was deleted */
    CNOTIFY_EVENT_MOVE,        /**< File/directory was moved */
    CNOTIFY_EVENT_ATTRIB,      /**< Metadata changed (permissions, timestamps, etc) */
    CNOTIFY_EVENT_CLOSE_WRITE, /**< File opened for writing was closed */
} cnotify_event_type_t;

/**
 * @struct cnotify_event
 * @brief File system event information
 *
 * All string pointers are valid only for the duration of the callback.
 * If you need to keep them, make copies.
 */
typedef struct {
    cnotify_event_type_t type; /**< Event type */
    const char* path;          /**< Directory path (never NULL) */
    const char* name;          /**< File/directory name (never NULL) */
    uint32_t cookie;           /**< Cookie for matching MOVE_FROM/MOVE_TO events */
    int is_dir;                /**< Non-zero if event applies to a directory */
} cnotify_event_t;

/**
 * @typedef cnotify_callback_t
 * @brief Event callback function
 *
 * @param event Event information (valid only during callback)
 * @param userdata User-provided context pointer
 * @return 0 to continue watching, non-zero to stop the event loop
 */
typedef int (*cnotify_callback_t)(const cnotify_event_t* event, void* userdata);

/**
 * @brief Initialize a new cnotify watcher
 *
 * @return Pointer to new watcher context, or NULL on failure (check errno)
 *
 * Possible errno values:
 *   EMFILE - User limit on total number of inotify instances reached
 *   ENFILE - System limit on total number of file descriptors reached
 *   ENOMEM - Insufficient kernel memory
 */
cnotify_t* cnotify_init(void);

/**
 * @brief Clean up and free a cnotify context
 *
 * @param cn Watcher context (NULL is safe to pass)
 *
 * This removes all watches and closes the inotify descriptor.
 * After calling this, the pointer is invalid.
 */
void cnotify_destroy(cnotify_t* cn);

/**
 * @brief Add a directory tree to the watch list
 *
 * @param cn Watcher context
 * @param path Directory path to watch recursively
 * @param exclude_dirs NULL-terminated array of directory basenames to skip (may be NULL)
 * @return 0 on success, -1 on error (check errno)
 *
 * This function walks the directory tree and adds inotify watches recursively.
 * Directories matching any name in exclude_dirs are skipped entirely.
 *
 * Common exclude patterns: ".git", ".svn", "node_modules", "__pycache__"
 *
 * Possible errno values:
 *   EACCES - Read access denied
 *   ENOENT - Path does not exist
 *   ENOTDIR - Path is not a directory
 *   ENOMEM - Out of memory
 *   ENOSPC - User limit on inotify watches reached
 */
int cnotify_add_watch(cnotify_t* cn, const char* path, const char* const* exclude_dirs);

/**
 * @brief Remove a watch on a specific path
 *
 * @param cn Watcher context
 * @param path Path that was previously watched
 * @return 0 on success, -1 if path was not being watched
 */
int cnotify_remove_watch(cnotify_t* cn, const char* path);

/**
 * @brief Set event debounce interval
 *
 * @param cn Watcher context
 * @param milliseconds Debounce time (0 to disable, default is 100ms)
 *
 * When events occur in rapid succession, only one callback will be triggered
 * after the debounce period expires. This is useful for tools that rebuild/reload
 * on file changes and don't need every individual event.
 */
void cnotify_set_debounce(cnotify_t* cn, unsigned int milliseconds);

/**
 * @brief Get the inotify file descriptor
 *
 * @param cn Watcher context
 * @return File descriptor, or -1 if cn is NULL
 *
 * This allows integration with event loops (poll, epoll, select, etc).
 * You can use cnotify_process_events() to handle events when the fd is ready.
 */
int cnotify_get_fd(cnotify_t* cn);

/**
 * @brief Process pending events (non-blocking)
 *
 * @param cn Watcher context
 * @param callback Event callback function
 * @param userdata User context pointer passed to callback
 * @return Number of events processed, or -1 on error (check errno)
 *
 * This reads and processes all available events from the inotify fd.
 * If no events are available, returns 0 immediately (non-blocking).
 *
 * Use this when integrating with your own event loop.
 */
int cnotify_process_events(cnotify_t* cn, cnotify_callback_t callback, void* userdata);

/**
 * @brief Start blocking event loop
 *
 * @param cn Watcher context
 * @param callback Event callback function
 * @param userdata User context pointer passed to callback
 * @return 0 on clean exit, -1 on error (check errno)
 *
 * This blocks until either:
 *   - The callback returns non-zero
 *   - An error occurs
 *   - The process receives a signal
 *
 * Common errno values on return:
 *   EINTR - Interrupted by signal
 *   EBADF - Invalid file descriptor
 */
int cnotify_start_loop(cnotify_t* cn, cnotify_callback_t callback, void* userdata);

/**
 * @brief Get human-readable event type name
 *
 * @param type Event type
 * @return String name (never NULL, returns "UNKNOWN" for invalid types)
 */
const char* cnotify_event_type_name(cnotify_event_type_t type);


/**
 * Check whether the file at path has changed since it was last seen.
 * Seeds the cache on first call. Returns 1 if changed, 0 if not.
 */
int cnotify_file_changed(cnotify_t* cn, const char* path);

/**
 * Remove path from the change-detection cache.
 * Returns 1 if found and removed, 0 otherwise.
 */
int cnotify_file_remove(cnotify_t* cn, const char* path);

#ifdef __cplusplus
}
#endif

#endif /* CNOTIFY_H */
