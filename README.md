# cnotify

A robust, high-performance file system watcher library and command-line tool for Linux, built on top of `inotify`. It is designed to be a faster, more reliable alternative to tools like `air` for hot-reloading applications (e.g., Go, Node.js, Python).

## Features

*   **Fast & Low Overhead:** Written in pure C, using `inotify` directly.
*   **Recursive Watching:** Automatically watches subdirectories.
*   **Dynamic Handling:** Detects new directories and adds watches to them immediately.
*   **Robust Process Management:** Uses process groups (`setpgid`/`kill`) to ensure child processes and their subprocesses are properly killed on reload.
*   **Debouncing:** Batches rapid events to prevent unnecessary restarts.
*   **Smart Exclusion:** Automatically ignores the output binary (if specified via `-bin`) to prevent infinite loops.
*   **Content-Based Caching:** Hashes file content to ensure reloads only happen when content *actually* changes (ignoring atomic saves or "Ctrl+S without changes").
*   **Config File Support:** Can read configuration from `cnotify.conf`.
*   **Zero Dependencies:** Only requires standard Linux libraries (libc).

## Building

To build the project (CLI tool, static library, and shared library):

```bash
make
```

This produces:
*   `bin/cnotify`: The hot-reload CLI tool.
*   `lib/libcnotify.a`: Static library.
*   `lib/libcnotify.so`: Shared library.

To run tests:
```bash
make test
```

## Installation

To install the binary, libraries, and header file to standard system locations (`/usr/local` by default):

```bash
sudo make install
```

You can customize the install prefix:
```bash
sudo make install PREFIX=/usr
```

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

### Configuration File

You can create a `cnotify.conf` file in the current directory to set default options. CLI arguments override these settings.

Example `cnotify.conf`:
```ini
# cnotify configuration
build=go build -o myapp
bin=./myapp
path=.
exclude=.git,node_modules,tmp
verbose=true
```

### Examples

**Hot-reload a Go application:**

```bash
cnotify -path . -build "go build -o app" -bin "./app"
```

**Hot-reload a Python script:**

```bash
cnotify -path src -bin "python3 src/main.py"
```

**Hot-reload a shell script (for testing):**

```bash
cnotify -path . -bin "./myscript.sh"
```

## Usage (Library)

To use `cnotify` in your own C programs:

1.  Include `cnotify.h`.
2.  Link against `libcnotify`.

```c
#include <cnotify.h>
#include <stdio.h>

int my_callback(const cnotify_event_t *event, void *user_data) {
    printf("Event: %s (Type: %d)\n", event->name, event->type);
    return 0; // Return 1 to stop the loop
}

int main() {
    cnotify_t *cn = cnotify_init();
    cnotify_add_watch(cn, ".", NULL);
    cnotify_start_loop(cn, my_callback, NULL);
    cnotify_destroy(cn);
    return 0;
}
```

Compile with:
```bash
gcc main.c -o main -lcnotify
```
