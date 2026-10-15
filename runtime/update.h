// Updates of an installed app, through Velopack (github.com/velopack/velopack) and the game's
// GitHub releases (gcn_add_game's UPDATE_URL). In a build without UPDATE_URL -- a development
// build -- and in an app Velopack did not install, all of this does nothing.
//
// At startup, update_startup() lets Velopack run its own hooks (an install, an uninstall) and
// applies an update downloaded by an earlier run, starting the new version in place of this
// one. The launcher then calls update_check_start(): a thread looks for a newer release and
// downloads it, to be applied at the next start. GCN_UPDATE_URL=<url or folder> looks
// elsewhere than the game's releases, for a test.
#pragma once
#include <string>

void update_startup();
void update_check_start();

struct UpdateStatus {
    enum State { NONE, CHECKING, LATEST, DOWNLOADING, READY, FAILED } state = NONE;
    std::string current;   // this version
    std::string next;      // the newer one, once found
    int percent = 0;       // of its download
};
// Where the check is, for the launcher: NONE outside an installed app.
UpdateStatus update_status();
