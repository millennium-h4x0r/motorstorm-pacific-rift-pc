/* MotorStorm: Pacific Rift port: the port's screens over the game's window,
 * drawn with Dear ImGui through the toolkit's rsx_ui (libs/video/rsx_ui.h).
 * Ported from the Apocalypse port's ui.cpp, trimmed to what applies here:
 *
 *   F1, or Select+Start held half a second   the in-game menu (Escape closes it)
 *   F10                                      the performance HUD: detailed, compact, off
 *   F11, or Alt+Enter                        full screen and back (the backend)
 *
 * While the menu is open the game gets a neutral pad (cellPad.c). Every change
 * is written to user/settings.ini at once (settings.h) and applies now.
 * The resolution scale and frame rate (Video page) are read at start, so they
 * take effect after a restart (the page has a Restart now button); the FSR 1 upscaler and its sharpness apply at once.
 * The Apocalypse start screen, languages, VSync, FPS limit and mods pages are
 * not here: this port's backend has none of those knobs. */
#include "settings.h"
#include "ui.h"

#ifdef PS3RECOMP_UI

#include "imgui.h"
#include "rsx_ui.h"

#include <SDL2/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
extern char** environ;
#endif

extern "C" unsigned ppu_boot_frames_presented(void);       /* main.cpp */
extern "C" unsigned rsx_vulkan_backend_guest_draws(void);  /* draws in the last frame */
extern "C" void rsx_vulkan_backend_set_fsr(int on, float sharpness);
extern "C" int  rsx_vulkan_backend_fsr_state(unsigned* in_w, unsigned* in_h, unsigned* out_w, unsigned* out_h);

namespace {

double g_fps = 0.0, g_fps_t = 0.0;
/* Ported from the Apocalypse port's ui.cpp (k_scales). */
const char* const k_scales[] = { "720p (1x)", "1440p (2x)", "4K (3x)" };
int g_boot_scale = 1;          /* the scale this run renders at (PS3RECOMP_RES_SCALE at start) */
int g_boot_fps = 30;           /* the frame rate this run runs at (PS3RECOMP_FPS at start) */
const char* const k_fps[] = { "30 (original)", "60" };
unsigned g_fps_frames = 0;
bool g_styled = false;

void style()
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = 10.0f; s.FrameRounding = 6.0f; s.TabRounding = 6.0f;
    s.GrabRounding = 6.0f; s.PopupRounding = 6.0f;
    s.WindowPadding = ImVec2(22, 18); s.FramePadding = ImVec2(12, 7);
    s.ItemSpacing = ImVec2(12, 12); s.WindowBorderSize = 0.0f;
    ImVec4* c = s.Colors;
    const ImVec4 accent(1.00f, 0.62f, 0.20f, 1.00f);                 /* volcanic orange */
    c[ImGuiCol_WindowBg]        = ImVec4(0.07f, 0.06f, 0.05f, 0.94f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.09f, 0.08f, 0.07f, 0.98f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.18f, 0.15f, 0.12f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.27f, 0.20f, 0.14f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.33f, 0.23f, 0.15f, 1.00f);
    c[ImGuiCol_Tab]             = ImVec4(0.16f, 0.13f, 0.10f, 1.00f);
    c[ImGuiCol_TabHovered]      = ImVec4(0.55f, 0.32f, 0.12f, 1.00f);
    c[ImGuiCol_TabSelected]     = ImVec4(0.62f, 0.34f, 0.10f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.24f, 0.18f, 0.13f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.55f, 0.32f, 0.12f, 1.00f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.72f, 0.40f, 0.12f, 1.00f);
    c[ImGuiCol_Header]          = ImVec4(0.24f, 0.18f, 0.13f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.55f, 0.32f, 0.12f, 1.00f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.72f, 0.40f, 0.12f, 1.00f);
    c[ImGuiCol_TitleBg]         = ImVec4(0.30f, 0.17f, 0.07f, 1.00f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.45f, 0.24f, 0.08f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]   = ImVec4(0.40f, 0.30f, 0.22f, 1.00f);
    c[ImGuiCol_CheckMark]       = accent;
    c[ImGuiCol_SliderGrab]      = accent;
    c[ImGuiCol_NavHighlight]    = accent;
    c[ImGuiCol_Separator]       = ImVec4(0.33f, 0.26f, 0.20f, 1.00f);
    c[ImGuiCol_Text]            = ImVec4(0.97f, 0.94f, 0.90f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.68f, 0.62f, 0.55f, 1.00f);
}

void save()
{
    if (port_settings_save(port_settings_get(), port_settings_path()))
        fprintf(stderr, "[ui] could not write %s\n", port_settings_path());
}

void hint(const char* t)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("%s", t);
    ImGui::PopStyleColor();
}

void set_fps_counter(int mode)
{
    port_settings& s = *port_settings_get();
    s.fps_counter = mode >= 0 && mode <= 2 ? mode : 0;
    rsx_ui_set_visible(s.fps_counter);
    save();
}

/* Start the game again with the menu's resolution and frame rate: both are
 * read once at start (render targets and shaders are built at the scale; the
 * frame timer's interval is set during boot). Same binary, same arguments,
 * the environment with those two variables replaced by the menu's values, so
 * they win over what the run started with. Progress since the last save is
 * lost. Idea from the Apocalypse port's ui.cpp (play / restart_note). */
#ifndef _WIN32
void restart_now(const port_settings& s)
{
    save();
    std::vector<std::string> args;
    { FILE* f = fopen("/proc/self/cmdline", "rb");
      if (!f) return;
      std::string a; int c;
      while ((c = fgetc(f)) != EOF) { if (c) a += (char)c; else { args.push_back(a); a.clear(); } }
      fclose(f); }
    if (args.empty()) return;
    std::vector<std::string> env;
    for (char** e = environ; *e; e++)
        if (strncmp(*e, "PS3RECOMP_RES_SCALE=", 20) && strncmp(*e, "PS3RECOMP_FPS=", 14)) env.push_back(*e);
    env.push_back("PS3RECOMP_RES_SCALE=" + std::to_string(s.scale));
    env.push_back(std::string("PS3RECOMP_FPS=") + (s.fps == 60 ? "60" : "30"));
    std::vector<char*> av, ev;
    for (auto& a : args) av.push_back(&a[0]);
    for (auto& e : env) ev.push_back(&e[0]);
    av.push_back(nullptr); ev.push_back(nullptr);
    char self[4096];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) return;
    self[n] = 0;
    fprintf(stderr, "[ui] restarting: resolution %dx, %d fps\n", s.scale, s.fps);
    fflush(stderr); fflush(stdout);
    execve(self, av.data(), ev.data());
    fprintf(stderr, "[ui] restart failed\n");
}

void restart_button(const port_settings& s)
{
    if (ImGui::Button("Restart now")) restart_now(s);
    ImGui::SameLine();
    hint("Unsaved race progress is lost.");
}
#else
void restart_button(const port_settings&) {}
#endif

void page_video(port_settings& s)
{
    /* The window as it is: F11 changes it without the menu. */
    bool b = rsx_ui_fullscreen();
    if (ImGui::Checkbox("Fullscreen  (F11, Alt+Enter)", &b)) {
        s.fullscreen = b; rsx_ui_set_fullscreen(b); save();
    }
    /* Render scale: every render target N times the game's 1280x720. */
    int sc = s.scale - 1;
    if (ImGui::Combo("Resolution", &sc, k_scales, 3)) { s.scale = sc + 1; save(); }
    if (s.scale != g_boot_scale)
        hint("Takes effect after a restart.");
    int fi = s.fps == 60 ? 1 : 0;
    if (ImGui::Combo("Frame rate", &fi, k_fps, 2)) { s.fps = fi ? 60 : 30; save(); }
    if (s.fps != g_boot_fps)
        hint("Takes effect after a restart.");
    if (s.scale != g_boot_scale || s.fps != g_boot_fps) restart_button(s);
    /* AMD FSR 1: upscales the presented frame to the window, applies now. */
    bool fsr = s.upscaler != 0;
    if (ImGui::Checkbox("AMD FSR 1 upscaling", &fsr)) {
        s.upscaler = fsr ? 1 : 0;
        rsx_vulkan_backend_set_fsr(s.upscaler, s.fsr_sharpness);
        save();
    }
    if (s.upscaler) {
        float sh = s.fsr_sharpness;
        if (ImGui::SliderFloat("FSR sharpness", &sh, 0.0f, 1.0f, "%.2f")) {
            s.fsr_sharpness = sh;
            rsx_vulkan_backend_set_fsr(1, sh);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) save();
    }
    ImGui::Separator();
    const ImGuiIO& io = ImGui::GetIO();
    unsigned iw = 0, ih = 0, ow = 0, oh = 0;
    const int on = rsx_vulkan_backend_fsr_state(&iw, &ih, &ow, &oh);
    ImGui::Text("Window: %.0f x %.0f", io.DisplaySize.x, io.DisplaySize.y);
    ImGui::Text("Game frame: %u x %u (%dx)", iw, ih, g_boot_scale);
    if (on) ImGui::Text("FSR 1: %u x %u -> %u x %u", iw, ih, ow, oh);
    else if (s.upscaler) hint("FSR 1 is idle: the window is not larger than the game frame.");
    else ImGui::TextUnformatted("Fitted to the window, shape kept");
}

void page_performance(port_settings& s)
{
    static const char* const k_hud[] = { "Off", "Detailed", "Compact" };
    int h = s.fps_counter;
    if (ImGui::Combo("Performance HUD  (F10)", &h, k_hud, 3)) set_fps_counter(h);
    ImGui::Separator();
    ImGui::Text("Frame rate: %.1f fps", g_fps);
    ImGui::Text("Frames presented: %u", ppu_boot_frames_presented());
    ImGui::Text("Draws in the last frame: %u", rsx_vulkan_backend_guest_draws());
}

void page_controller()
{
    SDL_GameController* gc = nullptr;
    /* Under SDL's joystick lock: cellPad polls the same pads from the game's
     * threads (see rsx_ui_frame). */
    SDL_LockJoysticks();
    for (int i = 0; i < SDL_NumJoysticks() && !gc; i++)
        if (SDL_IsGameController(i)) gc = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
    const char* name = gc ? SDL_GameControllerName(gc) : nullptr;
    std::string held;
    if (gc) {
        static const struct { SDL_GameControllerButton b; const char* n; } k[] = {
            { SDL_CONTROLLER_BUTTON_A, "Cross/A" }, { SDL_CONTROLLER_BUTTON_B, "Circle/B" },
            { SDL_CONTROLLER_BUTTON_X, "Square/X" }, { SDL_CONTROLLER_BUTTON_Y, "Triangle/Y" },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "L1" }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, "R1" },
            { SDL_CONTROLLER_BUTTON_BACK, "Select" }, { SDL_CONTROLLER_BUTTON_START, "Start" },
            { SDL_CONTROLLER_BUTTON_LEFTSTICK, "L3" }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, "R3" },
        };
        for (const auto& e : k)
            if (SDL_GameControllerGetButton(gc, e.b)) { held += held.empty() ? "" : "  "; held += e.n; }
    }
    SDL_UnlockJoysticks();
    ImGui::Text("Controller: %s", name ? name : "none found (connect one before starting the game)");
    if (gc) ImGui::Text("Held: %s", held.empty() ? "none" : held.c_str());
    hint("The game sees a neutral pad while this menu is open.");
}

void page_keys()
{
    ImGui::TextUnformatted("F1          this menu (or Select+Start held)");
    ImGui::TextUnformatted("F10         the performance HUD: detailed, compact, off");
    ImGui::TextUnformatted("F11         full screen (or Alt+Enter)");
    ImGui::TextUnformatted("Escape      close this menu; with it closed, quit the game");
}

void draw_menu()
{
    port_settings& s = *port_settings_get();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
    const float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fs * 34.0f < io.DisplaySize.x * 0.92f ? fs * 34.0f : io.DisplaySize.x * 0.92f,
                                    fs * 22.0f < io.DisplaySize.y * 0.9f ? fs * 22.0f : io.DisplaySize.y * 0.9f),
                             ImGuiCond_Always);
    ImGui::Begin("MotorStorm: Pacific Rift  ·  Options", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    if (ImGui::BeginTabBar("pages")) {
        if (ImGui::BeginTabItem("Video")) { page_video(s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Performance")) { page_performance(s); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Controller")) { page_controller(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Keys")) { page_keys(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - fs * 3.2f);
    ImGui::Separator();
    hint("Saved to user/settings.ini.");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - fs * 7.5f);
    if (ImGui::Button("Close  (F1)")) rsx_ui_set_menu_open(0);
    ImGui::End();
}

/* ---- performance HUD ------------------------------------------------------
 * Frame times come from the guest's presented-frame counter, sampled on each
 * UI frame: the time since it last moved, split over the frames it moved by.
 * The last k_ft samples feed the graph and the 1% low / worst figures. */
constexpr int k_ft = 240;
float g_ft[k_ft];               /* ms, ring */
int g_ft_n = 0, g_ft_i = 0;
double g_ft_since = 0.0;        /* s since the counter last moved */
unsigned g_ft_last = 0;
double g_cpu = -1.0;            /* this process, % of one core */

void hud_sample(double dt)
{
    const unsigned f = ppu_boot_frames_presented();
    g_ft_since += dt;
    if (f != g_ft_last) {
        const unsigned d = f - g_ft_last;
        g_ft_last = f;
        if (d < 1000) {
            const float ms = (float)(g_ft_since * 1000.0 / d);
            for (unsigned k = 0; k < d && k < (unsigned)k_ft; k++) {
                g_ft[g_ft_i] = ms; g_ft_i = (g_ft_i + 1) % k_ft;
                if (g_ft_n < k_ft) g_ft_n++;
            }
        }
        g_ft_since = 0.0;
    }
#ifndef _WIN32
    /* CPU time of the whole process (every guest thread) every half second. */
    static double acc = 0.0, last_cpu = -1.0;
    acc += dt;
    if (acc >= 0.5) {
        FILE* fp = fopen("/proc/self/stat", "r");
        if (fp) {
            char buf[1024];
            const size_t n = fread(buf, 1, sizeof buf - 1, fp);
            fclose(fp);
            buf[n] = 0;
            const char* q = strrchr(buf, ')');
            unsigned long ut = 0, st = 0;
            /* fields after the name: state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
            if (q && sscanf(q + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) == 2) {
                const double t = (double)(ut + st) / (double)sysconf(_SC_CLK_TCK);
                if (last_cpu >= 0.0) g_cpu = (t - last_cpu) / acc * 100.0;
                last_cpu = t;
            }
        }
        acc = 0.0;
    }
#endif
}

ImU32 fps_colour(double fps, double target)
{
    if (fps >= target * 0.95) return IM_COL32(120, 230, 120, 255);
    if (fps >= target * 0.75) return IM_COL32(255, 205, 90, 255);
    return IM_COL32(255, 100, 80, 255);
}

void draw_fps_compact()
{
    const ImGuiIO& io = ImGui::GetIO();
    char t[32];
    snprintf(t, sizeof t, "%.0f fps", g_fps);
    const ImVec2 sz = ImGui::CalcTextSize(t);
    const ImVec2 p(io.DisplaySize.x - sz.x - 18.0f, 12.0f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(p.x - 8, p.y - 4), ImVec2(p.x + sz.x + 8, p.y + sz.y + 4), IM_COL32(0, 0, 0, 150), 6.0f);
    dl->AddText(p, fps_colour(g_fps, g_boot_fps), t);
}

void draw_fps_detailed()
{
    const ImGuiIO& io = ImGui::GetIO();
    const float fs = ImGui::GetFontSize();
    const double target = g_boot_fps, target_ms = 1000.0 / target;

    /* Average, worst and 1% low over the samples held. */
    float sorted[k_ft];
    double sum = 0.0;
    for (int k = 0; k < g_ft_n; k++) { sorted[k] = g_ft[k]; sum += g_ft[k]; }
    std::sort(sorted, sorted + g_ft_n);
    const double avg = g_ft_n ? sum / g_ft_n : 0.0;
    const double worst = g_ft_n ? sorted[g_ft_n - 1] : 0.0;
    double low1 = 0.0;              /* mean fps of the slowest 1% of frames */
    if (g_ft_n) {
        const int m = g_ft_n / 100 > 0 ? g_ft_n / 100 : 1;
        double s = 0.0;
        for (int k = g_ft_n - m; k < g_ft_n; k++) s += sorted[k];
        low1 = 1000.0 / (s / m);
    }

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, 12.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.62f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 0.7f, fs * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(fs * 0.5f, fs * 0.2f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::Begin("##hud", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    const float w = fs * 15.0f;

    /* fps, large, coloured against the target */
    ImGui::SetWindowFontScale(1.6f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(fps_colour(g_fps, target)));
    ImGui::Text("%.0f", g_fps);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("fps  / %d", g_boot_fps);

    ImGui::Text("%.1f ms avg   %.1f ms worst", avg, worst);
    ImGui::Text("1%% low %.0f fps", low1);

    /* frame-time graph: bars, oldest left; dashed target line */
    const float gh = fs * 3.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + gh), IM_COL32(0, 0, 0, 90), 4.0f);
    const float top_ms = (float)(target_ms * 2.5);
    const float bw = w / k_ft;
    for (int k = 0; k < g_ft_n; k++) {
        const int idx = (g_ft_i - g_ft_n + k + k_ft) % k_ft;
        const float ms = g_ft[idx];
        const float hgt = gh * (ms < top_ms ? ms / top_ms : 1.0f);
        const float x = p0.x + (k_ft - g_ft_n + k) * bw;
        const ImU32 c = ms <= target_ms * 1.05 ? IM_COL32(120, 230, 120, 200)
                      : ms <= target_ms * 1.5 ? IM_COL32(255, 205, 90, 220) : IM_COL32(255, 100, 80, 240);
        dl->AddRectFilled(ImVec2(x, p0.y + gh - hgt), ImVec2(x + (bw > 1.5f ? bw - 0.5f : bw), p0.y + gh), c);
    }
    const float ty = p0.y + gh - gh * (float)(target_ms / top_ms);
    for (float x = p0.x; x < p0.x + w; x += 8.0f)
        dl->AddLine(ImVec2(x, ty), ImVec2(x + 4.0f < p0.x + w ? x + 4.0f : p0.x + w, ty), IM_COL32(255, 255, 255, 110));
    ImGui::Dummy(ImVec2(w, gh));

    /* what is being rendered */
    unsigned iw = 0, ih = 0, ow = 0, oh = 0;
    const int fsr = rsx_vulkan_backend_fsr_state(&iw, &ih, &ow, &oh);
    if (iw && ih) {
        if (fsr) ImGui::TextDisabled("%ux%u  FSR 1 -> %ux%u", iw, ih, ow, oh);
        else     ImGui::TextDisabled("%ux%u  (%dx)", iw, ih, g_boot_scale);
    }
    if (g_cpu >= 0.0) ImGui::TextDisabled("%u draws   CPU %.0f%%", rsx_vulkan_backend_guest_draws(), g_cpu);
    else              ImGui::TextDisabled("%u draws", rsx_vulkan_backend_guest_draws());

    ImGui::End();
    ImGui::PopStyleVar(3);
}

void draw_fps()
{
    if (port_settings_get()->fps_counter == 2) draw_fps_compact();
    else draw_fps_detailed();
}

void draw(void*)
{
    if (!g_styled) { style(); g_styled = true; }
    /* Guest frames presented over half a second; a fresh count after the UI
     * was hidden (no ImGui frames were built meanwhile). */
    static int last_frame = -10;
    if (ImGui::GetFrameCount() != last_frame + 1) {
        g_fps_frames = ppu_boot_frames_presented(); g_fps_t = 0.0;
        g_ft_last = g_fps_frames; g_ft_since = 0.0;
    }
    last_frame = ImGui::GetFrameCount();
    hud_sample(rsx_ui_frame_dt());
    g_fps_t += rsx_ui_frame_dt();
    if (g_fps_t >= 0.5) {
        const unsigned f = ppu_boot_frames_presented();
        g_fps = (double)(f - g_fps_frames) / g_fps_t;
        g_fps_frames = f; g_fps_t = 0.0;
    }
    if (port_settings_get()->fps_counter) draw_fps();
    if (rsx_ui_menu_open()) draw_menu();
}

/* The window's thread, under rsx_ui's lock: no rsx_ui_set_draw/_set_hotkey here. */
int hotkey(int key, void*)
{
    if (key == SDLK_F10) {
        /* detailed -> compact -> off -> detailed */
        static const int next[] = { 1, 2, 0 };
        set_fps_counter(next[port_settings_get()->fps_counter % 3]);
        static const char* const n[] = { "off", "detailed", "compact" };
        fprintf(stderr, "[ui] HUD %s\n", n[port_settings_get()->fps_counter % 3]);
        return 1;
    }
    return 0;
}

} /* namespace */

void port_ui_init(void)
{
    { const char* e = getenv("PS3RECOMP_RES_SCALE");
      const int n = e ? atoi(e) : 1;
      g_boot_scale = n >= 1 && n <= 4 ? n : 1; }
    { const char* e = getenv("PS3RECOMP_FPS");
      g_boot_fps = (e && atoi(e) == 60) ? 60 : 30; }
    rsx_ui_set_draw(draw, nullptr);
    rsx_ui_set_hotkey(hotkey, nullptr);
    rsx_ui_set_visible(port_settings_get()->fps_counter);
}

#else

void port_ui_init(void) {}

#endif
