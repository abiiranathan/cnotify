/*
 * test.c - Comprehensive test suite for cnotify library
 *
 * Compile: gcc -Wall -Wextra -Iinclude -O3 test.c cnotify.c -o test_cnotify
 */

#ifndef _GNU_SOURCE
    #define _GNU_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../include/cnotify.h"

#define ANSI_GREEN "\x1b[32m"
#define ANSI_RED   "\x1b[31m"
#define ANSI_RESET "\x1b[0m"

#define TEST_PATH_BUF_SIZE (PATH_MAX * 2)

static int test_count = 0;
static int test_passed = 0;
static char g_test_dir[PATH_MAX] = {0};

#define TEST(name)                                        \
    do {                                                  \
        test_count++;                                     \
        printf("Test %2d: %-52s ... ", test_count, name); \
        fflush(stdout);                                   \
    } while (0)

#define PASS()                                     \
    do {                                           \
        test_passed++;                             \
        printf(ANSI_GREEN "PASS" ANSI_RESET "\n"); \
    } while (0)

#define FAIL(msg)                                          \
    do {                                                   \
        printf(ANSI_RED "FAIL" ANSI_RESET " (%s)\n", msg); \
        return -1;                                         \
    } while (0)

/* Construct an absolute path inside g_test_dir safely without truncation warnings */
static void test_path(char* out, size_t out_size, const char* rel_path) {
    if (!rel_path || rel_path[0] == '\0') {
        snprintf(out, out_size, "%s", g_test_dir);
    } else {
        snprintf(out, out_size, "%s/%s", g_test_dir, rel_path);
    }
}

/* Recursively delete the temporary directory */
static void cleanup_test_dir(void) {
    if (g_test_dir[0] != '\0') {
        char cmd[TEST_PATH_BUF_SIZE];
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", g_test_dir);
        (void)system(cmd);
        g_test_dir[0] = '\0';
    }
}

/* Create a fresh, isolated temporary directory using mkdtemp */
static int setup_test_dir(void) {
    cleanup_test_dir();
    char template_path[] = "/tmp/cnotify_test_XXXXXX";
    char* dir = mkdtemp(template_path);
    if (!dir) return -1;
    strncpy(g_test_dir, dir, sizeof(g_test_dir) - 1);
    g_test_dir[sizeof(g_test_dir) - 1] = '\0';
    return 0;
}

/* Helper to write string content into a file inside g_test_dir */
static int write_file(const char* rel_path, const char* content) {
    char full[TEST_PATH_BUF_SIZE];
    test_path(full, sizeof(full), rel_path);
    FILE* f = fopen(full, "w");
    if (!f) return -1;
    if (content && fputs(content, f) < 0) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int count_lines(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;

    int lines = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') lines++;
    }

    fclose(f);
    return lines;
}

/* Test 1: Initialization and cleanup */
static int test_init_destroy(void) {
    TEST("init and destroy");

    cnotify_t* cn = cnotify_init();
    if (!cn) FAIL("cnotify_init failed");

    cnotify_destroy(cn);
    cnotify_destroy(NULL); /* Should be safe */

    PASS();
    return 0;
}

/* Test 2: Add watch on valid directory */
static int test_add_watch(void) {
    TEST("add watch on directory");

    if (setup_test_dir() < 0) FAIL("setup failed");

    cnotify_t* cn = cnotify_init();
    if (!cn) FAIL("init failed");

    if (cnotify_add_watch(cn, g_test_dir, NULL) < 0) {
        cnotify_destroy(cn);
        FAIL("add_watch failed");
    }

    cnotify_destroy(cn);
    PASS();
    return 0;
}

/* Test 3: Add watch with exclusions */
static int test_exclusions(void) {
    TEST("exclusion list filtering");

    if (setup_test_dir() < 0) FAIL("setup failed");

    char path_buf[TEST_PATH_BUF_SIZE];
    test_path(path_buf, sizeof(path_buf), "excluded");
    mkdir(path_buf, 0755);
    test_path(path_buf, sizeof(path_buf), "included");
    mkdir(path_buf, 0755);

    const char* exclude_list[] = {"excluded", ".git", NULL};

    cnotify_t* cn = cnotify_init();
    if (!cn) FAIL("init failed");

    if (cnotify_add_watch(cn, g_test_dir, exclude_list) < 0) {
        cnotify_destroy(cn);
        FAIL("add_watch with exclusions failed");
    }

    cnotify_destroy(cn);
    PASS();
    return 0;
}

/* Test 4: Invalid path error handling */
static int test_invalid_path(void) {
    TEST("invalid path error handling");

    cnotify_t* cn = cnotify_init();
    if (!cn) FAIL("init failed");

    int ret = cnotify_add_watch(cn, "/nonexistent/path/for/cnotify", NULL);
    if (ret == 0) {
        cnotify_destroy(cn);
        FAIL("should have failed on nonexistent path");
    }
    if (errno != ENOENT) {
        cnotify_destroy(cn);
        FAIL("wrong errno value");
    }

    cnotify_destroy(cn);
    PASS();
    return 0;
}

/* Test 5: Debounce setting */
static int test_debounce(void) {
    TEST("debounce configuration");

    cnotify_t* cn = cnotify_init();
    if (!cn) FAIL("init failed");

    cnotify_set_debounce(cn, 0);
    cnotify_set_debounce(cn, 500);
    cnotify_set_debounce(cn, 1000);

    cnotify_destroy(cn);
    PASS();
    return 0;
}

/* Test 6: Get file descriptor */
static int test_get_fd(void) {
    TEST("get inotify file descriptor");

    cnotify_t* cn = cnotify_init();
    if (!cn) FAIL("init failed");

    int fd = cnotify_get_fd(cn);
    if (fd < 0) {
        cnotify_destroy(cn);
        FAIL("invalid fd returned");
    }

    cnotify_destroy(cn);
    PASS();
    return 0;
}

/* Test 7: Event type names */
static int test_event_names(void) {
    TEST("event type string representations");

    if (strcmp(cnotify_event_type_name(CNOTIFY_EVENT_MODIFY), "MODIFY") != 0) FAIL("wrong name for MODIFY");
    if (strcmp(cnotify_event_type_name(CNOTIFY_EVENT_CREATE), "CREATE") != 0) FAIL("wrong name for CREATE");
    if (strcmp(cnotify_event_type_name(CNOTIFY_EVENT_DELETE), "DELETE") != 0) FAIL("wrong name for DELETE");
    if (strcmp(cnotify_event_type_name(CNOTIFY_EVENT_MOVE), "MOVE") != 0) FAIL("wrong name for MOVE");
    if (strcmp(cnotify_event_type_name(CNOTIFY_EVENT_ATTRIB), "ATTRIB") != 0) FAIL("wrong name for ATTRIB");
    if (strcmp(cnotify_event_type_name(CNOTIFY_EVENT_CLOSE_WRITE), "CLOSE_WRITE") != 0)
        FAIL("wrong name for CLOSE_WRITE");
    if (strcmp(cnotify_event_type_name((cnotify_event_type_t)999), "UNKNOWN") != 0) FAIL("wrong name for invalid type");

    PASS();
    return 0;
}

/* Test 8: Basic event loop detection */
static volatile int g_event_received = 0;

static int event_callback_stop(const cnotify_event_t* event, void* userdata) {
    (void)event;
    (void)userdata;
    g_event_received = 1;
    return 1; /* Non-zero stops loop */
}

static int test_event_detection(void) {
    TEST("basic file creation event detection");

    if (setup_test_dir() < 0) FAIL("setup failed");

    cnotify_t* cn = cnotify_init();
    if (!cn) FAIL("init failed");

    if (cnotify_add_watch(cn, g_test_dir, NULL) < 0) {
        cnotify_destroy(cn);
        FAIL("add_watch failed");
    }

    cnotify_set_debounce(cn, 50);

    pid_t pid = fork();
    if (pid < 0) {
        cnotify_destroy(cn);
        FAIL("fork failed");
    }

    if (pid == 0) {
        usleep(100000); /* 100ms */
        write_file("testfile.txt", "hello\n");
        _exit(0);
    }

    g_event_received = 0;
    cnotify_start_loop(cn, event_callback_stop, NULL);

    int status;
    waitpid(pid, &status, 0);
    cnotify_destroy(cn);

    if (!g_event_received) FAIL("no event received");

    PASS();
    return 0;
}

/* Test 9: Real-world recursive directory detection */
static volatile int g_recursive_event_count = 0;
static char g_last_event_path[TEST_PATH_BUF_SIZE] = {0};

static int recursive_event_cb(const cnotify_event_t* event, void* userdata) {
    (void)userdata;
    g_recursive_event_count++;
    snprintf(g_last_event_path, sizeof(g_last_event_path), "%s/%s", event->path, event->name);
    if (strstr(g_last_event_path, "deep_file.txt") != NULL) {
        return 1; /* Stop loop on deep target file */
    }
    return 0;
}

static int test_recursive_subdir_watch(void) {
    TEST("dynamic subdirectory creation & recursive watch");

    if (setup_test_dir() < 0) FAIL("setup failed");

    cnotify_t* cn = cnotify_init();
    if (!cn) FAIL("init failed");

    if (cnotify_add_watch(cn, g_test_dir, NULL) < 0) {
        cnotify_destroy(cn);
        FAIL("add_watch failed");
    }

    cnotify_set_debounce(cn, 20);

    pid_t pid = fork();
    if (pid < 0) {
        cnotify_destroy(cn);
        FAIL("fork failed");
    }

    if (pid == 0) {
        usleep(100000);
        char sub1[TEST_PATH_BUF_SIZE];
        test_path(sub1, sizeof(sub1), "nested_dir");
        mkdir(sub1, 0755);

        usleep(100000); /* Allow inotify to process and watch new dir */
        char deep_file[TEST_PATH_BUF_SIZE];
        test_path(deep_file, sizeof(deep_file), "nested_dir/deep_file.txt");
        FILE* f = fopen(deep_file, "w");
        if (f) {
            fprintf(f, "content\n");
            fclose(f);
        }
        _exit(0);
    }

    g_recursive_event_count = 0;
    g_last_event_path[0] = '\0';
    cnotify_start_loop(cn, recursive_event_cb, NULL);

    int status;
    waitpid(pid, &status, 0);
    cnotify_destroy(cn);

    if (strstr(g_last_event_path, "deep_file.txt") == NULL) {
        FAIL("did not receive event for file in dynamically created subdirectory");
    }

    PASS();
    return 0;
}

/* Test 10: File content hashing and change detection logic */
static int test_file_changed_cache(void) {
    TEST("content hash change detection vs touched mtime");

    if (setup_test_dir() < 0) FAIL("setup failed");

    cnotify_t* cn = cnotify_init();
    if (!cn) FAIL("init failed");

    if (write_file("data.txt", "v1.0") < 0) {
        cnotify_destroy(cn);
        FAIL("write_file failed");
    }

    char target[TEST_PATH_BUF_SIZE];
    test_path(target, sizeof(target), "data.txt");

    /* Pre-seed by adding watch */
    if (cnotify_add_watch(cn, g_test_dir, NULL) < 0) {
        cnotify_destroy(cn);
        FAIL("add_watch failed");
    }

    /* 1. First check right after seeding must return 0 (no changes) */
    if (cnotify_file_changed(cn, target) != 0) {
        cnotify_destroy(cn);
        FAIL("unmodified file flagged as changed");
    }

    /* 2. Touch mtime without modifying contents -> should return 0 */
    usleep(10000);
    struct stat st;
    stat(target, &st);
    struct timespec times[2] = {{st.st_atime, 0}, {st.st_mtime + 5, 0}};
    utimensat(AT_FDCWD, target, times, 0);

    if (cnotify_file_changed(cn, target) != 0) {
        cnotify_destroy(cn);
        FAIL("touched mtime with identical content flagged as changed");
    }

    /* 3. Real content modification -> must return 1 */
    write_file("data.txt", "v2.0 changed bytes");
    if (cnotify_file_changed(cn, target) != 1) {
        cnotify_destroy(cn);
        FAIL("modified file content not detected as change");
    }

    /* 4. Subsequent check without changes -> returns 0 */
    if (cnotify_file_changed(cn, target) != 0) {
        cnotify_destroy(cn);
        FAIL("cached file flagged as changed twice");
    }

    /* 5. Remove from cache */
    if (cnotify_file_remove(cn, target) != 1) {
        cnotify_destroy(cn);
        FAIL("cnotify_file_remove failed to find existing record");
    }

    cnotify_destroy(cn);
    PASS();
    return 0;
}

int callback(const cnotify_event_t* ev, void* u) {
    (void)ev;
    (*(int*)u)++;
    return 0;
}

/* Test 11: Non-blocking event drain */
static int test_process_events_drain(void) {
    TEST("non-blocking cnotify_process_events draining");

    if (setup_test_dir() < 0) FAIL("setup failed");

    cnotify_t* cn = cnotify_init();
    if (!cn) FAIL("init failed");

    if (cnotify_add_watch(cn, g_test_dir, NULL) < 0) {
        cnotify_destroy(cn);
        FAIL("add_watch failed");
    }

    /* Generate multiple events */
    write_file("f1.txt", "1");
    write_file("f2.txt", "2");
    write_file("f3.txt", "3");

    usleep(50000); /* Allow inotify queue to populate */

    int drain_count = 0;
    int ret = cnotify_process_events(cn, callback, &drain_count);
    if (ret < 0) {
        cnotify_destroy(cn);
        FAIL("cnotify_process_events failed");
    }

    if (drain_count < 3) {
        cnotify_destroy(cn);
        FAIL("failed to drain all pending events");
    }

    cnotify_destroy(cn);
    PASS();
    return 0;
}

/* Test 12: NULL parameter handling */
static int test_null_params(void) {
    TEST("NULL parameter safety");

    cnotify_destroy(NULL);

    if (cnotify_get_fd(NULL) != -1) FAIL("get_fd should return -1 for NULL");
    if (cnotify_file_changed(NULL, "path") != 0) FAIL("file_changed should handle NULL context");
    if (cnotify_file_changed((cnotify_t*)0x1, NULL) != 0) FAIL("file_changed should handle NULL path");
    if (cnotify_file_remove(NULL, "path") != 0) FAIL("file_remove should handle NULL context");
    if (cnotify_file_remove((cnotify_t*)0x1, NULL) != 0) FAIL("file_remove should handle NULL path");

    cnotify_set_debounce(NULL, 100);
    cnotify_request_stop(NULL);

    PASS();
    return 0;
}

/* Test 13: Remove watch */
static int test_remove_watch(void) {
    TEST("remove watch by path");

    if (setup_test_dir() < 0) FAIL("setup failed");

    cnotify_t* cn = cnotify_init();
    if (!cn) FAIL("init failed");

    if (cnotify_add_watch(cn, g_test_dir, NULL) < 0) {
        cnotify_destroy(cn);
        FAIL("add_watch failed");
    }

    if (cnotify_remove_watch(cn, g_test_dir) < 0) {
        cnotify_destroy(cn);
        FAIL("remove_watch failed");
    }

    /* Second remove on same path must return -1 (ENOENT) */
    if (cnotify_remove_watch(cn, g_test_dir) == 0) {
        cnotify_destroy(cn);
        FAIL("remove_watch should fail on already removed path");
    }

    cnotify_destroy(cn);
    PASS();
    return 0;
}

/* Test 14: Signal-safe stop request */
static volatile sig_atomic_t g_stop_test_cn_valid = 0;
static cnotify_t* g_stop_test_cn = NULL;

static void stop_test_alarm_handler(int sig) {
    (void)sig;
    if (g_stop_test_cn_valid && g_stop_test_cn) {
        cnotify_request_stop(g_stop_test_cn);
    }
}

static int never_called_callback(const cnotify_event_t* event, void* userdata) {
    (void)event;
    (void)userdata;
    return 0;
}

static int test_request_stop(void) {
    TEST("request_stop unblocks blocking loop safely");

    if (setup_test_dir() < 0) FAIL("setup failed");

    cnotify_t* cn = cnotify_init();
    if (!cn) FAIL("init failed");

    if (cnotify_add_watch(cn, g_test_dir, NULL) < 0) {
        cnotify_destroy(cn);
        FAIL("add_watch failed");
    }

    cnotify_set_debounce(cn, 0);

    g_stop_test_cn = cn;
    g_stop_test_cn_valid = 1;

    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop_test_alarm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGALRM, &sa, &old_sa) < 0) {
        cnotify_destroy(cn);
        FAIL("sigaction failed");
    }

    alarm(1);

    int ret = cnotify_start_loop(cn, never_called_callback, NULL);

    alarm(0);
    sigaction(SIGALRM, &old_sa, NULL);
    g_stop_test_cn_valid = 0;
    g_stop_test_cn = NULL;

    cnotify_destroy(cn);

    if (ret != 0) FAIL("start_loop did not return cleanly after request_stop");

    PASS();
    return 0;
}

/* Test 15: CLI integration - Unchanged atomic save (write tmp + rename) */
static int test_cli_unchanged_atomic_save(void) {
    TEST("CLI atomic save with identical content does not restart");

    if (setup_test_dir() < 0) FAIL("setup failed");

    char runner_path[TEST_PATH_BUF_SIZE], runlog_path[TEST_PATH_BUF_SIZE];
    char watched_path[TEST_PATH_BUF_SIZE], tmp_path[TEST_PATH_BUF_SIZE];

    test_path(runner_path, sizeof(runner_path), "runner.sh");
    test_path(runlog_path, sizeof(runlog_path), "run.log");
    test_path(watched_path, sizeof(watched_path), "watched.txt");
    test_path(tmp_path, sizeof(tmp_path), "watched.tmp");

    FILE* f = fopen(runner_path, "w");
    if (!f) FAIL("failed to create runner script");
    fprintf(f, "#!/bin/sh\necho run >> \"$1\"\nsleep 60\n");
    fclose(f);
    chmod(runner_path, 0755);

    f = fopen(watched_path, "w");
    if (!f) FAIL("failed to create watched file");
    fprintf(f, "hello\n");
    fclose(f);

    pid_t pid = fork();
    if (pid < 0) FAIL("fork failed");

    if (pid == 0) {
        char bin_cmd[TEST_PATH_BUF_SIZE * 2 + 16];
        snprintf(bin_cmd, sizeof(bin_cmd), "sh %s %s", runner_path, runlog_path);
        execl("./bin/cnotify", "./bin/cnotify", "-path", g_test_dir, "-exclude",
              ".git,.idea,.vscode,tmp,vendor,bin,run.log,runner.sh", "-bin", bin_cmd, (char*)NULL);
        _exit(127);
    }

    usleep(900000);

    int lines_after_start = count_lines(runlog_path);
    if (lines_after_start < 1) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        FAIL("initial process did not start");
    }

    /* Simulate atomic rename with identical content */
    f = fopen(tmp_path, "w");
    if (!f) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        FAIL("failed to create temp file");
    }
    fprintf(f, "hello\n");
    fclose(f);

    if (rename(tmp_path, watched_path) < 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        FAIL("rename failed");
    }

    usleep(900000);
    int lines_after_unchanged = count_lines(runlog_path);

    /* Real content change should trigger a restart */
    f = fopen(watched_path, "w");
    if (!f) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        FAIL("failed to modify watched file");
    }
    fprintf(f, "hello changed\n");
    fclose(f);

    usleep(1200000);
    int lines_after_change = count_lines(runlog_path);

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    if (lines_after_unchanged != lines_after_start) FAIL("unchanged atomic save caused restart");
    if (lines_after_change <= lines_after_unchanged) FAIL("real content change did not restart");

    PASS();
    return 0;
}

/* Test 16: CLI integration - Clean SIGTERM shutdown */
static int test_cli_sigterm_shutdown(void) {
    TEST("CLI clean process termination on SIGTERM");

    if (setup_test_dir() < 0) FAIL("setup failed");

    write_file("watched.txt", "hello\n");

    pid_t pid = fork();
    if (pid < 0) FAIL("fork failed");

    if (pid == 0) {
        execl("./bin/cnotify", "./bin/cnotify", "-path", g_test_dir, "-bin", "sleep 60", (char*)NULL);
        _exit(127);
    }

    usleep(500000);

    if (kill(pid, SIGTERM) < 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        FAIL("failed to send SIGTERM");
    }

    int status;
    pid_t waited = waitpid(pid, &status, 0);
    if (waited != pid) FAIL("cnotify process did not exit after SIGTERM");

    if (!WIFEXITED(status) && !WIFSIGNALED(status)) FAIL("unexpected exit status");

    PASS();
    return 0;
}

/* Test 17: CLI integration - Custom exclude retains default exclude list */
static int test_cli_custom_exclude_keeps_defaults(void) {
    TEST("CLI custom exclude retains default excludes");

    if (setup_test_dir() < 0) FAIL("setup failed");

    char runner_path[TEST_PATH_BUF_SIZE], runlog_path[TEST_PATH_BUF_SIZE];
    char git_dir[TEST_PATH_BUF_SIZE], custom_dir[TEST_PATH_BUF_SIZE];

    test_path(runner_path, sizeof(runner_path), "runner.sh");
    test_path(runlog_path, sizeof(runlog_path), "run.log");
    test_path(git_dir, sizeof(git_dir), ".git");
    test_path(custom_dir, sizeof(custom_dir), "custom_excl");

    mkdir(git_dir, 0755);
    mkdir(custom_dir, 0755);

    FILE* f = fopen(runner_path, "w");
    if (!f) FAIL("failed to create runner script");
    fprintf(f, "#!/bin/sh\necho run >> \"$1\"\nsleep 60\n");
    fclose(f);
    chmod(runner_path, 0755);

    write_file("watched.txt", "initial\n");

    pid_t pid = fork();
    if (pid < 0) FAIL("fork failed");

    if (pid == 0) {
        char bin_cmd[TEST_PATH_BUF_SIZE * 2 + 16];
        snprintf(bin_cmd, sizeof(bin_cmd), "sh %s %s", runner_path, runlog_path);
        execl("./bin/cnotify", "./bin/cnotify", "-path", g_test_dir, "-exclude", "custom_excl,run.log,runner.sh",
              "-bin", bin_cmd, (char*)NULL);
        _exit(127);
    }

    usleep(900000);
    int lines_initial = count_lines(runlog_path);
    if (lines_initial < 1) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        FAIL("initial process did not start");
    }

    /* 1. Modify inside .git (default exclude) */
    write_file(".git/config", "git change\n");
    usleep(900000);
    int lines_after_git = count_lines(runlog_path);
    if (lines_after_git != lines_initial) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        FAIL("change in default exclude directory (.git) triggered reload");
    }

    /* 2. Modify inside custom_excl */
    write_file("custom_excl/file.txt", "custom change\n");
    usleep(900000);
    int lines_after_custom = count_lines(runlog_path);
    if (lines_after_custom != lines_initial) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        FAIL("change in user exclude directory triggered reload");
    }

    /* 3. Modify normal watched file */
    write_file("watched.txt", "updated content\n");
    usleep(1200000);
    int lines_after_watched = count_lines(runlog_path);

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    if (lines_after_watched <= lines_after_custom) FAIL("change in watched file did not trigger reload");

    PASS();
    return 0;
}

/* Test 18: CLI integration - Ignore specific files and glob patterns */
static int test_cli_ignore_files_and_patterns(void) {
    TEST("CLI exact files and glob pattern ignores");

    if (setup_test_dir() < 0) FAIL("setup failed");

    char runner_path[TEST_PATH_BUF_SIZE], runlog_path[TEST_PATH_BUF_SIZE], static_css_dir[TEST_PATH_BUF_SIZE];
    test_path(runner_path, sizeof(runner_path), "runner.sh");
    test_path(runlog_path, sizeof(runlog_path), "run.log");
    test_path(static_css_dir, sizeof(static_css_dir), "static/css");

    char cmd[TEST_PATH_BUF_SIZE + 16];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"%s\"", static_css_dir);
    (void)system(cmd);

    FILE* f = fopen(runner_path, "w");
    if (!f) FAIL("failed to create runner script");
    fprintf(f, "#!/bin/sh\necho run >> \"$1\"\nsleep 60\n");
    fclose(f);
    chmod(runner_path, 0755);

    write_file("static/css/styles.css", "body { color: red; }\n");
    write_file("main_test.go", "package main\n");
    write_file("bundle.min.js", "console.log(1);\n");
    write_file("main.go", "package main\nfunc main() {}\n");

    pid_t pid = fork();
    if (pid < 0) FAIL("fork failed");

    if (pid == 0) {
        char bin_cmd[TEST_PATH_BUF_SIZE * 2 + 16];
        snprintf(bin_cmd, sizeof(bin_cmd), "sh %s %s", runner_path, runlog_path);
        execl("./bin/cnotify", "./bin/cnotify", "-path", g_test_dir, "-exclude", "run.log,runner.sh", "-ignore",
              "static/css/styles.css, *_test.go, *.min.js", "-bin", bin_cmd, (char*)NULL);
        _exit(127);
    }

    usleep(900000);
    int lines_initial = count_lines(runlog_path);
    if (lines_initial < 1) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        FAIL("initial process did not start");
    }

    /* 1. Modify ignored exact path */
    write_file("static/css/styles.css", "body { color: blue; }\n");
    usleep(600000);

    /* 2. Modify glob *_test.go */
    write_file("main_test.go", "package main\n// updated\n");
    usleep(600000);

    /* 3. Modify glob *.min.js */
    write_file("bundle.min.js", "console.log(2);\n");
    usleep(600000);

    int lines_after_ignored = count_lines(runlog_path);
    if (lines_after_ignored != lines_initial) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        FAIL("modifying ignored files/patterns caused reload");
    }

    /* 4. Modify regular file */
    write_file("main.go", "package main\nfunc main() { /* changed */ }\n");
    usleep(1200000);
    int lines_after_watched = count_lines(runlog_path);

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    if (lines_after_watched <= lines_after_ignored) FAIL("change in watched file did not trigger reload");

    PASS();
    return 0;
}

int main(void) {
    printf("\n=== cnotify Test Suite ===\n\n");

    test_init_destroy();
    test_add_watch();
    test_exclusions();
    test_invalid_path();
    test_debounce();
    test_get_fd();
    test_event_names();
    test_event_detection();
    test_recursive_subdir_watch();
    test_file_changed_cache();
    test_process_events_drain();
    test_null_params();
    test_remove_watch();
    test_request_stop();
    test_cli_unchanged_atomic_save();
    test_cli_sigterm_shutdown();
    test_cli_custom_exclude_keeps_defaults();
    test_cli_ignore_files_and_patterns();

    cleanup_test_dir();

    printf("\n=== Results ===\n");
    printf("Total tests: %d\n", test_count);
    printf("Passed:      " ANSI_GREEN "%d" ANSI_RESET "\n", test_passed);
    printf("Failed:      " ANSI_RED "%d" ANSI_RESET "\n", test_count - test_passed);

    return (test_passed == test_count) ? 0 : 1;
}
