// The system's own dialog for choosing a file: a disc image for the launcher's "Browse", a
// save to import. Modal: it returns once the player has chosen (the path) or cancelled (empty).
#pragma once
#include <string>
#include <vector>

// A file with one of `exts` (no dot: "gci").
std::string ui_pick_file(const std::string& title, const std::vector<std::string>& exts);

// A disc image, in any format the disc reader takes (disc.h).
inline std::string ui_pick_disc(const std::string& title) {
    return ui_pick_file(title, {"iso", "ciso", "gcz", "rvz", "wia", "wbfs", "nfs", "tgc"});
}
