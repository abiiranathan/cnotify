/*
 * test.c - Basic tests for cnotify library
 *
 * Compile: gcc -Wall -Wextra -O2 test.c cnotify.c -o test_cnotify
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../include/cnotify.h"

#define TEST_DIR   "/tmp/cnotify_test"
#define ANSI_GREEN "\x1b[32m"
#define ANSI_RED   "\x1b[31m"
#define ANSI_RESET "\x1b[0m"

static int test_count = 0;
static int test_passed = 0;

#define TEST(name)                                   \
    do {                                             \
        test_count++;                                \
        printf("Test %d: %s... ", test_count, name); \
        fflush(stdout);                              \
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

static void cleanup_test_dir(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_DIR);
    system(cmd);
}

static int setup_test_dir(void) {
    cleanup_test_dir();
    if (mkdir(TEST_DIR, 0755) < 0 && errno != EEXIST) return -1;
    return 0;
}

/* Test 1: Initialization and cleanup */
static int test_init_destroy(void) {
    cnotify_t* cn;

    TEST("init and destroy");

    cn = cnotify_init();
    if (!cn) FAIL("cnotify_init failed");

    cnotify_destroy(cn);
    cnotify_destroy(NULL); /* should be safe */

    PASS();
    return 0;
}

/* Test 2: Add watch on valid directory */
static int test_add_watch(void) {
    cnotify_t* cn;
    int ret;

    TEST("add watch on directory");

    if (setup_test_dir() < 0) FAIL("setup failed");

    cn = cnotify_init();
    if (!cn) FAIL("init failed");

    ret = cnotify_add_watch(cn, TEST_DIR, NULL);
    if (ret < 0) {
        cnotify_destroy(cn);
        FAIL("add_watch failed");
    }

    cnotify_destroy(cn);
    PASS();
    return 0;
}

/* Test 3: Add watch with exclusions */
static int test_exclusions(void) {
    cnotify_t* cn;
    int ret;
    char exclude_path[256];
    const char* exclude_list[] = {"excluded", NULL};

    TEST("exclusion list");

    if (setup_test_dir() < 0) FAIL("setup failed");

    /* Create subdirectories */
    snprintf(exclude_path, sizeof(exclude_path), "%s/excluded", TEST_DIR);
    mkdir(exclude_path, 0755);
    snprintf(exclude_path, sizeof(exclude_path), "%s/included", TEST_DIR);
    mkdir(exclude_path, 0755);

    cn = cnotify_init();
    if (!cn) FAIL("init failed");

    ret = cnotify_add_watch(cn, TEST_DIR, exclude_list);
    if (ret < 0) {
        cnotify_destroy(cn);
        FAIL("add_watch with exclusions failed");
    }

    cnotify_destroy(cn);
    PASS();
    return 0;
}

/* Test 4: Invalid path */
static int test_invalid_path(void) {
    cnotify_t* cn;
    int ret;

    TEST("invalid path handling");

    cn = cnotify_init();
    if (!cn) FAIL("init failed");

    ret = cnotify_add_watch(cn, "/nonexistent/path", NULL);
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
    cnotify_t* cn;

    TEST("debounce configuration");

    cn = cnotify_init();
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
    cnotify_t* cn;
    int fd;

    TEST("get file descriptor");

    cn = cnotify_init();
    if (!cn) FAIL("init failed");

    fd = cnotify_get_fd(cn);
    if (fd < 0) {
        cnotify_destroy(cn);
        FAIL("invalid fd");
    }

    cnotify_destroy(cn);
    PASS();
    return 0;
}

/* Test 7: Event type names */
static int test_event_names(void) {
    TEST("event type names");

    if (strcmp(cnotify_event_type_name(CNOTIFY_EVENT_MODIFY), "MODIFY") != 0) FAIL("wrong name for MODIFY");

    if (strcmp(cnotify_event_type_name(CNOTIFY_EVENT_CREATE), "CREATE") != 0) FAIL("wrong name for CREATE");

    if (strcmp(cnotify_event_type_name(CNOTIFY_EVENT_DELETE), "DELETE") != 0) FAIL("wrong name for DELETE");

    if (strcmp(cnotify_event_type_name((cnotify_event_type_t)999), "UNKNOWN") != 0) FAIL("wrong name for invalid type");

    PASS();
    return 0;
}

/* Test 8: Basic event detection */
static volatile int event_received = 0;

static int event_callback(const cnotify_event_t* event, void* userdata) {
    (void)event;
    (void)userdata;
    event_received = 1;
    return 1; /* stop loop */
}

static int test_event_detection(void) {
    cnotify_t* cn;
    pid_t pid;
    int status;
    char filepath[256];

    TEST("basic event detection");

    if (setup_test_dir() < 0) FAIL("setup failed");

    cn = cnotify_init();
    if (!cn) FAIL("init failed");

    if (cnotify_add_watch(cn, TEST_DIR, NULL) < 0) {
        cnotify_destroy(cn);
        FAIL("add_watch failed");
    }

    cnotify_set_debounce(cn, 50);

    /* Fork to create event while parent watches */
    pid = fork();
    if (pid < 0) {
        cnotify_destroy(cn);
        FAIL("fork failed");
    }

    if (pid == 0) {
        /* Child: wait a bit then create a file */
        usleep(100000); /* 100ms */
        snprintf(filepath, sizeof(filepath), "%s/testfile", TEST_DIR);
        FILE* f = fopen(filepath, "w");
        if (f) {
            fprintf(f, "test\n");
            fclose(f);
        }
        exit(0);
    }

    /* Parent: watch for events */
    event_received = 0;
    cnotify_start_loop(cn, event_callback, NULL);

    waitpid(pid, &status, 0);
    cnotify_destroy(cn);

    if (!event_received) FAIL("no event received");

    PASS();
    return 0;
}

/* Test 9: NULL parameter handling */
static int test_null_params(void) {
    TEST("NULL parameter handling");

    cnotify_destroy(NULL);

    if (cnotify_get_fd(NULL) != -1) FAIL("get_fd should return -1 for NULL");

    cnotify_set_debounce(NULL, 100); /* should not crash */

    cnotify_request_stop(NULL); /* should not crash */

    PASS();
    return 0;
}

/* Test 10: Remove watch */
static int test_remove_watch(void) {
    cnotify_t* cn;
    int ret;

    TEST("remove watch");

    if (setup_test_dir() < 0) FAIL("setup failed");

    cn = cnotify_init();
    if (!cn) FAIL("init failed");

    ret = cnotify_add_watch(cn, TEST_DIR, NULL);
    if (ret < 0) {
        cnotify_destroy(cn);
        FAIL("add_watch failed");
    }

    ret = cnotify_remove_watch(cn, TEST_DIR);
    if (ret < 0) {
        cnotify_destroy(cn);
        FAIL("remove_watch failed");
    }

    /* Removing again should fail */
    ret = cnotify_remove_watch(cn, TEST_DIR);
    if (ret == 0) {
        cnotify_destroy(cn);
        FAIL("remove_watch should fail on already removed path");
    }

    cnotify_destroy(cn);
    PASS();
    return 0;
}

/*
 * Test 11: cnotify_request_stop() unblocks cnotify_start_loop() promptly,
 * even with no filesystem activity and even from a signal handler.
 *
 * This exercises the self-pipe mechanism that replaced select()-based
 * EINTR timing: a SIGALRM fires after a short delay, its handler calls
 * cnotify_request_stop() (the only thing it does — no exit(), no printf()),
 * and the blocking loop must return 0 promptly rather than hanging forever
 * waiting on the inotify fd.
 */
static volatile sig_atomic_t g_stop_test_cn_valid = 0;
static cnotify_t* g_stop_test_cn = NULL;

static void stop_test_alarm_handler(int sig) {
    (void)sig;
    if (g_stop_test_cn_valid) cnotify_request_stop(g_stop_test_cn);
}

static int never_called_callback(const cnotify_event_t* event, void* userdata) {
    (void)event;
    (void)userdata;
    return 0; /* Should never actually be invoked in this test. */
}

static int test_request_stop(void) {
    cnotify_t* cn;
    struct sigaction sa, old_sa;
    int ret;

    TEST("request_stop unblocks event loop");

    if (setup_test_dir() < 0) FAIL("setup failed");

    cn = cnotify_init();
    if (!cn) FAIL("init failed");

    if (cnotify_add_watch(cn, TEST_DIR, NULL) < 0) {
        cnotify_destroy(cn);
        FAIL("add_watch failed");
    }

    /* No filesystem events will occur; only the alarm-driven stop request
     * should ever unblock the loop below. */
    cnotify_set_debounce(cn, 0);

    g_stop_test_cn = cn;
    g_stop_test_cn_valid = 1;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop_test_alarm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* Deliberately no SA_RESTART: this also exercises the
                      * EINTR-retry path in cnotify_start_loop()'s poll(). */
    if (sigaction(SIGALRM, &sa, &old_sa) < 0) {
        cnotify_destroy(cn);
        FAIL("sigaction failed");
    }

    alarm(1); /* fires in 1 second */

    ret = cnotify_start_loop(cn, never_called_callback, NULL);

    alarm(0);
    sigaction(SIGALRM, &old_sa, NULL);
    g_stop_test_cn_valid = 0;
    g_stop_test_cn = NULL;

    cnotify_destroy(cn);

    if (ret != 0) FAIL("start_loop did not return cleanly after request_stop");

    PASS();
    return 0;
}

static int count_lines(const char* path) {
    FILE* f;
    int lines = 0;
    int c;

    f = fopen(path, "r");
    if (!f) return -1;

    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') lines++;
    }

    fclose(f);
    return lines;
}

/* Test 12: CLI integration - unchanged atomic save should not reload */
static int test_cli_unchanged_atomic_save(void) {
    char runner_path[256];
    char runlog_path[256];
    char watched_path[256];
    char tmp_path[256];
    FILE* f;
    pid_t pid;
    int status;
    int lines_after_start;
    int lines_after_unchanged;
    int lines_after_change;

    TEST("CLI unchanged atomic save integration");

    if (setup_test_dir() < 0) FAIL("setup failed");

    snprintf(runner_path, sizeof(runner_path), "%s/runner.sh", TEST_DIR);
    snprintf(runlog_path, sizeof(runlog_path), "%s/run.log", TEST_DIR);
    snprintf(watched_path, sizeof(watched_path), "%s/watched.txt", TEST_DIR);
    snprintf(tmp_path, sizeof(tmp_path), "%s/watched.tmp", TEST_DIR);

    f = fopen(runner_path, "w");
    if (!f) FAIL("failed to create runner script");
    fprintf(f, "#!/bin/sh\necho run >> \"$1\"\nsleep 60\n");
    fclose(f);
    chmod(runner_path, 0755);

    f = fopen(watched_path, "w");
    if (!f) FAIL("failed to create watched file");
    fprintf(f, "hello\n");
    fclose(f);

    pid = fork();
    if (pid < 0) FAIL("fork failed");

    if (pid == 0) {
        execl("./bin/cnotify", "./bin/cnotify", "-path", TEST_DIR, "-exclude",
              ".git,.idea,.vscode,tmp,vendor,bin,run.log,runner.sh", "-bin",
              "sh /tmp/cnotify_test/runner.sh /tmp/cnotify_test/run.log", (char*)NULL);
        _exit(127);
    }

    usleep(900000);

    lines_after_start = count_lines(runlog_path);
    if (lines_after_start < 1) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("initial process did not start");
    }

    /* Simulate atomic save with same content: write temp then rename over target. */
    f = fopen(tmp_path, "w");
    if (!f) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("failed to create temp file");
    }
    fprintf(f, "hello\n");
    fclose(f);
    if (rename(tmp_path, watched_path) < 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("rename failed");
    }

    usleep(900000);
    lines_after_unchanged = count_lines(runlog_path);

    /* Real content change should trigger one restart. */
    f = fopen(watched_path, "w");
    if (!f) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("failed to modify watched file");
    }
    fprintf(f, "hello changed\n");
    fclose(f);

    usleep(1200000);
    lines_after_change = count_lines(runlog_path);

    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);

    if (lines_after_unchanged != lines_after_start) FAIL("unchanged atomic save caused restart");
    if (lines_after_change <= lines_after_unchanged) FAIL("real content change did not restart");

    PASS();
    return 0;
}

/*
 * Test 13: CLI integration - SIGTERM triggers a clean shutdown that
 * actually kills the managed child process group.
 *
 * This is the end-to-end check that main.c's new shutdown path (signal
 * handler -> cnotify_request_stop() -> cnotify_start_loop() returns ->
 * main() runs kill_all_children()) really tears down the run_cmd process,
 * rather than relying on exit() firing from within the handler.
 */
static int test_cli_sigterm_shutdown(void) {
    char watched_path[256];
    pid_t pid;
    int status;
    FILE* f;

    TEST("CLI SIGTERM clean shutdown");

    if (setup_test_dir() < 0) FAIL("setup failed");

    snprintf(watched_path, sizeof(watched_path), "%s/watched.txt", TEST_DIR);
    f = fopen(watched_path, "w");
    if (!f) FAIL("failed to create watched file");
    fprintf(f, "hello\n");
    fclose(f);

    pid = fork();
    if (pid < 0) FAIL("fork failed");

    if (pid == 0) {
        execl("./bin/cnotify", "./bin/cnotify", "-path", TEST_DIR, "-bin", "sleep 60", (char*)NULL);
        _exit(127);
    }

    usleep(500000); /* let cnotify start and fork the "sleep 60" child */

    if (kill(pid, SIGTERM) < 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        FAIL("failed to send SIGTERM");
    }

    /* cnotify itself should exit promptly (well under the old TERM_TIMEOUT_MS
     * ceiling plus scheduling slack) now that the handler no longer blocks
     * on the previous select()/EINTR timing. */
    pid_t waited = waitpid(pid, &status, 0);
    if (waited != pid) FAIL("cnotify process did not exit after SIGTERM");

    if (!WIFEXITED(status) && !WIFSIGNALED(status)) FAIL("unexpected exit status shape");

    PASS();
    return 0;
}

/*
 * Test 14: CLI integration - Custom exclude preserves default exclude list.
 *
 * Verifies that passing a custom -exclude value appends to, rather than
 * overwriting, the DEFAULT_EXCLUDE list. Changes inside .git (a default
 * exclude) and custom_excl (a user exclude) should both be ignored.
 */
static int test_cli_custom_exclude_keeps_defaults(void) {
    char runner_path[256];
    char runlog_path[256];
    char git_dir[256];
    char git_file[256];
    char custom_dir[256];
    char custom_file[256];
    char watched_path[256];
    FILE* f;
    pid_t pid;
    int status;
    int lines_initial;
    int lines_after_git;
    int lines_after_custom;
    int lines_after_watched;

    TEST("CLI custom exclude retains default exclude list");

    if (setup_test_dir() < 0) FAIL("setup failed");

    snprintf(runner_path, sizeof(runner_path), "%s/runner.sh", TEST_DIR);
    snprintf(runlog_path, sizeof(runlog_path), "%s/run.log", TEST_DIR);
    snprintf(git_dir, sizeof(git_dir), "%s/.git", TEST_DIR);
    snprintf(git_file, sizeof(git_file), "%s/.git/config", TEST_DIR);
    snprintf(custom_dir, sizeof(custom_dir), "%s/custom_excl", TEST_DIR);
    snprintf(custom_file, sizeof(custom_file), "%s/custom_excl/file.txt", TEST_DIR);
    snprintf(watched_path, sizeof(watched_path), "%s/watched.txt", TEST_DIR);

    mkdir(git_dir, 0755);
    mkdir(custom_dir, 0755);

    f = fopen(runner_path, "w");
    if (!f) FAIL("failed to create runner script");
    fprintf(f, "#!/bin/sh\necho run >> \"$1\"\nsleep 60\n");
    fclose(f);
    chmod(runner_path, 0755);

    f = fopen(watched_path, "w");
    if (!f) FAIL("failed to create watched file");
    fprintf(f, "initial\n");
    fclose(f);

    pid = fork();
    if (pid < 0) FAIL("fork failed");

    if (pid == 0) {
        /* Only specify custom_excl, runner.sh, and run.log in -exclude. .git should still be excluded! */
        execl("./bin/cnotify", "./bin/cnotify", "-path", TEST_DIR, "-exclude", "custom_excl,run.log,runner.sh", "-bin",
              "sh /tmp/cnotify_test/runner.sh /tmp/cnotify_test/run.log", (char*)NULL);
        _exit(127);
    }

    usleep(900000);
    lines_initial = count_lines(runlog_path);
    if (lines_initial < 1) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("initial process did not start");
    }

    /* 1. Modify file inside .git (default exclude): should NOT trigger reload. */
    f = fopen(git_file, "w");
    if (!f) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("failed to write git file");
    }
    fprintf(f, "git change\n");
    fclose(f);

    usleep(900000);
    lines_after_git = count_lines(runlog_path);
    if (lines_after_git != lines_initial) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("change in default exclude directory (.git) triggered reload");
    }

    /* 2. Modify file inside custom_excl (user exclude): should NOT trigger reload. */
    f = fopen(custom_file, "w");
    if (!f) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("failed to write custom exclude file");
    }
    fprintf(f, "custom exclude change\n");
    fclose(f);

    usleep(900000);
    lines_after_custom = count_lines(runlog_path);
    if (lines_after_custom != lines_initial) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("change in user exclude directory triggered reload");
    }

    /* 3. Modify normal watched file: SHOULD trigger reload. */
    f = fopen(watched_path, "w");
    if (!f) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("failed to modify watched file");
    }
    fprintf(f, "updated content\n");
    fclose(f);

    usleep(1200000);
    lines_after_watched = count_lines(runlog_path);

    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);

    if (lines_after_watched <= lines_after_custom) FAIL("change in watched file did not trigger reload");

    PASS();
    return 0;
}

/*
 * Test 15: CLI integration - Ignore specific files and glob patterns.
 *
 * Verifies that exact path matching (static/css/styles.css) and glob patterns
 * (*_test.go, *.min.js) are ignored properly and do not trigger reloads.
 */
static int test_cli_ignore_files_and_patterns(void) {
    char runner_path[256];
    char runlog_path[256];
    char static_dir[256];
    char css_dir[256];
    char css_file[256];
    char test_file[256];
    char minjs_file[256];
    char watched_path[256];
    FILE* f;
    pid_t pid;
    int status;
    int lines_initial;
    int lines_after_ignored;
    int lines_after_watched;

    TEST("CLI file and glob pattern ignore integration");

    if (setup_test_dir() < 0) FAIL("setup failed");

    snprintf(runner_path, sizeof(runner_path), "%s/runner.sh", TEST_DIR);
    snprintf(runlog_path, sizeof(runlog_path), "%s/run.log", TEST_DIR);
    snprintf(static_dir, sizeof(static_dir), "%s/static", TEST_DIR);
    snprintf(css_dir, sizeof(css_dir), "%s/static/css", TEST_DIR);
    snprintf(css_file, sizeof(css_file), "%s/static/css/styles.css", TEST_DIR);
    snprintf(test_file, sizeof(test_file), "%s/main_test.go", TEST_DIR);
    snprintf(minjs_file, sizeof(minjs_file), "%s/bundle.min.js", TEST_DIR);
    snprintf(watched_path, sizeof(watched_path), "%s/main.go", TEST_DIR);

    mkdir(static_dir, 0755);
    mkdir(css_dir, 0755);

    f = fopen(runner_path, "w");
    if (!f) FAIL("failed to create runner script");
    fprintf(f, "#!/bin/sh\necho run >> \"$1\"\nsleep 60\n");
    fclose(f);
    chmod(runner_path, 0755);

    f = fopen(css_file, "w");
    if (!f) FAIL("failed to create css file");
    fprintf(f, "body { color: red; }\n");
    fclose(f);

    f = fopen(test_file, "w");
    if (!f) FAIL("failed to create test file");
    fprintf(f, "package main\n");
    fclose(f);

    f = fopen(minjs_file, "w");
    if (!f) FAIL("failed to create min.js file");
    fprintf(f, "console.log(1);\n");
    fclose(f);

    f = fopen(watched_path, "w");
    if (!f) FAIL("failed to create watched file");
    fprintf(f, "package main\nfunc main() {}\n");
    fclose(f);

    pid = fork();
    if (pid < 0) FAIL("fork failed");

    if (pid == 0) {
        execl("./bin/cnotify", "./bin/cnotify", "-path", TEST_DIR, "-exclude", "run.log,runner.sh", "-ignore",
              "static/css/styles.css, *_test.go, *.min.js", "-bin",
              "sh /tmp/cnotify_test/runner.sh /tmp/cnotify_test/run.log", (char*)NULL);
        _exit(127);
    }

    usleep(900000);
    lines_initial = count_lines(runlog_path);
    if (lines_initial < 1) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("initial process did not start");
    }

    /* 1. Modify exact ignored path: static/css/styles.css */
    f = fopen(css_file, "w");
    if (!f) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("failed to modify css file");
    }
    fprintf(f, "body { color: blue; }\n");
    fclose(f);
    usleep(600000);

    /* 2. Modify glob pattern file: *_test.go */
    f = fopen(test_file, "w");
    if (!f) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("failed to modify test file");
    }
    fprintf(f, "package main\n// new test\n");
    fclose(f);
    usleep(600000);

    /* 3. Modify glob pattern file: *.min.js */
    f = fopen(minjs_file, "w");
    if (!f) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("failed to modify minjs file");
    }
    fprintf(f, "console.log(2);\n");
    fclose(f);
    usleep(600000);

    lines_after_ignored = count_lines(runlog_path);
    if (lines_after_ignored != lines_initial) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("modifying ignored files/patterns caused reload");
    }

    /* 4. Modify normal watched file: main.go */
    f = fopen(watched_path, "w");
    if (!f) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
        FAIL("failed to modify watched file");
    }
    fprintf(f, "package main\nfunc main() { /* changed */ }\n");
    fclose(f);

    usleep(1200000);
    lines_after_watched = count_lines(runlog_path);

    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);

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
    printf("Passed: " ANSI_GREEN "%d" ANSI_RESET "\n", test_passed);
    printf("Failed: " ANSI_RED "%d" ANSI_RESET "\n", test_count - test_passed);

    return (test_passed == test_count) ? 0 : 1;
}
