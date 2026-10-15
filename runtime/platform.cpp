// Host OS services: POSIX and Windows implementations of platform.h.
#include "platform.h"
#include <cctype>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

// ===========================================================================
#ifdef _WIN32
// ===========================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <io.h>
#include <process.h>

void* plat_reserve(size_t size) {
    return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
}

bool plat_commit(void* base, size_t size) {
    return VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

// ---- diagnostics ----
static bool g_sym_ready;

void plat_backtrace_print() {
    HANDLE proc = GetCurrentProcess();
    if (!g_sym_ready) {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        SymInitialize(proc, nullptr, TRUE);
        g_sym_ready = true;
    }
    void* frames[64];
    USHORT n = CaptureStackBackTrace(1, 64, frames, nullptr);
    // SymFromAddr wants a SYMBOL_INFO with room for the name after it.
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    for (USHORT i = 0; i < n; i++) {
        DWORD64 addr = (DWORD64)(uintptr_t)frames[i];
        memset(sym, 0, sizeof(SYMBOL_INFO));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = MAX_SYM_NAME;
        DWORD64 disp = 0;
        const char* name = SymFromAddr(proc, addr, &disp, sym) ? sym->Name : "?";

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line))
            fprintf(stderr, "  %2u %p %s (%s:%lu)\n", i, frames[i], name, line.FileName, line.LineNumber);
        else
            fprintf(stderr, "  %2u %p %s\n", i, frames[i], name);
    }
    fflush(stderr);
}

void plat_watchdog(int seconds, int exit_code) {
    std::thread([seconds, exit_code] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        _exit(exit_code);
    }).detach();
}

void plat_exit_now(int code) { _exit(code); }

static void (*g_on_interrupt)();
static void (*g_on_fault)(const void*, int);

static BOOL WINAPI console_ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_on_interrupt) g_on_interrupt();
        return TRUE;
    }
    return FALSE;
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    const void* addr = nullptr;
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        ep->ExceptionRecord->NumberParameters >= 2)
        addr = (const void*)ep->ExceptionRecord->ExceptionInformation[1];
    if (g_on_fault) g_on_fault(addr, (int)code);
    return EXCEPTION_EXECUTE_HANDLER;
}

void plat_install_crash_handlers(void (*on_interrupt)(), void (*on_fault)(const void*, int)) {
    g_on_interrupt = on_interrupt;
    g_on_fault = on_fault;
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    SetUnhandledExceptionFilter(unhandled_filter);
    // Don't pop the "program stopped working" dialog in a non-interactive run.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

// ---- threads ----
struct ThreadStart { void* (*fn)(void*); void* arg; };

static DWORD WINAPI thread_trampoline(void* p) {
    ThreadStart ts = *(ThreadStart*)p;
    delete (ThreadStart*)p;
    ts.fn(ts.arg);
    return 0;
}

bool plat_thread_start(void* (*fn)(void*), void* arg, size_t stack_size) {
    auto* ts = new ThreadStart{fn, arg};
    // STACK_SIZE_PARAM_IS_A_RESERVATION keeps the large stack reserved rather than
    // committed, so each guest thread costs address space and not RAM.
    HANDLE h = CreateThread(nullptr, stack_size, thread_trampoline, ts,
                            STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) { delete ts; return false; }
    CloseHandle(h);
    return true;
}

void plat_thread_exit() {
    ExitThread(0);
    for (;;) {}  // not reached; keeps [[noreturn]] honest under MSVC
}

// ---- time ----
long plat_utc_offset_seconds() {
    time_t t = time(nullptr);
    struct tm lt;
    if (localtime_s(&lt, &t) != 0) return 0;
    // Re-read the local wall clock as if it were UTC: the difference is the offset,
    // with whatever DST was in effect already folded in by localtime_s.
    time_t as_utc = _mkgmtime(&lt);
    if (as_utc == (time_t)-1) return 0;
    return (long)(as_utc - t);
}

// ---- files ----
bool plat_readable(const char* path) { return path && *path && _access(path, 4) == 0; }

// ---- modules and processes ----
std::string plat_executable_path() {
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::string(buf, n);
}

void plat_restart(char** argv) {
    _execv(plat_executable_path().c_str(), argv);
}

static std::string quote_arg(const std::string& a);

void plat_spawn_detached(const std::vector<std::string>& argv) {
    std::string cmd;
    for (const auto& a : argv) cmd += (cmd.empty() ? "" : " ") + quote_arg(a);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void plat_open_folder(const std::string& dir) {
    plat_make_dirs(dir);
    plat_run({"explorer", dir}, nullptr);
}

void* plat_module_open(const char* path, std::string* err) {
    // A module that cannot load is an error to report, not a dialog box to wait on.
    DWORD old_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode);
    HMODULE m = LoadLibraryA(path);
    SetThreadErrorMode(old_mode, nullptr);
    if (!m && err) *err = "LoadLibrary failed (" + std::to_string(GetLastError()) + ")";
    return (void*)m;
}

void* plat_module_symbol(void* module, const char* name) {
    return (void*)GetProcAddress((HMODULE)module, name);
}

// One argument as CreateProcess's command line wants it: quoted, its quotes and the
// backslashes before them escaped, as the C runtime splits a command line again.
static std::string quote_arg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string q = "\"";
    size_t slashes = 0;
    for (char c : a) {
        if (c == '\\') { slashes++; continue; }
        q.append(c == '"' ? slashes * 2 + 1 : slashes, '\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, '\\');
    return q + "\"";
}

int plat_run(const std::vector<std::string>& argv, const char* log) {
    std::string cmd;
    for (const auto& a : argv) cmd += (cmd.empty() ? "" : " ") + quote_arg(a);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = INVALID_HANDLE_VALUE;
    if (log) {
        out = CreateFileA(log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    // Nothing to read: a tool that asks for input gets none rather than waiting for ever.
    HANDLE in = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = si.hStdError = out != INVALID_HANDLE_VALUE ? out : GetStdHandle(STD_OUTPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

// ===========================================================================
#else  // POSIX
// ===========================================================================
#include <csignal>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <execinfo.h>

void* plat_reserve(size_t size) {
    void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

bool plat_commit(void* base, size_t size) {
    return mprotect(base, size, PROT_READ | PROT_WRITE) == 0;
}

void plat_backtrace_print() {
    void* bt[64];
    int n = backtrace(bt, 64);
    backtrace_symbols_fd(bt, n, 2);
}

void plat_watchdog(int seconds, int exit_code) {
    static int s_code;
    s_code = exit_code;
    signal(SIGALRM, [](int) { _exit(s_code); });
    alarm((unsigned)seconds);
}

void plat_exit_now(int code) { _exit(code); }

static void (*g_on_interrupt)();
static void (*g_on_fault)(const void*, int);

static void sig_interrupt(int) { if (g_on_interrupt) g_on_interrupt(); }
static void sig_fault(int sig, siginfo_t* si, void*) {
    if (g_on_fault) g_on_fault(si ? si->si_addr : nullptr, sig);
}

void plat_install_crash_handlers(void (*on_interrupt)(), void (*on_fault)(const void*, int)) {
    g_on_interrupt = on_interrupt;
    g_on_fault = on_fault;
    signal(SIGINT, sig_interrupt);
    signal(SIGTERM, sig_interrupt);
    struct sigaction sa = {};
    sa.sa_sigaction = sig_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

bool plat_thread_start(void* (*fn)(void*), void* arg, size_t stack_size) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size);
    pthread_t th;
    int rc = pthread_create(&th, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

void plat_thread_exit() {
    pthread_exit(nullptr);
    for (;;) {}  // not reached
}

long plat_utc_offset_seconds() {
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    return (long)lt.tm_gmtoff;
}

bool plat_readable(const char* path) { return path && *path && access(path, R_OK) == 0; }

// ---- modules and processes ----
#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;

std::string plat_executable_path() {
    std::string p;
#ifdef __APPLE__
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char real[4096];
        p = realpath(buf, real) ? real : buf;
    }
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) p.assign(buf, (size_t)n);
#endif
    return p;
}

void plat_restart(char** argv) {
    execv(plat_executable_path().c_str(), argv);
}

void plat_spawn_detached(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid;
    posix_spawn(&pid, args[0], nullptr, nullptr, args.data(), environ);
}

void plat_open_folder(const std::string& dir) {
    plat_make_dirs(dir);
#ifdef __APPLE__
    plat_run({"open", dir}, kPlatNull);
#else
    plat_run({"xdg-open", dir}, kPlatNull);
#endif
}

void* plat_module_open(const char* path, std::string* err) {
    void* m = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!m && err) { const char* e = dlerror(); *err = e ? e : "dlopen failed"; }
    return m;
}

void* plat_module_symbol(void* module, const char* name) { return dlsym(module, name); }

int plat_run(const std::vector<std::string>& argv, const char* log) {
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (log) {
        posix_spawn_file_actions_addopen(&fa, 1, log, O_WRONLY | O_CREAT | O_APPEND, 0644);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
    }
    pid_t pid;
    const int rc = posix_spawnp(&pid, args[0], &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#endif

// ===========================================================================
// Shared
// ===========================================================================
std::string plat_executable_dir() {
    const std::string p = plat_executable_path();
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

void plat_make_dirs(const std::string& path) {
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
}

std::string plat_find_file(const char* dir, const char* suffix) {
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec), end;
    if (ec) return {};
    const size_t slen = strlen(suffix);
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string name = it->path().filename().string();
        if (name.size() <= slen) continue;
        std::string tail = name.substr(name.size() - slen);
        bool match = true;
        for (size_t i = 0; i < slen; i++)
            match &= (char)tolower((unsigned char)tail[i]) == (char)tolower((unsigned char)suffix[i]);
        if (match) return it->path().string();
    }
    return {};
}

