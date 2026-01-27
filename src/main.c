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
// Helper to get basename from path
static const char* get_basename(const char* path) {
    const char* last_slash = strrchr(path, '/');
    return last_slash ? last_slash + 1 : path;
}

// Helper to extract binary name from command string
static char* extract_binary_name(const char* cmd) {
    if (!cmd) return NULL;

    char* tmp = strdup(cmd);
    if (!tmp) return NULL;

    // Get first token (the binary)
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

// Hash map for file content
#define HASH_MAP_SIZE 1024

typedef struct FileHash {
    char* path;
    unsigned long hash;
    struct FileHash* next;
} FileHash;

static FileHash* hash_map[HASH_MAP_SIZE];

// djb2 hash for strings
static unsigned long str_hash(const char* str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) hash = ((hash << 5) + hash) + c;
    return hash;
}

// Simple hash for file content
static unsigned long file_hash(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;  // Treat unreadable as 0

    unsigned long hash = 5381;
    int c;
    while ((c = fgetc(f)) != EOF) hash = ((hash << 5) + hash) + c;

    fclose(f);
    return hash;
}

static void map_put(const char* path, unsigned long hash) {
    unsigned int idx = str_hash(path) % HASH_MAP_SIZE;
    FileHash* curr = hash_map[idx];

    // Update existing
    while (curr) {
        if (strcmp(curr->path, path) == 0) {
            curr->hash = hash;
            return;
        }
        curr = curr->next;
    }

    // Insert new
    FileHash* node = malloc(sizeof(FileHash));
    if (!node) return;
    node->path = strdup(path);
    node->hash = hash;
    node->next = hash_map[idx];
    hash_map[idx] = node;
}

static int map_check_and_update(const char* path) {
    unsigned long new_hash = file_hash(path);
    unsigned int idx = str_hash(path) % HASH_MAP_SIZE;
    FileHash* curr = hash_map[idx];

    while (curr) {
        if (strcmp(curr->path, path) == 0) {
            if (curr->hash == new_hash) return 0;  // No change
            curr->hash = new_hash;
            return 1;  // Changed
        }
        curr = curr->next;
    }

    // Not found, insert new
    map_put(path, new_hash);
    return 1;  // Treated as change
}

// Clean up hash map
static void free_hash_map() {
    for (int i = 0; i < HASH_MAP_SIZE; i++) {
        FileHash* curr = hash_map[i];
        while (curr) {
            FileHash* next = curr->next;
            free(curr->path);
            free(curr);
            curr = next;
        }
        hash_map[i] = NULL;
    }
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

static int on_change(const cnotify_event_t* event, void* user_data) {
    (void)user_data;
    if (verbose) printf("Event: %s/%s\n", event->path, event->name);

    // Only verify content for MODIFY events on files
    // Directories don't have content in the same way, and other events (CREATE, DELETE) definitely change state
    if (event->type == CNOTIFY_EVENT_MODIFY && !event->is_dir) {
        char fullpath[4096];  // Should be enough, or use malloc
        snprintf(fullpath, sizeof(fullpath), "%s/%s", event->path, event->name);

        if (!map_check_and_update(fullpath)) {
            if (verbose) printf("Ignored (content unchanged): %s\n", fullpath);
            return 0;
        }
    } else if (event->type == CNOTIFY_EVENT_DELETE || event->type == CNOTIFY_EVENT_MOVE) {
        // Should ideally remove from map, but lazy leaving it is fine for now
        // Or we could implement map_remove
    }

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

static void parse_config_file(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) return;

    if (verbose) printf("Loading config from %s\n", filename);

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        // Trim newline
        char* p = strchr(line, '\n');
        if (p) *p = '\0';

        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\0') continue;

        // Parse key=value
        char* eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char* key = line;
        char* val = eq + 1;

        // Trim spaces around key
        while (isspace(*key)) key++;
        char* end = key + strlen(key) - 1;
        while (end > key && isspace(*end)) *end-- = '\0';

        // Trim spaces around val
        while (isspace(*val)) val++;
        end = val + strlen(val) - 1;
        while (end > val && isspace(*end)) *end-- = '\0';

        if (strcmp(key, "build") == 0) {
            if (!build_cmd) build_cmd = strdup(val);
        } else if (strcmp(key, "bin") == 0) {
            if (!run_cmd) run_cmd = strdup(val);
        } else if (strcmp(key, "path") == 0) {
            // Only set if still default (checking against "." literal address might fail if compiler merges strings,
            // but strcmp is safer)
            if (strcmp(watch_path, ".") == 0) watch_path = strdup(val);
        } else if (strcmp(key, "exclude") == 0) {
            // Check if exclude_str is the default literal
            if (strncmp(exclude_str, ".git", 4) == 0) exclude_str = strdup(val);
        } else if (strcmp(key, "verbose") == 0) {
            if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0) verbose = 1;
        }
    }

    fclose(f);
}

int main(int argc, char* argv[]) {
    // Check for config file first (defaults)
    parse_config_file("cnotify.conf");

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
    cnotify_t* cn = cnotify_init();
    if (!cn) {
        log_err("Failed to initialize watcher");
        return 1;
    }

    exclude_list = split_string(exclude_str, ",");
    if (!exclude_list && exclude_str) {
        log_err("Failed to allocate memory for exclude list");
        cnotify_destroy(cn);
        return 1;
    }

    // Attempt to automatically add the binary name to exclusion list
    char* binary_name = extract_binary_name(run_cmd);
    if (binary_name) {
        if (verbose) printf("Auto-excluding binary: %s\n", binary_name);

        // Count existing exclusions
        size_t count = 0;
        if (exclude_list) {
            while (exclude_list[count]) count++;
        }

        // Reallocate list to add binary name
        const char** new_list = realloc((void*)exclude_list, sizeof(char*) * (count + 2));
        if (new_list) {
            exclude_list = new_list;
            exclude_list[count] = binary_name;
            exclude_list[count + 1] = NULL;
        } else {
            free(binary_name);  // Failed to add, but not fatal
        }
    }

    log_info("Watching for changes...");
    if (cnotify_add_watch(cn, watch_path, exclude_list) < 0) {
        log_err("Failed to add watch");
    }

    cnotify_start_loop(cn, on_change, NULL);

    cnotify_destroy(cn);

    // Cleanup
    free_hash_map();
    if (exclude_list) {
        for (int i = 0; exclude_list[i]; i++) {
            free((void*)exclude_list[i]);
        }
        free(exclude_list);
    }

    return 0;
}
