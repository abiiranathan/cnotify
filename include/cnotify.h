#ifndef CNOTIFY_H
#define CNOTIFY_H

#include <limits.h>
#include <sys/inotify.h>

/**
 * @file cnotify.h
 * @brief A simple, recursive file system watcher library for Linux based on inotify.
 */

typedef struct CNotify CNotify;

/**
 * @enum CNotifyEventType
 * @brief Types of events that can be detected.
 */
typedef enum {
    CNOTIFY_EVENT_MODIFY = 1,
    CNOTIFY_EVENT_CREATE,
    CNOTIFY_EVENT_DELETE,
    CNOTIFY_EVENT_MOVE,
    CNOTIFY_EVENT_UNKNOWN,
    CNOTIFY_EVENT_ERROR
} CNotifyEventType;

/**
 * @struct CNotifyEvent
 * @brief Structure representing a file system event.
 */
typedef struct {
    CNotifyEventType type; /**< Type of the event */
    char* path;            /**< Directory path where the event occurred */
    char* filename;        /**< Name of the file associated with the event */
} CNotifyEvent;

/**
 * @typedef CNotifyCallback
 * @brief Callback function type for handling events.
 *
 * @param event Pointer to the event details.
 * @param user_data Pointer to user-provided data.
 * @return 0 to continue watching, non-zero to stop the loop.
 */
typedef int (*CNotifyCallback)(CNotifyEvent* event, void* user_data);

/**
 * @brief Initializes a new cnotify context.
 *
 * Allocates memory for the context and initializes the inotify instance.
 *
 * @return Pointer to a new CNotify context, or NULL on failure.
 */
CNotify* cnotify_init(void);

/**
 * @brief Frees a cnotify context and releases all resources.
 *
 * Closes the inotify file descriptor and frees all allocated memory.
 *
 * @param cn Pointer to the CNotify context.
 */
void cnotify_free(CNotify* cn);

/**
 * @brief Adds a directory and its subdirectories to the watch list.
 *
 * Recursively watches the specified path. Directories in the exclude list
 * are skipped.
 *
 * @param cn Pointer to the CNotify context.
 * @param path The directory path to watch.
 * @param exclude_dirs NULL-terminated array of directory names to exclude (e.g., {".git", NULL}).
 * @return 0 on success, -1 on failure.
 */
int cnotify_add_watch(CNotify* cn, const char* path, const char** exclude_dirs);

/**
 * @brief Sets the debounce time for event processing.
 *
 * Groups rapid events into a single callback trigger to avoid flooding.
 *
 * @param cn Pointer to the CNotify context.
 * @param ms Debounce time in milliseconds (default: 100ms).
 */
void cnotify_set_debounce(CNotify* cn, int ms);

/**
 * @brief Starts the event listening loop.
 *
 * This function blocks until the callback returns non-zero or an error occurs.
 *
 * @param cn Pointer to the CNotify context.
 * @param cb The callback function to invoke when an event occurs.
 * @param user_data User data to pass to the callback.
 */
void cnotify_start_loop(CNotify* cn, CNotifyCallback cb, void* user_data);

#endif
