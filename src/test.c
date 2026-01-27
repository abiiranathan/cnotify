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

    cleanup_test_dir();

    printf("\n=== Results ===\n");
    printf("Total tests: %d\n", test_count);
    printf("Passed: " ANSI_GREEN "%d" ANSI_RESET "\n", test_passed);
    printf("Failed: " ANSI_RED "%d" ANSI_RESET "\n", test_count - test_passed);

    return (test_passed == test_count) ? 0 : 1;
}
