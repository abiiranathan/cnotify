# cnotify

A robust, high-performance file system watcher library and command-line tool for Linux, built on top of `inotify`. It is designed to be a faster, more reliable alternative to tools like `air` for hot-reloading applications (e.g., Go, Node.js, Python).

## Features

*   **Fast & Low Overhead:** Written in pure C, using `inotify` directly.
*   **Recursive Watching:** Automatically watches subdirectories.
*   **Dynamic Handling:** Detects new directories and adds watches to them immediately.
*   **Robust Process Management:** Uses process groups (`setpgid`/`kill`) to ensure child processes and their subprocesses are properly killed on reload.
*   **Debouncing:** Batches rapid events to prevent unnecessary restarts.
*   **Zero Dependencies:** Only requires standard Linux libraries (libc).

## Building

```bash
make
```

This will produce the library object `build/cnotify.o` and the command-line tool `bin/cnotify`.

## Usage (CLI)

The `cnotify` tool monitors a directory and restarts a command when changes are detected.

```bash
./bin/cnotify -path <dir> -bin <command> [-build <command>] [-exclude <list>] [-v]
```

### Options

*   `-path <dir>`: Directory to watch (default: current directory).
*   `-bin <cmd>`: The command to run your application. This is required.
*   `-build <cmd>`: Optional command to run before starting the application (e.g., `go build`). If the build fails, the app is not restarted.
*   `-exclude <list>`: Comma-separated list of directories/files to ignore (default: `.git,.idea,.vscode,tmp,vendor,bin`).
*   `-v`: Verbose output (shows which files changed).

### Examples

**Hot-reload a Go application:**

```bash
./bin/cnotify -path . -build "go build -o app" -bin "./app"
```

**Hot-reload a Python script:**

```bash
./bin/cnotify -path src -bin "python3 src/main.py"
```

**Hot-reload a shell script (for testing):**

```bash
./bin/cnotify -path . -bin "./myscript.sh"
```

## Usage (Library)

To use `cnotify` in your own C programs:

1.  Include `cnotify.h`.
2.  Link against `cnotify.o`.

```c
#include "cnotify.h"

int my_callback(CNotifyEvent *event, void *user_data) {
    printf("File changed: %s/%s\n", event->path, event->filename);
    return 0; // Return 1 to stop the loop
}

int main() {
    CNotify *cn = cnotify_init();
    cnotify_add_watch(cn, ".", NULL);
    cnotify_start_loop(cn, my_callback, NULL);
    cnotify_free(cn);
    return 0;
}
```
