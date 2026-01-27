#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../include/cnotify.h"

#define MAX_ARGS 64

// Configuration
static char* build_cmd = NULL;
static char* run_cmd = NULL;  // The binary to run
static char* watch_path = ".";
static char* exclude_str = ".git,.idea,.vscode,tmp,vendor,bin";
static const char** exclude_list = NULL;
static int verbose = 0;

// State
static pid_t child_pid = 0;

// Helper to split string by comma
static const char** split_string(char* str, const char* delim) {
    if (!str) return NULL;

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

    tmp = strdup(str);
    if (!tmp) {
        free(result);
        return NULL;
    }

    token = strtok(tmp, delim);
    int i = 0;
    while (token) {
        // Trim spaces
        while (isspace(*token)) token++;
        char* end = token + strlen(token) - 1;
        while (end > token && isspace(*end)) *end-- = '\0';

        char* dup = strdup(token);
        if (!dup) {
            // Allocation failed, cleanup everything
            for (int k = 0; k < i; k++) {
                free((void*)result[k]);
            }
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

static void log_info(const char* msg) { printf("\033[36m[cnotify]\033[0m %s\n", msg); }

static void log_err(const char* msg) { fprintf(stderr, "\033[31m[cnotify] Error:\033[0m %s\n", msg); }

static void kill_child() {
    if (child_pid > 0) {
        if (verbose) printf("Killing process group %d\n", child_pid);
        // Kill the process group
        kill(-child_pid, SIGTERM);

        // Wait a bit, then force kill if necessary?
        // For speed, we just waitpid.
        int status;
        waitpid(child_pid, &status, 0);
        child_pid = 0;
    }
}

static void start_process(const char* cmd) {
    if (!cmd) return;

    // Use sh -c to execute command so arguments are handled
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        // Child
        // Create new process group so we can kill it and its children
        setpgid(0, 0);

        char* args[] = {"/bin/sh", "-c", (char*)cmd, NULL};
        execv(args[0], args);
        perror("execv");
        exit(1);
    } else {
        // Parent
        child_pid = pid;
        if (verbose) printf("Started process %d: %s\n", pid, cmd);
    }
}

static void restart_app() {
    log_info("Change detected. Reloading...");
    kill_child();

    if (build_cmd) {
        log_info("Building...");
        int ret = system(build_cmd);
        if (ret != 0) {
            log_err("Build failed. Waiting for changes...");
            return;
        }
    }

    log_info("Restarting app...");
    start_process(run_cmd);
}

static int on_change(CNotifyEvent* event, void* user_data) {
    (void)user_data;
    if (verbose) printf("Event: %s/%s\n", event->path, event->filename);
    restart_app();
    return 0;  // Continue loop
}

static void handle_sigint(int sig) {
    (void)sig;
    printf("\n");
    log_info("Stopping...");
    kill_child();

    // Cleanup exclude list memory
    if (exclude_list) {
        for (int i = 0; exclude_list[i]; i++) {
            free((void*)exclude_list[i]);
        }
        free(exclude_list);
    }

    exit(0);
}

void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -build <cmd>    Command to build the project (optional)\n");
    printf("  -bin <cmd>      Command to run the binary/script (required)\n");
    printf("  -path <dir>     Directory to watch (default: .)\n");
    printf("  -exclude <list> Comma separated list of directories to exclude (default: .git,.idea,...)\n");
    printf("  -v              Verbose output\n");
    printf("  -h              Show this help\n");
}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-build") == 0 && i + 1 < argc) {
            build_cmd = argv[++i];
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

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    // Initial build/run
    if (build_cmd) {
        log_info("Initial build...");
        if (system(build_cmd) != 0) {
            log_err("Initial build failed. Still starting watcher...");
        } else {
            start_process(run_cmd);
        }
    } else {
        start_process(run_cmd);
    }

    // Init watcher
    CNotify* cn = cnotify_init();
    if (!cn) {
        log_err("Failed to initialize watcher");
        return 1;
    }

    exclude_list = split_string(exclude_str, ",");
    if (!exclude_list && exclude_str) {
        log_err("Failed to allocate memory for exclude list");
        cnotify_free(cn);
        return 1;
    }

    log_info("Watching for changes...");
    if (cnotify_add_watch(cn, watch_path, exclude_list) < 0) {
        log_err("Failed to add watch");
    }

    cnotify_start_loop(cn, on_change, NULL);

    cnotify_free(cn);

    // Cleanup
    if (exclude_list) {
        for (int i = 0; exclude_list[i]; i++) {
            free((void*)exclude_list[i]);
        }
        free(exclude_list);
    }

    return 0;
}
