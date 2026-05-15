#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>   /* for select() used as portable sleep */
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../include/cnotify.h"

#define MAX_ARGS             64
#define TERM_TIMEOUT_MS      1000  /* Wait 1 second before SIGKILL */
#define DEFAULT_PRE_GRACE_MS 150   /* Head-start for pre-cmd before main binary */

/* ========================================================================
 * Configuration (populated from cnotify.conf then CLI flags)
 * ======================================================================== */

static char*        build_cmd    = NULL;
static char*        pre_cmd      = NULL;  /* Background command, e.g. tailwindcss --watch */
static char*        run_cmd      = NULL;  /* Primary binary/script to run */
static char*        watch_path   = ".";
static char*        exclude_str  = ".git,.idea,.vscode,tmp,vendor,bin,node_modules,dist";
static const char** exclude_list = NULL;
static int          verbose      = 0;
static unsigned int pre_grace_ms = DEFAULT_PRE_GRACE_MS;

/* ========================================================================
 * Process state
 * ======================================================================== */

static pid_t pre_pid = 0;  /* PID of the pre-command process group leader */
static pid_t run_pid = 0;  /* PID of the main binary process group leader */

/* ========================================================================
 * String utilities
 * ======================================================================== */

static const char* get_basename(const char* path) {
    const char* last_slash = strrchr(path, '/');
    return last_slash ? last_slash + 1 : path;
}

static char* extract_binary_name(const char* cmd) {
    if (!cmd) return NULL;

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

static const char** split_string(char* str, const char* delim) {
    if (!str) return NULL;

    /* First pass: count tokens */
    size_t count = 0;
    char* tmp = strdup(str);
    if (!tmp) return NULL;

    char* token = strtok(tmp, delim);
    while (token) {
        count++;
        token = strtok(NULL, delim);
    }
    free(tmp);

    const char** result = malloc(sizeof(char*) * (count + 1));
    if (!result) return NULL;

    /* Second pass: fill tokens */
    tmp = strdup(str);
    if (!tmp) {
        free(result);
        return NULL;
    }

    token = strtok(tmp, delim);
    size_t i = 0;
    while (token) {
        /* Trim leading whitespace */
        while (isspace((unsigned char)*token)) token++;
        char* end = token + strlen(token) - 1;
        while (end > token && isspace((unsigned char)*end)) *end-- = '\0';

        char* dup = strdup(token);
        if (!dup) {
            for (size_t k = 0; k < i; k++) free((void*)result[k]);
            free(result);
            free(tmp);
            return NULL;
        }
        result[i++] = dup;
        token = strtok(NULL, delim);
    }
    result[i] = NULL;
    free(tmp);
    return result;
}

/* ========================================================================
 * Logging
 * ======================================================================== */

static void log_info(const char* msg) { printf("\033[36m[cnotify]\033[0m %s\n", msg); }
static void log_err(const char* msg)  { fprintf(stderr, "\033[31m[cnotify] Error:\033[0m %s\n", msg); }

/* ========================================================================
 * Process management
 * ======================================================================== */

/**
 * Kill the process group rooted at *pid_ptr and reap all its members.
 *
 * Because we launch commands via `/bin/sh -c`, the shell spawns the real
 * binary as a grandchild.  Both shell and grandchild share a process group
 * whose ID equals the shell's PID (set via setpgid(0,0) in the child).
 * Using waitpid(-pgid, ...) harvests every process in that group, preventing
 * zombie grandchildren.
 */
static void kill_process(pid_t* pid_ptr) {
    pid_t pgid = *pid_ptr;
    if (pgid <= 0) return;

    if (verbose) printf("Killing process group %d\n", (int)pgid);
    kill(-pgid, SIGTERM);

    int status;
    int attempts       = 0;
    const int max_att  = TERM_TIMEOUT_MS / 100;

    while (attempts < max_att) {
        /*
         * Drain the whole process group: loop while waitpid returns children.
         * Stop when ECHILD (no more members) or no child exited yet (result==0).
         */
        pid_t result = waitpid(-pgid, &status, WNOHANG);
        if (result > 0) {
            /* A member exited — keep draining without sleeping. */
            continue;
        }
        if (result == -1) {
            if (errno == EINTR)  continue;
            if (errno == ECHILD) {
                /* All members gone. */
                if (verbose) printf("Process group %d fully exited\n", (int)pgid);
                *pid_ptr = 0;
                return;
            }
        }
        /* result == 0: group still running, wait and retry. */
        usleep(100000);
        attempts++;
    }

    if (verbose) printf("Process group %d didn't exit, sending SIGKILL\n", (int)pgid);
    kill(-pgid, SIGKILL);

    /* Drain any remaining zombies after SIGKILL. */
    while (waitpid(-pgid, &status, WNOHANG) > 0);

    *pid_ptr = 0;
}

static void kill_all_children(void) {
    /*
     * Kill pre_cmd first so it does not race with the dying run_cmd.
     * For a watcher like tailwindcss this order does not matter much,
     * but it mirrors the startup order and is easier to reason about.
     */
    kill_process(&pre_pid);
    kill_process(&run_pid);
}

/**
 * Fork and exec cmd via /bin/sh -c in a new process group.
 * The new PGID equals the child's PID, stored in *pid_out.
 */
static void start_process(const char* cmd, pid_t* pid_out) {
    if (!cmd) return;

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        /* Child: new process group so kill(-pgid) reaches all descendants. */
        setpgid(0, 0);
        char* args[] = {"/bin/sh", "-c", (char*)cmd, NULL};
        execv(args[0], args);
        perror("execv");
        _exit(1);
    }

    *pid_out = pid;
    if (verbose) printf("Started process %d: %s\n", (int)pid, cmd);
}

/**
 * Sleep for ms milliseconds using select(2) as a portable, signal-safe
 * alternative to usleep/nanosleep that does not interact with SIGALRM.
 */
static void sleep_ms(unsigned int ms) {
    if (ms == 0) return;
    struct timeval tv = {
        .tv_sec  = (time_t)(ms / 1000),
        .tv_usec = (suseconds_t)((ms % 1000) * 1000),
    };
    select(0, NULL, NULL, NULL, &tv);
}

/**
 * Launch the pre-command (if configured) followed by the main binary.
 *
 * pre_cmd is a long-lived background process (e.g. `tailwindcss --watch`).
 * We give it a grace period — DEFAULT_PRE_GRACE_MS by default, overridden
 * with -pre-grace — before starting run_cmd so it can write its initial
 * output (CSS bundle, generated files, etc.) before the server starts.
 */
static void launch_all(void) {
    if (pre_cmd) {
        log_info("Starting pre-command...");
        start_process(pre_cmd, &pre_pid);
        if (pre_grace_ms > 0) {
            if (verbose)
                printf("Waiting %ums for pre-command to initialise\n", pre_grace_ms);
            sleep_ms(pre_grace_ms);
        }
    }
    log_info("Starting app...");
    start_process(run_cmd, &run_pid);
}

static void restart_app(void) {
    log_info("Change detected. Reloading...");
    kill_all_children();

    if (build_cmd) {
        log_info("Building...");
        int ret = system(build_cmd);
        if (ret != 0) {
            log_err("Build failed. Waiting for changes...");
            return;
        }
    }

    launch_all();
}

/* ========================================================================
 * inotify event callback
 * ======================================================================== */

static int on_change(const cnotify_event_t* event, void* user_data) {
    cnotify_t* cn = (cnotify_t*)user_data;

    if (event->is_dir) return 0;

    char fullpath[PATH_MAX];
    int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", event->path, event->name);
    if (n < 0 || (size_t)n >= sizeof(fullpath)) return 0;

    if (event->type == CNOTIFY_EVENT_DELETE) {
        if (cnotify_file_remove(cn, fullpath)) {
            restart_app();
        }
        return 0;
    }

    if (event->type == CNOTIFY_EVENT_MOVE) {
        if (access(fullpath, F_OK) == 0) {
            if (cnotify_file_changed(cn, fullpath)) {
                restart_app();
            }
        } else {
            cnotify_file_remove(cn, fullpath);
        }
        return 0;
    }

    if (cnotify_file_changed(cn, fullpath)) {
        restart_app();
    }
    return 0;
}

/* ========================================================================
 * Signal handling
 * ======================================================================== */

static void handle_sigint(int sig) {
    (void)sig;
    printf("\n");
    log_info("Stopping...");
    kill_all_children();

    if (exclude_list) {
        for (int i = 0; exclude_list[i]; i++) free((void*)exclude_list[i]);
        free(exclude_list);
    }

    exit(0);
}

/* ========================================================================
 * Configuration: usage, config file, CLI
 * ======================================================================== */

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -build <cmd>      Command to build the project (optional)\n");
    printf("  -pre <cmd>        Command to run before the binary (e.g. tailwindcss --watch)\n");
    printf("  -pre-grace <ms>   Grace period after pre-cmd before starting -bin (default: %u)\n",
           DEFAULT_PRE_GRACE_MS);
    printf("  -bin <cmd>        Command to run the binary/script (required)\n");
    printf("  -path <dir>       Directory to watch (default: .)\n");
    printf("  -exclude <list>   Comma-separated directories to exclude\n");
    printf("                    (default: .git,.idea,.vscode,tmp,vendor,bin)\n");
    printf("  -v                Verbose output\n");
    printf("  -h                Show this help\n");
}

static void parse_config_file(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) return;
    if (verbose) printf("Loading config from %s\n", filename);

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char* p = strchr(line, '\n');
        if (p) *p = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';

        char* key = line;
        char* val = eq + 1;

        while (isspace((unsigned char)*key)) key++;
        char* end = key + strlen(key) - 1;
        while (end > key && isspace((unsigned char)*end)) *end-- = '\0';

        while (isspace((unsigned char)*val)) val++;
        end = val + strlen(val) - 1;
        while (end > val && isspace((unsigned char)*end)) *end-- = '\0';

        if (strcmp(key, "build") == 0) {
            if (!build_cmd)                         build_cmd    = strdup(val);
        } else if (strcmp(key, "pre") == 0) {
            if (!pre_cmd)                           pre_cmd      = strdup(val);
        } else if (strcmp(key, "pre_grace") == 0) {
            pre_grace_ms = (unsigned int)atoi(val);
        } else if (strcmp(key, "bin") == 0) {
            if (!run_cmd)                           run_cmd      = strdup(val);
        } else if (strcmp(key, "path") == 0) {
            if (strcmp(watch_path, ".") == 0)       watch_path   = strdup(val);
        } else if (strcmp(key, "exclude") == 0) {
            if (strncmp(exclude_str, ".git", 4) == 0) exclude_str = strdup(val);
        } else if (strcmp(key, "verbose") == 0) {
            if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) verbose = 1;
        }
    }
    fclose(f);
}

/* ========================================================================
 * Entry point
 * ======================================================================== */

int main(int argc, char* argv[]) {
    parse_config_file("cnotify.conf");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-build") == 0 && i + 1 < argc) {
            build_cmd = argv[++i];
        } else if (strcmp(argv[i], "-pre") == 0 && i + 1 < argc) {
            pre_cmd = argv[++i];
        } else if (strcmp(argv[i], "-pre-grace") == 0 && i + 1 < argc) {
            pre_grace_ms = (unsigned int)atoi(argv[++i]);
        } else if (strcmp(argv[i], "-bin") == 0 && i + 1 < argc) {
            run_cmd = argv[++i];
        } else if (strcmp(argv[i], "-path") == 0 && i + 1 < argc) {
            watch_path = argv[++i];
        } else if (strcmp(argv[i], "-exclude") == 0 && i + 1 < argc) {
            exclude_str = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (!run_cmd) {
        log_err("-bin argument is required");
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT,  handle_sigint);
    signal(SIGTERM, handle_sigint);

    /* Initial build (synchronous), then start pre-cmd + main binary. */
    if (build_cmd) {
        log_info("Initial build...");
        if (system(build_cmd) != 0) {
            log_err("Initial build failed. Still starting watcher...");
            /* Start the app anyway so the watcher loop runs. */
        }
    }
    launch_all();

    /* Initialise watcher. */
    cnotify_t* cn = cnotify_init();
    if (!cn) {
        log_err("Failed to initialize watcher");
        kill_all_children();
        return 1;
    }

    exclude_list = split_string(exclude_str, ",");
    if (!exclude_list && exclude_str) {
        log_err("Failed to allocate memory for exclude list");
        cnotify_destroy(cn);
        kill_all_children();
        return 1;
    }

    /*
     * Auto-exclude the binary itself so that writing the compiled output
     * into the watch tree does not trigger a second reload cycle.
     */
    char* binary_name = extract_binary_name(run_cmd);
    if (binary_name) {
        if (verbose) printf("Auto-excluding binary: %s\n", binary_name);

        size_t count = 0;
        if (exclude_list) while (exclude_list[count]) count++;

        const char** new_list = realloc((void*)exclude_list, sizeof(char*) * (count + 2));
        if (new_list) {
            exclude_list = new_list;
            exclude_list[count]     = binary_name;
            exclude_list[count + 1] = NULL;
        } else {
            free(binary_name); /* Non-fatal — watcher still runs. */
        }
    }

    log_info("Watching for changes...");
    if (cnotify_add_watch(cn, watch_path, exclude_list) < 0) {
        log_err("Failed to add watch");
        /* Non-fatal: continue so the binary still runs even if watching fails. */
    }

    cnotify_start_loop(cn, on_change, cn);
    cnotify_destroy(cn);

    if (exclude_list) {
        for (int i = 0; exclude_list[i]; i++) free((void*)exclude_list[i]);
        free(exclude_list);
    }

    return 0;
}
