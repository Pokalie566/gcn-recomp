// The disc image picker on Windows (the system's Open dialog) and Linux (zenity or kdialog,
// whichever is there). macOS's is pick_file_mac.mm. See pick_file.h.
#include "ui/pick_file.h"
#include "platform.h"
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#else
#include <filesystem>
#include <fstream>
#endif

#if defined(_WIN32)
std::string ui_pick_disc(const std::string& title) {
    char path[MAX_PATH] = "";
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "GameCube disc image (*.iso, *.ciso)\0*.iso;*.ciso\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? std::string(path) : std::string();
}
#elif !defined(__APPLE__)
std::string ui_pick_disc(const std::string& title) {
    const std::string out = (std::filesystem::temp_directory_path() / "gcn_pick.txt").string();
    for (const std::vector<std::string>& cmd : {
             std::vector<std::string>{"zenity", "--file-selection", "--title=" + title,
                                      "--file-filter=Disc images | *.iso *.ciso *.ISO *.CISO"},
             std::vector<std::string>{"kdialog", "--getopenfilename", ".", "*.iso *.ciso", "--title", title}}) {
        std::error_code ec;
        std::filesystem::remove(out, ec);
        const int rc = plat_run(cmd, out.c_str());
        if (rc == -1 || rc == 127) continue;   // not installed: the next one
        std::ifstream in(out);
        std::string path;
        std::getline(in, path);
        return rc == 0 ? path : std::string();
    }
    return {};
}
#endif
