#ifndef _GNU_SOURCE
    #define _GNU_SOURCE
#endif

#include <ctype.h>      /* for isspace */
#include <dirent.h>     /* for directory iteration in cnotify */
#include <errno.h>      /* for errno, ECHILD, EINTR */
#include <fnmatch.h>    /* for fnmatch pattern matching */
#include <limits.h>     /* for PATH_MAX */
#include <signal.h>     /* for signal, SIGTERM, SIGINT, SIGKILL */
#include <stdio.h>      /* for printf, fprintf, fopen, fgets, snprintf */
#include <stdlib.h>     /* for malloc, realloc, free, strdup, exit, atoi, system */
#include <string.h>     /* for strcmp, strncmp, strchr, strrchr, strlen, strtok */
#include <sys/select.h> /* for select — used as a portable, SIGALRM-safe sleep */
#include <sys/stat.h>   /* for struct stat (used transitively by cnotify) */
#include <sys/types.h>  /* for pid_t, size_t */
#include <sys/wait.h>   /* for waitpid, WNOHANG */
#include <unistd.h>     /* for fork, execv, setpgid, access, _exit, usleep */

#include "../include/cnotify.h"

/* =========================================================================
 * Constants
 * ========================================================================= */

/** Maximum grace period in milliseconds before escalating SIGTERM → SIGKILL. */
#define TERM_TIMEOUT_MS 2000

/** Default head-start given to pre_cmd before run_cmd is launched. */
#define DEFAULT_PRE_GRACE_MS 500

/** Severity levels understood by the logger. */
typedef enum {
    LOG_DEBUG = 0, /* Verbose developer output, gated by the -v flag. */
    LOG_INFO = 1,  /* Normal operational messages.                     */
    LOG_WARN = 2,  /* Recoverable problems that do not stop execution. */
    LOG_ERR = 3,   /* Errors the operator must act on.                 */
} log_level_t;

/*
 * Runtime verbosity flag.  Declared here so log_write() can gate LOG_DEBUG
 * output without needing a parameter.  Set to 1 by -v / verbose=true.
 */
static int g_verbose = 0;

/**
 * Write a single log line to stdout (INFO/DEBUG) or stderr (WARN/ERR).
 *
 * ANSI colour codes are kept in a small table indexed by log_level_t so
 * the mapping is obvious at a glance and trivially extended.
 *
 * @param level   Severity of the message.
 * @param msg     Null-terminated message string.
 */
static void log_write(log_level_t level, const char* msg) {
    /* Gate verbose-only output early to avoid unnecessary work. */
    if (level == LOG_DEBUG && !g_verbose) return;

    /* ANSI escape sequences indexed by log_level_t. */
    static const char* const colors[] = {
        [LOG_DEBUG] = "\033[90m", /* dark grey  */
        [LOG_INFO] = "\033[36m",  /* cyan       */
        [LOG_WARN] = "\033[33m",  /* yellow     */
        [LOG_ERR] = "\033[31m",   /* red        */
    };
    static const char* const labels[] = {
        [LOG_DEBUG] = "DEBUG",
        [LOG_INFO] = "INFO ",
        [LOG_WARN] = "WARN ",
        [LOG_ERR] = "ERROR",
    };
    static const char RESET[] = "\033[0m";

    FILE* dest = (level >= LOG_ERR) ? stderr : stdout;
    fprintf(dest, "%s[cnotify %s]%s %s\n", colors[level], labels[level], RESET, msg);
}

/* Convenience macros so call sites stay concise. */
#define LOG_DBG(msg) log_write(LOG_DEBUG, (msg))
#define LOG_INF(msg) log_write(LOG_INFO, (msg))
#define LOG_WRN(msg) log_write(LOG_WARN, (msg))
#define LOG_ERR(msg) log_write(LOG_ERR, (msg))

/** Aggregated runtime configuration populated from cnotify.conf then CLI. */
typedef struct {
    char* build_cmd;           /**< Optional build command (e.g. "go build ./...").      */
    char* pre_cmd;             /**< Long-lived background command (e.g. tailwindcss).    */
    char* run_cmd;             /**< Primary binary or script to run and reload.          */
    char* watch_path;          /**< Root directory to watch; defaults to ".".            */
    char* exclude_str;         /**< Raw comma-separated exclude list from config or CLI. */
    char* ignore_str;          /**< Comma-separated file/pattern ignore list.            */
    unsigned int pre_grace_ms; /**< Milliseconds to wait after pre_cmd before run_cmd.  */
} config_t;

/** Default exclude dirs — covers the most common noise sources out of the box. */
#define DEFAULT_EXCLUDE ".git,.idea,.vscode,tmp,vendor,bin,node_modules,dist"

/** Path of the optional configuration file loaded at startup. */
static const char* CONFIG_FILE = "cnotify.conf";

/* =========================================================================
 * Process state
 *
 * We track exactly two long-lived processes: the optional pre-command and the
 * main run command.  Each is started in its own process group so a single
 * kill(-pgid, SIG) reaches the shell and every grandchild it spawned.
 * ========================================================================= */

/** PID of the pre-command process group leader (0 when not running). */
static pid_t g_pre_pid = 0;

/** PID of the main binary process group leader (0 when not running). */
static pid_t g_run_pid = 0;

/*
 * We keep a reference to the watcher as a file-scope variable only because
 * the signal handler must reach it to request a clean shutdown. Everywhere
 * else, prefer passing it explicitly.
 *
 * The handler itself does nothing more than call cnotify_request_stop(),
 * which only performs a non-blocking write() to an internal pipe — an
 * async-signal-safe operation per POSIX. All the non-signal-safe cleanup
 * (killing children, freeing the exclude list, destroying the watcher) runs
 * afterward in main(), once cnotify_start_loop() has returned normally from
 * its own stack frame.
 */
static cnotify_t* g_cn = NULL;
static const char** g_exclude_list = NULL;
static const char** g_ignore_list = NULL;

/* =========================================================================
 * String utilities
 * ========================================================================= */

/**
 * Return the final path component of @p path without modifying it.
 *
 * Unlike POSIX basename(3), this function is re-entrant and never modifies
 * its argument.  It handles the edge cases "no slash" (returns path as-is)
 * and "trailing slash" (returns an empty string, consistent with the POSIX
 * definition — callers should not pass paths with trailing slashes).
 */
static const char* get_basename(const char* path) {
    const char* last_slash = strrchr(path, '/');
    return last_slash ? last_slash + 1 : path;
}

/**
 * Extract the bare binary name from a shell command string.
 *
 * For a command like "/usr/bin/mytool --flag arg", this returns "mytool".
 * The returned string is heap-allocated; the caller must free() it.
 *
 * @param cmd  Shell command string.  May be NULL (returns NULL).
 * @return     Heap-allocated basename, or NULL on allocation failure.
 */
static char* extract_binary_name(const char* cmd) {
    if (!cmd) return NULL;

    /*
     * strtok modifies its argument, so we work on a throwaway copy.
     * We only need the first token (the executable path).
     */
    char* tmp = strdup(cmd);
    if (!tmp) return NULL;

    char* token = strtok(tmp, " ");
    if (!token) {
        free(tmp);
        return NULL;
    }

    const char* base = get_basename(token);
    char* ret = strdup(base);
    free(tmp);
    return ret;
}

/**
 * Split @p str by @p delim and return a NULL-terminated array of
 * heap-allocated tokens with leading/trailing whitespace stripped.
 *
 * Uses two passes over a strdup'd copy so that strtok's destructive
 * tokenisation does not clobber the caller's string.
 *
 * @param str    String to split.  Must be non-NULL.
 * @param delim  Delimiter passed directly to strtok(3).
 * @return       NULL-terminated array, or NULL on any allocation failure.
 *               Caller must free each element and the array itself.
 */
static const char** split_string(char* str, const char* delim) {
    if (!str) return NULL;

    /* ---- Pass 1: count tokens so we allocate exactly the right array. ---- */
    size_t count = 0;
    char* tmp = strdup(str);
    if (!tmp) return NULL;

    for (char* t = strtok(tmp, delim); t; t = strtok(NULL, delim)) count++;
    free(tmp);

    const char** result = malloc(sizeof(char*) * (count + 1)); /* +1 for sentinel */
    if (!result) return NULL;

    /* ---- Pass 2: fill tokens, trimming whitespace from each one. */
    tmp = strdup(str);
    if (!tmp) {
        free(result);
        return NULL;
    }

    size_t i = 0;
    for (char* t = strtok(tmp, delim); t; t = strtok(NULL, delim)) {
        /* Trim leading whitespace in-place by advancing the pointer. */
        while (isspace((unsigned char)*t)) t++;

        /* Trim trailing whitespace by overwriting from the right. */
        char* end = t + strlen(t) - 1;
        while (end > t && isspace((unsigned char)*end)) *end-- = '\0';

        char* dup = strdup(t);
        if (!dup) {
            /* Partial failure: release every token allocated so far. */
            for (size_t k = 0; k < i; k++) free((void*)result[k]);
            free(result);
            free(tmp);
            return NULL;
        }
        result[i++] = dup;
    }
    result[i] = NULL; /* Sentinel so callers can iterate without a length. */

    free(tmp);
    return result;
}

/**
 * Check whether a file should be ignored based on exact paths or glob patterns.
 *
 * Matches against bare filename, path relative to watch root, or full path,
 * allowing patterns like "*.min.js", "*_test.go", or explicit paths like
 * "static/css/styles.css".
 *
 * @param fullpath    Full path to the file.
 * @param name        Bare filename.
 * @param watch_path  Watched root directory path.
 * @param patterns    NULL-terminated array of glob patterns / file paths.
 * @return            1 if the file matches an ignore rule, 0 otherwise.
 */
static int should_ignore_file(const char* fullpath, const char* name, const char* watch_path, const char** patterns) {
    if (!patterns) return 0;

    /* Compute path relative to watch_path if fullpath starts with watch_path. */
    const char* rel_path = fullpath;
    if (watch_path) {
        size_t wlen = strlen(watch_path);
        if (strncmp(fullpath, watch_path, wlen) == 0) {
            rel_path = fullpath + wlen;
            while (*rel_path == '/') rel_path++;
        }
    }

    /* Clean leading "./" from paths. */
    while (rel_path[0] == '.' && rel_path[1] == '/') rel_path += 2;

    const char* clean_path = fullpath;
    while (clean_path[0] == '.' && clean_path[1] == '/') clean_path += 2;

    for (size_t i = 0; patterns[i]; i++) {
        const char* pat = patterns[i];
        if (!pat || pat[0] == '\0') continue;

        while (pat[0] == '.' && pat[1] == '/') pat += 2;

        /* 1. Match against bare filename (e.g. *_test.go, *.min.css). */
        if (fnmatch(pat, name, 0) == 0) return 1;

        /* 2. Match against relative path within watch tree (e.g. static/css/styles.css). */
        if (fnmatch(pat, rel_path, 0) == 0 || fnmatch(pat, rel_path, FNM_PATHNAME) == 0) return 1;

        /* 3. Match against full / cleaned path. */
        if (fnmatch(pat, clean_path, 0) == 0 || fnmatch(pat, fullpath, 0) == 0) return 1;

        /* 4. Suffix match for subpath patterns (e.g. "css/styles.css" matching "static/css/styles.css"). */
        size_t plen = strlen(pat);
        size_t rlen = strlen(rel_path);
        if (rlen >= plen) {
            const char* suffix = rel_path + (rlen - plen);
            if (strcmp(suffix, pat) == 0 && (suffix == rel_path || *(suffix - 1) == '/')) {
                return 1;
            }
        }
    }

    return 0;
}

/* =========================================================================
 * Process management
 * ========================================================================= */

/**
 * Sleep for @p ms milliseconds using select(2).
 *
 * Why select() instead of usleep/nanosleep?
 *  - usleep is deprecated in POSIX.1-2008.
 *  - nanosleep can be interrupted by signals and requires a restart loop.
 *  - select() with all fd sets NULL and only a timeout is perfectly legal,
 *    signal-safe in the ways that matter here, and available everywhere.
 *  - SIGALRM does not interact with select(), unlike with sleep(3).
 *
 * This is a plain fixed-duration sleep with no fd to wait on, so it is left
 * as select() rather than poll(): poll(NULL, 0, ms) would be an equally
 * valid one-line substitute, but there is no readiness/signal-race concern
 * here for select() to lose to — unlike cnotify_start_loop()'s multiplexed
 * wait, which now uses poll() specifically to watch two descriptors and a
 * shared timeout at once.
 */
static void sleep_ms(unsigned int ms) {
    if (ms == 0) return;
    struct timeval tv = {
        .tv_sec = (time_t)(ms / 1000),
        .tv_usec = (suseconds_t)((ms % 1000) * 1000),
    };
    select(0, NULL, NULL, NULL, &tv);
}

/**
 * Send SIGTERM to process group @p *pid_ptr and reap all its members.
 *
 * Why kill by process group rather than PID?
 * ─────────────────────────────────────────
 * We launch commands via `/bin/sh -c <cmd>`, which means the shell is the
 * direct child and the actual binary is a grandchild.  If we only sent a
 * signal to the shell's PID, the grandchild would be orphaned and continue
 * running.  Instead, start_process() calls setpgid(0,0) in the child so
 * that both shell and grandchild share a process group whose ID equals the
 * shell's PID.  Sending to -pgid delivers the signal to every member.
 *
 * Why the two-phase SIGTERM → SIGKILL approach?
 * ─────────────────────────────────────────────
 * A well-behaved process (e.g. a web server) should catch SIGTERM, finish
 * in-flight requests, and exit cleanly.  We therefore give it TERM_TIMEOUT_MS
 * to do so before resorting to SIGKILL, which cannot be caught or ignored.
 *
 * Why loop on waitpid(-pgid, WNOHANG)?
 * ─────────────────────────────────────
 * A process group can contain more than two processes (shell + one binary).
 * For example, the binary might fork workers.  A single waitpid call only
 * reaps one child at a time.  We loop until ECHILD (no members left) or we
 * exhaust our patience and escalate.
 *
 * @param pid_ptr  In/out: PID of the process group leader.  Set to 0 on exit.
 */
static void kill_process(pid_t* pid_ptr) {
    pid_t pgid = *pid_ptr;
    if (pgid <= 0) return;

    char buf[64];
    snprintf(buf, sizeof(buf), "Killing process group %d", (int)pgid);
    LOG_DBG(buf);

    kill(-pgid, SIGTERM);

    const int poll_interval_us = 100000; /* 100 ms between polls */
    const int max_attempts = TERM_TIMEOUT_MS / (poll_interval_us / 1000);
    int attempts = 0;

    while (attempts < max_attempts) {
        pid_t result = waitpid(-pgid, NULL, WNOHANG);

        if (result > 0) {
            /*
             * One member exited.  Don't increment attempts — keep draining
             * without sleeping so we reap siblings as fast as possible.
             */
            continue;
        }
        if (result == -1) {
            if (errno == EINTR) continue; /* Interrupted by a signal; retry. */
            if (errno == ECHILD) {
                /* No children left in the group — we're done. */
                snprintf(buf, sizeof(buf), "Process group %d fully exited", (int)pgid);
                LOG_DBG(buf);
                *pid_ptr = 0;
                return;
            }
            /* Unexpected error: break and escalate to SIGKILL. */
            break;
        }

        /* result == 0: group still has live members.  Sleep and retry. */
        usleep((useconds_t)poll_interval_us);
        attempts++;
    }

    /* Patience exhausted — force-kill anything still alive. */
    snprintf(buf, sizeof(buf), "Process group %d timed out; sending SIGKILL", (int)pgid);
    LOG_WRN(buf);
    kill(-pgid, SIGKILL);

    /*
     * Drain zombies after SIGKILL.  SIGKILL is not deferrable, so this
     * loop should complete almost immediately.
     */
    while (waitpid(-pgid, NULL, WNOHANG) > 0);

    *pid_ptr = 0;
}

/**
 * Stop both managed processes in startup order (pre first, then run).
 *
 * Killing pre_cmd before run_cmd mirrors the startup sequence and avoids
 * a race where the dying run_cmd triggers pre_cmd cleanup prematurely.
 */
static void kill_all_children(void) {
    kill_process(&g_pre_pid);
    kill_process(&g_run_pid);
}

/**
 * Fork and exec @p cmd via /bin/sh -c in a new, isolated process group.
 *
 * Placing the child in its own process group (setpgid(0,0)) is the
 * foundation of the "kill the whole tree" strategy — see kill_process().
 * The group ID equals the child's PID, which is stored in *pid_out.
 *
 * @param cmd      Shell command string to execute.
 * @param pid_out  Receives the child's PID on success; unchanged on failure.
 */
static void start_process(const char* cmd, pid_t* pid_out) {
    if (!cmd) return;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        /*
         * Child path.
         *
         * setpgid(0, 0) moves this process into a new process group before
         * exec, so that kill(-pgid, SIG) in the parent reaches the shell
         * AND the binary it spawns.  This call must happen before execv;
         * once exec succeeds there is no way to set the process group.
         */
        setpgid(0, 0);
        char* args[] = {"/bin/sh", "-c", (char*)cmd, NULL};
        execv(args[0], args);
        /* execv only returns on failure. */
        perror("execv");
        _exit(1);
    }

    /* Parent path: record the group leader PID. */
    *pid_out = pid;
    char buf[256];
    snprintf(buf, sizeof(buf), "Started PID %d: %s", (int)pid, cmd);
    LOG_DBG(buf);
}

/**
 * Launch pre_cmd (if configured) and then run_cmd.
 *
 * The optional grace period lets pre_cmd complete its first-run work
 * (e.g. generate a CSS bundle) before the server tries to serve files.
 *
 * @param cfg  Runtime configuration.
 */
static void launch_all(const config_t* cfg) {
    if (cfg->pre_cmd) {
        start_process(cfg->pre_cmd, &g_pre_pid);

        if (cfg->pre_grace_ms > 0) {
            char buf[80];
            snprintf(buf, sizeof(buf), "Waiting %ums for pre-command to initialise", cfg->pre_grace_ms);
            LOG_DBG(buf);
            sleep_ms(cfg->pre_grace_ms);
        }
    }

    start_process(cfg->run_cmd, &g_run_pid);
}

/**
 * Tear down running processes, optionally rebuild, then restart.
 *
 * If the build step fails we log and return early rather than launching a
 * potentially broken binary — the user will fix the error and save again,
 * triggering another reload attempt.
 *
 * @param cfg  Runtime configuration.
 */
static void restart_app(const config_t* cfg) {
    LOG_INF("Change detected. Reloading...");
    kill_all_children();

    if (cfg->build_cmd) {
        int ret = system(cfg->build_cmd);
        if (ret != 0) {
            LOG_ERR("Build failed. Waiting for changes...");
            return;
        }
    }

    launch_all(cfg);
}

/* =========================================================================
 * inotify event callback
 * ========================================================================= */

/**
 * Invoked by the cnotify event loop for every filesystem event.
 *
 * We ignore directory events (we watch files, not directories themselves)
 * and delegate changed/deleted/moved logic to restart_app().
 *
 * @param event      Filesystem event descriptor.
 * @param user_data  Pointer to the owning cnotify_t; cast accordingly.
 * @return           0 to continue the loop.
 */
static int on_change(const cnotify_event_t* event, void* user_data) {
    /*
     * user_data carries three things bundled into one pointer: the cnotify
     * handle, the config, and the ignore patterns list. We use a small
     * context struct to pass all cleanly rather than relying on additional
     * file-scope globals.
     */
    typedef struct {
        cnotify_t* cn;
        const config_t* cfg;
        const char** ignore_list;
    } ctx_t;
    ctx_t* ctx = (ctx_t*)user_data;

    /* Directory creation/deletion events are not actionable here. */
    if (event->is_dir) return 0;

    char fullpath[PATH_MAX];
    int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", event->path, event->name);
    if (n < 0 || (size_t)n >= sizeof(fullpath)) {
        LOG_WRN("Path too long; skipping event");
        return 0;
    }

    /* Check if the file matches any ignore rules (e.g. *.min.css, static/css/styles.css). */
    if (should_ignore_file(fullpath, event->name, ctx->cfg->watch_path, ctx->ignore_list)) {
        char buf[PATH_MAX + 64];
        snprintf(buf, sizeof(buf), "Ignored change on %s (matched ignore pattern)", fullpath);
        LOG_DBG(buf);
        return 0;
    }

    if (event->type == CNOTIFY_EVENT_DELETE) {
        /*
         * File removed: update the watcher's internal checksum table so a
         * future file of the same name is treated as new, not unchanged.
         */
        if (cnotify_file_remove(ctx->cn, fullpath)) restart_app(ctx->cfg);
        return 0;
    }

    if (event->type == CNOTIFY_EVENT_MOVE) {
        /*
         * Move events can be renames-into-tree (move_to) or
         * renames-out-of-tree (move_from).  Distinguish by checking
         * whether the destination path now exists.
         */
        if (access(fullpath, F_OK) == 0) {
            if (cnotify_file_changed(ctx->cn, fullpath)) restart_app(ctx->cfg);
        } else {
            cnotify_file_remove(ctx->cn, fullpath);
        }
        return 0;
    }

    /* CNOTIFY_EVENT_CREATE / CNOTIFY_EVENT_MODIFY and any future types. */
    if (cnotify_file_changed(ctx->cn, fullpath)) restart_app(ctx->cfg);

    return 0;
}

/* =========================================================================
 * Signal handling
 * ========================================================================= */

/**
 * Handler for SIGINT and SIGTERM.
 *
 * This does the absolute minimum permitted in an async signal handler:
 * request that the event loop stop, via a single non-blocking write() to
 * cnotify's internal self-pipe (async-signal-safe per POSIX). It does NOT
 * call printf(), free(), or exit() — all of which are unsafe to invoke from
 * a signal handler and were previously called here. cnotify_start_loop()
 * observes the pipe becoming readable via poll(), returns 0 from its own
 * stack frame, and main() performs the actual shutdown sequence (killing
 * children, freeing the exclude list, destroying the watcher) as ordinary
 * post-loop code.
 */
static void handle_sigint(int sig) {
    (void)sig; /* We treat SIGINT and SIGTERM identically. */
    cnotify_request_stop(g_cn);
}

/* =========================================================================
 * Initialisation helpers
 * ========================================================================= */

/** Print command-line usage to stdout. */
static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -build <cmd>      Command to build the project (optional)\n");
    printf("  -pre   <cmd>      Long-lived background command (e.g. tailwindcss --watch)\n");
    printf(
        "  -pre-grace <ms>   Grace period after pre-cmd before launching -bin "
        "(default: %u ms)\n",
        DEFAULT_PRE_GRACE_MS);
    printf("  -bin   <cmd>      Command to run the binary/script (required)\n");
    printf("  -path  <dir>      Directory to watch (default: .)\n");
    printf("  -exclude <list>   Comma-separated directories to exclude (appended\n");
    printf("                    to default: %s)\n", DEFAULT_EXCLUDE);
    printf("  -ignore  <list>   Comma-separated files or patterns to ignore\n");
    printf("                    (e.g. \"static/css/styles.css, *_test.go, *.min.css\")\n");
    printf("  -v                Verbose / debug output\n");
    printf("  -h                Show this help and exit\n");
}

/**
 * Parse @p filename into @p cfg.
 *
 * Config file values act as defaults and are silently overridden by CLI
 * flags parsed afterward.  An absent config file is not an error.
 *
 * Format: one "key = value" pair per line; '#' introduces a comment.
 *
 * @param filename  Path to the configuration file.
 * @param cfg       Configuration struct to populate.
 */
static void parse_config_file(const char* filename, config_t* cfg) {
    FILE* f = fopen(filename, "r");
    if (!f) return; /* Missing config file is not an error. */

    char buf[64];
    snprintf(buf, sizeof(buf), "Loading config from %s", filename);
    LOG_DBG(buf);

    char line[1024];
    while (fgets(line, (int)sizeof(line), f)) {
        /* Strip trailing newline. */
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        /* Skip blank lines and comments. */
        if (line[0] == '#' || line[0] == '\0') continue;

        /* Require "key = value" format; skip malformed lines silently. */
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';

        char* key = line;
        char* val = eq + 1;

        /* Trim whitespace from both key and value. */
        while (isspace((unsigned char)*key)) key++;
        char* end = key + strlen(key) - 1;
        while (end > key && isspace((unsigned char)*end)) *end-- = '\0';

        while (isspace((unsigned char)*val)) val++;
        end = val + strlen(val) - 1;
        while (end > val && isspace((unsigned char)*end)) *end-- = '\0';

        /*
         * Config file provides the baseline; only write if the field is not
         * already set (allows CLI flags parsed later to win without special
         * casing every field).
         */
        if (strcmp(key, "build") == 0) {
            if (!cfg->build_cmd) cfg->build_cmd = strdup(val);
        } else if (strcmp(key, "pre") == 0) {
            if (!cfg->pre_cmd) cfg->pre_cmd = strdup(val);
        } else if (strcmp(key, "pre_grace") == 0) {
            cfg->pre_grace_ms = (unsigned int)atoi(val);
        } else if (strcmp(key, "bin") == 0) {
            if (!cfg->run_cmd) cfg->run_cmd = strdup(val);
        } else if (strcmp(key, "path") == 0) {
            if (!cfg->watch_path) cfg->watch_path = strdup(val);
        } else if (strcmp(key, "exclude") == 0) {
            if (!cfg->exclude_str) cfg->exclude_str = strdup(val);
        } else if (strcmp(key, "ignore") == 0 || strcmp(key, "ignore_files") == 0 ||
                   strcmp(key, "ignore_patterns") == 0) {
            if (!cfg->ignore_str) cfg->ignore_str = strdup(val);
        } else if (strcmp(key, "verbose") == 0) {
            if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) g_verbose = 1;
        }
    }
    fclose(f);
}

/**
 * Parse argv into @p cfg.
 *
 * CLI flags override anything loaded from the config file.  Unknown flags
 * are silently ignored to allow forward-compatibility with new flags added
 * later.
 *
 * @param argc  Argument count from main.
 * @param argv  Argument vector from main.
 * @param cfg   Configuration struct to populate.
 * @return      0 on success, 1 if -h was given (caller should exit cleanly).
 */
static int parse_args(int argc, char* argv[], config_t* cfg) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-build") == 0 && i + 1 < argc) {
            cfg->build_cmd = argv[++i];
        } else if (strcmp(argv[i], "-pre") == 0 && i + 1 < argc) {
            cfg->pre_cmd = argv[++i];
        } else if (strcmp(argv[i], "-pre-grace") == 0 && i + 1 < argc) {
            cfg->pre_grace_ms = (unsigned int)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-bin") == 0 && i + 1 < argc) {
            cfg->run_cmd = argv[++i];
        } else if (strcmp(argv[i], "-path") == 0 && i + 1 < argc) {
            cfg->watch_path = argv[++i];
        } else if (strcmp(argv[i], "-exclude") == 0 && i + 1 < argc) {
            cfg->exclude_str = argv[++i];
        } else if ((strcmp(argv[i], "-ignore") == 0 || strcmp(argv[i], "-ignore-patterns") == 0) && i + 1 < argc) {
            cfg->ignore_str = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 1;
        }
    }
    return 0;
}

/**
 * Register signal handlers for orderly shutdown.
 *
 * We treat SIGINT (Ctrl-C) and SIGTERM (process manager stop) identically:
 * request that cnotify_start_loop() stop, and let main() perform the
 * actual teardown once control returns to it normally.
 */
static void setup_signals(void) {
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
}

/**
 * Build the exclude list and initialise the cnotify file watcher.
 *
 * The exclude list is built by combining DEFAULT_EXCLUDE with any user-provided
 * exclude directories, and is further extended with the run_cmd binary name so
 * that writing a newly compiled binary into the watch tree does not trigger a
 * spurious reload cycle.
 *
 * @param cfg       Runtime configuration.
 * @param cn_out    Receives the initialised cnotify handle.
 * @param excl_out  Receives the NULL-terminated exclude array.
 * @return          0 on success, non-zero on fatal error.
 */
static int setup_watcher(const config_t* cfg, cnotify_t** cn_out, const char*** excl_out) {
    cnotify_t* cn = cnotify_init();
    if (!cn) {
        LOG_ERR("Failed to initialise cnotify watcher");
        return 1;
    }

    /*
     * Build the initial exclude list by combining the default excludes with
     * any user-provided excludes, ensuring default noisy directories are always
     * ignored.
     *
     * split_string needs a mutable char* because strtok modifies the
     * string in-place. We pass a heap-allocated copy so cfg is left intact.
     */
    char* excl_copy = NULL;
    if (cfg->exclude_str && cfg->exclude_str[0] != '\0') {
        size_t len = strlen(DEFAULT_EXCLUDE) + 1 + strlen(cfg->exclude_str) + 1;
        excl_copy = malloc(len);
        if (excl_copy) {
            snprintf(excl_copy, len, "%s,%s", DEFAULT_EXCLUDE, cfg->exclude_str);
        }
    } else {
        excl_copy = strdup(DEFAULT_EXCLUDE);
    }

    if (!excl_copy) {
        LOG_ERR("Failed to allocate memory for exclude list copy");
        cnotify_destroy(cn);
        return 1;
    }

    const char** excl = split_string(excl_copy, ",");
    free(excl_copy);

    if (!excl) {
        LOG_ERR("Failed to split exclude list");
        cnotify_destroy(cn);
        return 1;
    }

    /*
     * Append the compiled binary name to the exclude list so that writing
     * the freshly built binary into the watched tree does not trigger an
     * infinite reload loop.
     *
     * Pattern:
     *   count existing entries → realloc for +2 slots (name + sentinel)
     *   → fill → update sentinel.
     */
    char* binary_name = extract_binary_name(cfg->run_cmd);
    if (binary_name) {
        size_t count = 0;
        while (excl[count]) count++;

        const char** extended = realloc((void*)excl, sizeof(char*) * (count + 2));
        if (extended) {
            excl = extended;
            excl[count] = binary_name;
            excl[count + 1] = NULL;
        } else {
            /* Non-fatal: the watcher still works; it just may self-trigger. */
            LOG_WRN("Could not extend exclude list with binary name");
            free(binary_name);
        }
    }

    if (cnotify_add_watch(cn, cfg->watch_path, excl) < 0)
        LOG_WRN("Failed to add watch (watcher will not fire, but binary still runs)");

    *cn_out = cn;
    *excl_out = excl;
    return 0;
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

int main(int argc, char* argv[]) {
    /* Establish defaults. */
    config_t cfg = {
        .build_cmd = NULL,
        .pre_cmd = NULL,
        .run_cmd = NULL,
        .watch_path = NULL,  /* NULL means "." — resolved in setup_watcher */
        .exclude_str = NULL, /* Appended to DEFAULT_EXCLUDE */
        .ignore_str = NULL,  /* NULL means no extra files/patterns ignored */
        .pre_grace_ms = DEFAULT_PRE_GRACE_MS,
    };

    /* Load config file first; CLI flags override below. */
    parse_config_file(CONFIG_FILE, &cfg);

    /* Parse CLI; g_verbose may be set here too. */
    if (parse_args(argc, argv, &cfg) != 0) return 0; /* -h was given; usage already printed. */

    /* watch_path falls back to "." if unset by both config and CLI. */
    if (!cfg.watch_path) cfg.watch_path = ".";

    /* Validate required arguments. */
    if (!cfg.run_cmd) {
        LOG_ERR("-bin is required");
        print_usage(argv[0]);
        return 1;
    }

    /* Parse file ignore patterns if specified. */
    if (cfg.ignore_str) {
        char* ign_copy = strdup(cfg.ignore_str);
        if (ign_copy) {
            g_ignore_list = split_string(ign_copy, ",");
            free(ign_copy);
        }
    }

    /* Initialise filesystem watcher first: signal handlers reference g_cn,
     * so it must be valid (or safely NULL, per cnotify_request_stop's NULL
     * check) before we can wire up signal handling. */
    if (setup_watcher(&cfg, &g_cn, &g_exclude_list) != 0) return 1;

    /* Wire up signal handlers before forking anything. */
    setup_signals();

    /* Initial build (synchronous) — failure is non-fatal. */
    if (cfg.build_cmd) {
        if (system(cfg.build_cmd) != 0) LOG_WRN("Initial build failed; launching binary and waiting for changes...");
    }

    /* Start managed processes. */
    launch_all(&cfg);

    LOG_INF("Watching for changes...");

    /*
     * Bundle the cnotify handle, config, and ignore list into a single context
     * object so on_change() receives everything through the void* user_data
     * parameter — avoiding additional file-scope globals.
     */
    typedef struct {
        cnotify_t* cn;
        const config_t* cfg;
        const char** ignore_list;
    } ctx_t;
    ctx_t ctx = {.cn = g_cn, .cfg = &cfg, .ignore_list = g_ignore_list};

    /*
     * Block in the event loop until a callback requests stop or a signal
     * handler calls cnotify_request_stop() (SIGINT/SIGTERM). Either way,
     * this returns normally rather than the process being torn down mid
     * signal-handler, so everything below always runs.
     */
    if (cnotify_start_loop(g_cn, on_change, &ctx) < 0) LOG_ERR("Event loop exited due to an error");

    LOG_INF("Stopping...");
    kill_all_children();

    if (g_exclude_list) {
        for (size_t i = 0; g_exclude_list[i]; i++) free((void*)g_exclude_list[i]);
        free(g_exclude_list);
        g_exclude_list = NULL;
    }

    if (g_ignore_list) {
        for (size_t i = 0; g_ignore_list[i]; i++) free((void*)g_ignore_list[i]);
        free(g_ignore_list);
        g_ignore_list = NULL;
    }

    cnotify_destroy(g_cn);
    g_cn = NULL;

    return 0;
}
