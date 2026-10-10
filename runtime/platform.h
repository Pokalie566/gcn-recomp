// Host OS services that differ between POSIX and Windows. Implemented in platform.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ---- virtual memory ----
// Reserve `size` bytes of address space without committing it. Returns nullptr on failure.
void* plat_reserve(size_t size);
// Make [base, base+size) readable and writable within an existing reservation.
bool plat_commit(void* base, size_t size);

// ---- diagnostics ----
// Print a symbolized backtrace of the calling thread to stderr.
void plat_backtrace_print();
// Install handlers for interactive interrupt and for memory faults. `on_fault` is
// passed the faulting address (0 if unknown); both callbacks must not return.
void plat_install_crash_handlers(void (*on_interrupt)(), void (*on_fault)(const void* addr, int sig));
// Hard-exit after `seconds` no matter what, so a crash handler can never hang.
void plat_watchdog(int seconds, int exit_code);
// Exit immediately without running destructors or flushing other threads' buffers.
[[noreturn]] void plat_exit_now(int code);

// ---- threads ----
// Start a thread with a `stack_size`-byte stack (reserved, not committed).
bool plat_thread_start(void* (*fn)(void*), void* arg, size_t stack_size);
// Terminate the calling thread. Used when the process is shutting down, so skipping
// destructor/unwind cleanup is intentional.
[[noreturn]] void plat_thread_exit();

// ---- time ----
// Seconds to add to UTC to get local wall-clock time, including any DST in effect.
long plat_utc_offset_seconds();

// ---- files ----
bool plat_readable(const char* path);
// Create `path` and any missing parents. Ignores already-exists.
void plat_make_dirs(const std::string& path);
// Replace `to` with `from`, overwriting an existing file.
bool plat_replace_file(const char* from, const char* to);
// First entry in `dir` whose name ends with `suffix` (case-insensitive), or "".
std::string plat_find_file(const char* dir, const char* suffix);


// The directory of the running executable, without a trailing separator.
std::string plat_executable_path();
std::string plat_executable_dir();
// Starts this program again in place of this process, with these arguments (null-ended).
// Returns only if it could not.
void plat_restart(char** argv);
// Starts `argv` and leaves it running, this process not waiting for it (nor it for this one).
void plat_spawn_detached(const std::vector<std::string>& argv);
// Shows a directory in the system's file browser, creating it first.
void plat_open_folder(const std::string& dir);

// A shared library loaded at run time -- the recompiled game, see game_module.h. Null and a
// reason in `err` if it cannot be loaded.
void* plat_module_open(const char* path, std::string* err);
void* plat_module_symbol(void* module, const char* name);

// Runs a program and waits for it. argv[0] is looked up in PATH; its output is appended to
// `log` when one is given. Returns its exit status, or -1 if it could not be started.
int plat_run(const std::vector<std::string>& argv, const char* log);
// The log for a run whose output is of no interest.
#ifdef _WIN32
constexpr const char* kPlatNull = "NUL";
#else
constexpr const char* kPlatNull = "/dev/null";
#endif
