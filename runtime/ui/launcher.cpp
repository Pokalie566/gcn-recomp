// The launcher. See launcher.h.
#include "ui/launcher.h"
#include "game_module.h"
#include "gx/gl.h"
#include "gx/texture_pack.h"
#include "platform.h"
#include "prepare.h"
#include "runtime.h"
#include "saves.h"
#include "ui/pick_file.h"
#include "versions.h"
#include "ui/ui.h"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace {

constexpr int kKeepCopies = 10;

std::string card_path() { return g_memcard_path ? std::string(g_memcard_path) : save_path("memcard_a.raw"); }
std::string copies_dir() { return save_path("backups"); }

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

std::string latin1(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && p[i]; i++) {
        if (p[i] == '\n') s += ' ';
        else if (p[i] < 0x80) s += (char)p[i];
        else { s += (char)(0xC0 | p[i] >> 6); s += (char)(0x80 | (p[i] & 0x3F)); }
    }
    return s;
}

// The disc's banner, opening.bnr in its file system: a 96x32 picture and the game's name and
// description, as a GameCube menu shows them. The picture is written as a TGA, which is what
// RmlUi's renderer reads. False if the disc has none; it is not needed for anything.
bool read_banner(const std::string& tga, std::string* title, std::string* desc) {
    uint8_t hdr[0x430];
    if (!iso_read(0, hdr, sizeof(hdr))) return false;
    const uint32_t fst_off = be32(hdr + 0x424), fst_size = be32(hdr + 0x428);
    if (!fst_size || fst_size > (16u << 20)) return false;
    std::vector<uint8_t> fst(fst_size);
    if (!iso_read(fst_off, fst.data(), fst_size)) return false;
    const uint32_t n = be32(&fst[8]);
    if ((uint64_t)n * 12 > fst_size) return false;
    const char* names = (const char*)&fst[n * 12];
    uint32_t off = 0, len = 0;
    for (uint32_t i = 1; i < n; i++) {
        const uint8_t* e = &fst[i * 12];
        const uint32_t name = be32(e) & 0xFFFFFF;
        if (!e[0] && n * 12 + name < fst_size && !strcmp(names + name, "opening.bnr")) {
            off = be32(e + 4);
            len = be32(e + 8);
            break;
        }
    }
    std::vector<uint8_t> bnr(0x1960);
    if (len < bnr.size() || !iso_read(off, bnr.data(), (uint32_t)bnr.size()) || memcmp(bnr.data(), "BNR", 3)) return false;
    // 96x32 RGB5A3 in 4x4 tiles, to BGRA rows top to bottom.
    std::vector<uint8_t> px(96 * 32 * 4);
    const uint8_t* src = &bnr[0x20];
    for (int ty = 0; ty < 8; ty++)
        for (int tx = 0; tx < 24; tx++)
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++, src += 2) {
                    const uint16_t v = (uint16_t)(src[0] << 8 | src[1]);
                    int r, g, b, a;
                    if (v & 0x8000) {
                        r = (v >> 10 & 31) * 255 / 31; g = (v >> 5 & 31) * 255 / 31; b = (v & 31) * 255 / 31; a = 255;
                    } else {
                        a = (v >> 12 & 7) * 255 / 7; r = (v >> 8 & 15) * 17; g = (v >> 4 & 15) * 17; b = (v & 15) * 17;
                    }
                    uint8_t* d = &px[((ty * 4 + y) * 96 + tx * 4 + x) * 4];
                    d[0] = (uint8_t)b; d[1] = (uint8_t)g; d[2] = (uint8_t)r; d[3] = (uint8_t)a;
                }
    const uint8_t tga_hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 96, 0, 32, 0, 32, 0x28};
    std::ofstream out(tga, std::ios::binary);
    out.write((const char*)tga_hdr, sizeof(tga_hdr));
    out.write((const char*)px.data(), (std::streamsize)px.size());
    const uint8_t* info = &bnr[0x1820];
    *title = latin1(info + 0x40, 0x40);
    if (title->empty()) *title = latin1(info, 0x20);
    *desc = latin1(info + 0xC0, 0x80);
    return (bool)out;
}

// The preparation, on its own thread: what it is at, for the page to show.
struct Preparation {
    std::mutex m;
    std::thread thread;
    bool done = false, ok = false;
    float progress = 0;
    std::string step, error, copy;
};
Preparation g_prep;

UiLauncher g_l;
std::string g_module;    // the prepared game, once there is one
std::string g_failed;    // why the last preparation failed

void check(const LauncherEnv& env, const Settings& cur) {
    UiLauncher l;
    l.preparing = g_l.preparing;
    l.progress = g_l.progress;
    l.progress_text = g_l.progress_text;
    const char* iso = cur.iso.c_str();
    l.no_disc = cur.iso.empty() || !plat_readable(iso);
    std::string sha1, err;
    bool disc_ok = false;
    if (l.no_disc) {
        l.checks.push_back({"Disc", "fail", cur.iso.empty() ? ui_tr("None yet: drop your disc image on this window")
                                                             : ui_trf("Cannot read {}", cur.iso)});
    } else {
        l.checks.push_back({"Disc", "ok", cur.iso.substr(cur.iso.find_last_of("/\\") + 1)});
        DiscVerdict v;
        disc_ok = prepare_check_disc(iso, &sha1, &err, &v);
        l.checks.push_back(disc_ok ? UiCheck{"Game", "ok", ui_trf("{}, the version this port is made for", v.arg)}
                                   : UiCheck{"Game", "fail", ui_trf(v.message, v.arg)});
        if (disc_ok) {
            const std::string tga = save_path("banner-" + sha1.substr(0, 12) + ".tga");
            plat_make_dirs(save_path(""));
            std::string title, desc;
            if (read_banner(tga, &title, &desc)) {
                l.banner = tga;
                if (!title.empty()) l.title = title;
                l.description = desc;
            }
        }
    }
    g_module = disc_ok ? game_module_find(prepare_module_path(sha1)) : "";
    if (disc_ok) {
        if (l.preparing) l.checks.push_back({"Prepared", "busy", l.progress_text});
        else if (!g_module.empty()) l.checks.push_back({"Prepared", "ok", ui_tr("Ready to play")});
        // What the whole image's SHA-1, read while preparing, says about the dump: information only.
        std::string id, iso_sha1;
        if (!g_module.empty() && (std::ifstream(fs::path(g_module).parent_path() / "disc.txt") >> id >> iso_sha1)) {
            const DiscVerdict d = versions_dump_info(prepare_versions(), id, iso_sha1);
            l.checks.push_back({"Dump", "ok", ui_trf(d.message, d.arg)});
        }
        else if (!g_failed.empty()) l.checks.push_back({"Prepared", "fail", g_failed});
        else l.checks.push_back({"Prepared", "warn", ui_tr("Not yet: Prepare builds the game from your disc, once")});
    }
    std::string missing;
    const bool tools = prepare_tools_present(&missing);
    if (tools) l.checks.push_back({"Tools", "ok", ui_tr("A C compiler and python3, to prepare the game")});
    else if (!g_module.empty()) l.checks.push_back({"Tools", "warn", ui_trf("Missing ({}), not needed now", ui_tr(missing))});
    else l.checks.push_back({"Tools", "fail", ui_trf("Missing: {}, needed to prepare the game", ui_tr(missing))});
    l.checks.push_back({"Graphics", "ok", env.gl});
    l.checks.push_back(env.audio ? UiCheck{"Sound", "ok", ui_tr("Ready")} : UiCheck{"Sound", "warn", ui_tr("No sound device")});
    int pads = 0;
    std::string pad_name;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && !pads++) pad_name = SDL_GameControllerNameForIndex(i) ? SDL_GameControllerNameForIndex(i) : "Controller";
    l.checks.push_back(pads ? UiCheck{"Controller", "ok", pad_name}
                            : UiCheck{"Controller", "warn", ui_tr("None: the keyboard plays (Settings, Controls)")});
    size_t saves = 0;
    for (const CardFile& f : card_files(card_path())) saves += f.game == GCN_DISC_ID;
    l.checks.push_back({"Memory card", "ok", !plat_readable(card_path().c_str()) ? ui_tr("New, made at the first save")
                                             : saves == 0 ? ui_tr("No save yet") : saves == 1 ? ui_tr("1 save") : ui_trf("{} saves", std::to_string(saves))});
    const size_t textures = gx::texture_pack_count();
    if (!textures) l.checks.push_back({"Textures", "ok", ui_tr("The game's own (no pack in textures/)")});
    else l.checks.push_back({"Textures", "ok", ui_trf("{} in the pack", std::to_string(textures)) + (cur.textures ? "" : ui_tr(", turned off in Settings"))});
    l.disc_ok = disc_ok;
    l.can_play = disc_ok && !g_module.empty() && !l.preparing;
    // The Disc settings page: the disc's check, and the game's when there is a disc.
    const UiCheck& disc = l.checks[0];
    const UiCheck* game = l.checks.size() > 1 && l.checks[1].label == "Game" ? &l.checks[1] : nullptr;
    ui_set_disc(l.no_disc && cur.iso.empty() ? ui_tr("No disc yet") : cur.iso.substr(cur.iso.find_last_of("/\\") + 1),
                game ? game->state : disc.state, game ? game->detail : disc.detail);
    l.can_prepare = disc_ok && g_module.empty() && tools && !l.preparing;
#ifdef __APPLE__
    l.can_install_tools = !tools && g_module.empty();
#endif
    g_l = l;
    ui_set_launcher(g_l);
}

// The disc is the preparation's while it runs: nothing else reads it until it is done.
void prepare_start(const std::string& iso) {
    g_prep.done = false;
    g_prep.thread = std::thread([iso] {
        std::string module, copy, err;
        const bool ok = prepare_game(iso.c_str(), &module, &copy, &err, [](float f, const std::string& what) {
            std::lock_guard<std::mutex> lk(g_prep.m);
            g_prep.progress = f;
            g_prep.step = what;
        });
        std::lock_guard<std::mutex> lk(g_prep.m);
        g_prep.ok = ok;
        g_prep.error = err;
        g_prep.copy = copy;
        g_prep.done = true;
    });
}

}  // namespace

void saves_update(const std::string& message) {
    UiSaves s;
    for (const CardFile& f : card_files(card_path()))
        if (f.game == GCN_DISC_ID) s.files.push_back(f);
    s.backups = card_backups(copies_dir());
    s.message = message;
    ui_set_saves(s);
}

bool saves_act(int action, int index) {
    switch (action) {
    case UI_BACKUP:
        saves_update(ui_tr(card_backup(card_path(), copies_dir(), kKeepCopies) ? "Copy made."
                     : plat_readable(card_path().c_str()) ? "The newest copy is already the same as the card."
                                                          : "No card yet: it is made at the first save."));
        return true;
    case UI_RESTORE: {
        const auto copies = card_backups(copies_dir());
        if (index < 0 || index >= (int)copies.size()) return true;
        saves_update(card_restore(copies[index].path, card_path(), copies_dir(), kKeepCopies)
                         ? ui_trf("Restored the copy of {}. The card as it was is now a copy.", copies[index].when)
                         : ui_tr("Could not restore that copy."));
        return true;
    }
    case UI_OPEN_SAVES:
        plat_open_folder(save_path(""));
        return true;
    default:
        return false;
    }
}

void saves_backup_on_start() { card_backup(card_path(), copies_dir(), kKeepCopies); }

void settings_pages_update(const Settings& cur, bool in_game) {
    if (in_game) {
        const bool ok = plat_readable(cur.iso.c_str());
        ui_set_disc(cur.iso.substr(cur.iso.find_last_of("/\\") + 1), ok ? "ok" : "fail", cur.iso);
    }
    std::vector<UiPack> packs;
    for (const gx::TexturePack& p : gx::texture_pack_list()) packs.push_back({p.name, p.textures});
    ui_set_packs(packs);
}

bool launcher_run(LauncherEnv& env, Settings& cur) {
    check(env, cur);
    if (cur.setup_done) ui_show_launcher(cur, g_l);
    else ui_show_setup(cur, g_l);
    saves_update();
    settings_pages_update(cur, false);
    // A disc chosen, dropped on the window or picked: the one to play from now on.
    auto set_disc = [&](const std::string& path) {
        if (g_l.preparing) return;
        Settings s = cur;
        s.iso = path;
        env.commit(s);
        cur = s;
        ui_set_settings(cur);   // or the menus, still on the old disc, would put it back
        g_failed.clear();
        check(env, cur);
    };
    auto start_preparing = [&] {
        g_failed.clear();
        g_l.preparing = true;
        g_l.progress = 0;
        g_l.progress_text = ui_trf("Preparing: {}", ui_tr("starting"));
        check(env, cur);
        prepare_start(cur.iso);
    };
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                if (g_prep.thread.joinable()) g_prep.thread.detach();
                return false;
            }
            const bool scripted_drop = e.type == SDL_USEREVENT && e.user.code == kUiDropEvent;
            if (e.type == SDL_DROPFILE || scripted_drop) {
                char* file = scripted_drop ? (char*)e.user.data1 : e.drop.file;
                set_disc(file);
                SDL_free(file);
                continue;
            }
            if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) {
                env.event(e);
                if (!g_l.preparing) check(env, cur);
                continue;
            }
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                // Back from the file browser, a pack copied in maybe: look again.
                gx::texture_pack_reset();
                settings_pages_update(cur, false);
            }
            if (ui_event(e)) continue;
            env.event(e);
        }
        ui_set_pad(env.pad(), env.pad_name());

        UiAction a = ui_take_action();
        const int index = ui_action_index();
        if (a == UI_QUIT) {
            if (g_prep.thread.joinable()) g_prep.thread.detach();
            return false;
        }
        if (a == UI_SETUP_DONE) {
            Settings s = cur;
            s.setup_done = true;
            env.commit(s);
            cur = s;
            ui_set_settings(cur);
            if (index == 1 && g_l.can_play) a = UI_PLAY;
            else ui_show_launcher(cur, g_l);
        }
        if (a == UI_PLAY) {
            std::string err;
            if (game_module_load(g_module, &err)) {
                saves_backup_on_start();
                ui_hide();
                return true;
            }
            g_failed = "Cannot load the prepared game: " + err;
            g_module.clear();
            check(env, cur);
        } else if (a == UI_PREPARE || (a == UI_PREPARE_AGAIN && g_l.disc_ok && !g_l.preparing)) {
            start_preparing();
        } else if (a == UI_PICK_DISC) {
            const std::string path = ui_pick_disc(ui_tr("Choose your disc image"));
            if (!path.empty()) set_disc(path);
        } else if (a == UI_TEST_RUMBLE) {
            env.rumble();
        } else if (a == UI_OPEN_TEXTURES) {
            plat_open_folder(gx::texture_pack_dir());
        } else if (a == UI_SETUP_AGAIN) {
            ui_show_setup(cur, g_l);
        } else if (a == UI_INSTALL_TOOLS) {
            plat_run({"xcode-select", "--install"}, kPlatNull);
        } else if (saves_act(a, index)) {
            if (a == UI_RESTORE && !g_l.preparing) check(env, cur);
        }

        Settings s = cur;
        if (ui_take_settings(s)) {
            env.commit(s);
            cur = s;
        }

        if (g_l.preparing) {
            bool done;
            {
                std::lock_guard<std::mutex> lk(g_prep.m);
                done = g_prep.done;
                g_l.progress = g_prep.progress;
                const std::string step = g_prep.step.compare(0, 10, "compiling ") ? ui_tr(g_prep.step) : ui_trf("compiling {}", g_prep.step.substr(10));
                g_l.progress_text = ui_trf("Preparing: {}", step) + " (" + std::to_string((int)(g_prep.progress * 100)) + "%)";
            }
            if (done) {
                g_prep.thread.join();
                g_l.preparing = false;
                if (!g_prep.ok) g_failed = g_prep.error;
                if (g_prep.ok && g_prep.copy != cur.iso) set_disc(g_prep.copy);   // the player's image is no longer needed
                else check(env, cur);
                // The Prepare button is gone: the focus goes to Play, or the assistant's Next.
                if (!ui_setup_open()) ui_show_launcher(cur, g_l);
            } else {
                for (UiCheck& c : g_l.checks)
                    if (c.label == "Prepared") c.detail = g_l.progress_text;
                ui_set_launcher(g_l);
            }
        }

        int w, h;
        SDL_GL_GetDrawableSize(env.win, &w, &h);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, w, h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ui_render(w, h);
        if (!g_l.preparing) ui_script(w, h);
        SDL_GL_SwapWindow(env.win);
        SDL_Delay(12);
    }
}
