// The log and the crash message. See crash_log.h.
#include "crash_log.h"
#include "platform.h"
#include "runtime.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#ifdef _WIN32
#include <io.h>
#define pipe(fds) _pipe(fds, 4096, 0x8000 /* _O_BINARY */)
#define dup _dup
#define dup2 _dup2
#define read _read
#define write _write
#define close _close
#define fileno _fileno
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {
std::string g_log_path;
// The dialog's sentences in the player's language (crash_set_translator); else English.
std::string untranslated(const std::string& english, const std::string& arg) {
    const size_t at = english.find("{}");
    return at == std::string::npos ? english : english.substr(0, at) + arg + english.substr(at + 2);
}
std::string (*g_trf)(const std::string&, const std::string&) = untranslated;
FILE* g_log;
int g_console = -1;   // stderr as it was: the terminal, if any
int g_stderr = 2;     // stderr's descriptor
}

void log_start(const std::string& path) {
    std::error_code ec;
    const fs::path p(path);
    if (fs::exists(p, ec)) fs::rename(p, fs::path(p).replace_extension(".old.log"), ec);
    g_log = fopen(path.c_str(), "wb");
    if (!g_log) {
        fprintf(stderr, "log: cannot write %s\n", path.c_str());
        return;
    }
    g_log_path = path;
    // stderr into a pipe; a thread hands what comes out to the terminal and the file. A
    // Windows app has no console, and its stderr no descriptor until it is given one.
    if (fileno(stderr) < 0) (void)!freopen("NUL", "w", stderr);
    g_stderr = fileno(stderr);
    int fds[2];
    fflush(stderr);
    if (pipe(fds) != 0) return;
    g_console = dup(g_stderr);
    dup2(fds[1], g_stderr);
    close(fds[1]);
    setvbuf(stderr, nullptr, _IONBF, 0);
    std::thread([rd = fds[0]] {
        char buf[4096];
        for (int n; (n = (int)read(rd, buf, sizeof(buf))) > 0;) {
            if (g_console >= 0) (void)!write(g_console, buf, (unsigned)n);
            fwrite(buf, 1, (size_t)n, g_log);
            fflush(g_log);
        }
    }).detach();
}

void crash_set_translator(std::string (*trf)(const std::string& english, const std::string& arg)) { g_trf = trf; }

void crash_report(const char* what) {
    static bool once;
    if (once) return;
    once = true;
    // What is still in the pipe, then the summary, written straight to both.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    char line[1200];
    snprintf(line, sizeof(line), "\nCRASH: %s\nThe log: %s\n", what, g_log_path.c_str());
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    if (g_console >= 0) (void)!write(g_console, line, (unsigned)strlen(line));
    const char* dialog = getenv("GCN_CRASH_DIALOG");
    if (dialog && !strcmp(dialog, "0")) return;
    if (g_console >= 0) dup2(g_console, g_stderr);   // the dialog's process gets the terminal, not a pipe about to close
    const std::string exe = plat_executable_path();
    const std::string text = g_trf("The game stopped: {}", what) + "\n\n" +
                             (g_log_path.empty() ? g_trf("There is no log.", "")
                                                 : g_trf("What happened is in the log, which the next start keeps as the old one: {}", g_log_path));
    plat_spawn_detached({exe, "--crash-report=" + text});
}

bool crash_report_main(int argc, char** argv) {
    std::string text;
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "--crash-report=", 15)) text = argv[i] + 15;
    if (text.empty()) return false;
    const char* dialog = getenv("GCN_CRASH_DIALOG");
    if (dialog && !strcmp(dialog, "print")) printf("[crash dialog] %s\n", text.c_str());
    else SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GCN_GAME_TITLE " (recompiled)", text.c_str(), nullptr);
    return true;
}
