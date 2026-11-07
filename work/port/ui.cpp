/* MotorStorm: Pacific Rift port: the port's screens over the game's window,
 * drawn with Dear ImGui through the toolkit's rsx_ui (libs/video/rsx_ui.h).
 * Ported from the Apocalypse port's ui.cpp, trimmed to what applies here:
 *
 *   F1, or Select+Start held half a second   the in-game menu (Escape closes it)
 *   F10                                      the FPS counter on or off
 *   F11, or Alt+Enter                        full screen and back (the backend)
 *
 * While the menu is open the game gets a neutral pad (cellPad.c). Every change
 * is written to user/settings.ini at once (settings.h) and applies now.
 * The resolution scale (Video page) is read at start, so it says it takes
 * effect after a restart; the FSR 1 upscaler and its sharpness apply at once.
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
#include <string>

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

void set_fps_counter(bool on)
{
    port_settings& s = *port_settings_get();
    s.fps_counter = on ? 1 : 0;
    rsx_ui_set_visible(s.fps_counter);
    save();
}

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
    else ImGui::TextUnformatted("Stretched to the window");
}

void page_performance(port_settings& s)
{
    bool b = s.fps_counter;
    if (ImGui::Checkbox("On-screen FPS counter  (F10)", &b)) set_fps_counter(b);
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
    ImGui::TextUnformatted("F10         the FPS counter");
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

void draw_fps()
{
    const ImGuiIO& io = ImGui::GetIO();
    char t[32];
    snprintf(t, sizeof t, "%.0f fps", g_fps);
    const ImVec2 sz = ImGui::CalcTextSize(t);
    const ImVec2 p(io.DisplaySize.x - sz.x - 18.0f, 12.0f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(p.x - 8, p.y - 4), ImVec2(p.x + sz.x + 8, p.y + sz.y + 4), IM_COL32(0, 0, 0, 150), 6.0f);
    dl->AddText(p, IM_COL32(255, 190, 110, 255), t);
}

void draw(void*)
{
    if (!g_styled) { style(); g_styled = true; }
    /* Guest frames presented over half a second; a fresh count after the UI
     * was hidden (no ImGui frames were built meanwhile). */
    static int last_frame = -10;
    if (ImGui::GetFrameCount() != last_frame + 1) {
        g_fps_frames = ppu_boot_frames_presented(); g_fps_t = 0.0;
    }
    last_frame = ImGui::GetFrameCount();
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
        set_fps_counter(!port_settings_get()->fps_counter);
        fprintf(stderr, "[ui] FPS counter %s\n", port_settings_get()->fps_counter ? "on" : "off");
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
