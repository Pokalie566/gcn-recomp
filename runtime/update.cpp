// Updates of an installed app. See update.h.
#include "update.h"
#include <mutex>

#ifndef GCN_UPDATE_URL

void update_startup() {}
void update_check_start() {}
UpdateStatus update_status() { return {}; }

#else
#include <Velopack.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
std::mutex g_mutex;
UpdateStatus g_status;

void set(UpdateStatus::State state, int percent = 0) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_status.state = state;
    g_status.percent = percent;
}

// Velopack's messages go to the log with the rest (crash_log.h), but not its tracing.
void log(void*, const char* level, const char* message) {
    if (strcmp(level, "trace") && strcmp(level, "debug")) fprintf(stderr, "[update] %s: %s\n", level, message);
}
}

void update_startup() {
    try {
        Velopack::VelopackApp::Build().SetLogger(log, nullptr).Run();
    } catch (const std::exception& e) {
        fprintf(stderr, "[update] %s\n", e.what());
    }
}

void update_check_start() {
    std::thread([] {
        try {
            // Throws in an app Velopack did not install, and while the releases cannot be read
            // (offline, or a private repository).
            const char* test = getenv("GCN_UPDATE_URL");
            Velopack::UpdateManager manager = test ? Velopack::UpdateManager(test)
                                                   : Velopack::UpdateManager(std::make_unique<Velopack::GithubSource>(GCN_UPDATE_URL));
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                g_status.current = manager.GetCurrentVersion();
                g_status.state = UpdateStatus::CHECKING;
            }
            const std::optional<Velopack::UpdateInfo> update = manager.CheckForUpdates();
            if (!update) return set(UpdateStatus::LATEST);
            {
                std::lock_guard<std::mutex> lk(g_mutex);
                g_status.next = update->TargetFullRelease.Version;
            }
            set(UpdateStatus::DOWNLOADING);
            manager.DownloadUpdates(*update, [](void*, size_t percent) { set(UpdateStatus::DOWNLOADING, (int)percent); });
            set(UpdateStatus::READY);   // applied by update_startup, next time
            fprintf(stderr, "[update] version %s downloaded: it starts next time\n", update->TargetFullRelease.Version.c_str());
        } catch (const std::exception& e) {
            fprintf(stderr, "[update] %s\n", e.what());
            std::lock_guard<std::mutex> lk(g_mutex);
            g_status.state = g_status.current.empty() ? UpdateStatus::NONE : UpdateStatus::FAILED;
        }
    }).detach();
}

UpdateStatus update_status() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_status;
}
#endif
