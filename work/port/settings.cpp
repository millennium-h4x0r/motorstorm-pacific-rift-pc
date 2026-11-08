/* MotorStorm: Pacific Rift port: user/settings.ini (settings.h). */
#include "settings.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

static port_settings g_settings = { 0, 1, 0, 0.8f, 30, 0 };

void port_settings_defaults(port_settings* s)
{
    s->fullscreen = 0;
    s->scale = 1;
    s->upscaler = 0;
    s->fsr_sharpness = 0.8f;
    s->fps = 30;
    s->fps_counter = 1;
}

static char* trim(char* t)
{
    while (*t == ' ' || *t == '\t') t++;
    char* e = t + strlen(t);
    while (e > t && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return t;
}

static int as_bool(const char* v, int def)
{
    if (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "yes")) return 1;
    if (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "no")) return 0;
    return def;
}

int port_settings_load(port_settings* s, const char* path)
{
    port_settings_defaults(s);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[512], section[64] = "";
    while (fgets(line, sizeof line, f)) {
        char* t = trim(line);
        if (!*t || *t == '#' || *t == ';') continue;
        if (*t == '[') {
            char* e = strchr(t, ']');
            if (e) { *e = 0; snprintf(section, sizeof section, "%s", t + 1); }
            continue;
        }
        char* eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        /* A value may carry a comment after it ("scale = 2 ; 1440p"). */
        if (char* c = strpbrk(eq + 1, ";#")) *c = 0;
        const char* k = trim(t);
        const char* v = trim(eq + 1);
        if (!strcmp(section, "video") && !strcmp(k, "fullscreen")) s->fullscreen = as_bool(v, s->fullscreen);
        else if (!strcmp(section, "video") && !strcmp(k, "scale")) {
            const int n = atoi(v);
            if (n >= 1 && n <= 3) s->scale = n;
        } else if (!strcmp(section, "video") && !strcmp(k, "upscaler")) {
            if (!strcmp(v, "fsr") || !strcmp(v, "fsr1")) s->upscaler = 1;
            else if (!strcmp(v, "off") || !strcmp(v, "none")) s->upscaler = 0;
        } else if (!strcmp(section, "video") && !strcmp(k, "fps")) {
            const int n = atoi(v);
            if (n == 30 || n == 60) s->fps = n;
        } else if (!strcmp(section, "video") && !strcmp(k, "fsr_sharpness")) {
            char* end = nullptr;
            const float f = strtof(v, &end);
            if (end != v && f >= 0.0f && f <= 1.0f) s->fsr_sharpness = f;
        } else if (!strcmp(section, "performance") && !strcmp(k, "fps_counter"))
        {   /* 0 off, 1 detailed, 2 compact; on/off/true/false as 1/0 */
            char* end = nullptr;
            const long n = strtol(v, &end, 10);
            s->fps_counter = (end != v && n >= 0 && n <= 2) ? (int)n : as_bool(v, s->fps_counter);
        }
    }
    fclose(f);
    return 1;
}

static void make_parent_dir(const char* path)
{
    std::string d = path;
    const size_t k = d.find_last_of('/');
    if (k == std::string::npos || k == 0) return;
    d.resize(k);
#ifdef _WIN32
    _mkdir(d.c_str());
#else
    mkdir(d.c_str(), 0755);
#endif
}

int port_settings_save(const port_settings* s, const char* path)
{
    make_parent_dir(path);
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f,
            "# MotorStorm: Pacific Rift port settings. The in-game menu (F1) writes this file.\n"
            "# An environment variable set before the game starts wins over a value here.\n"
            "\n[video]\n"
            "fullscreen = %d\n"
            "scale = %d          ; 1 = 720p, 2 = 1440p, 3 = 4K (takes effect after a restart)\n"
            "upscaler = %s      ; off, or fsr (AMD FSR 1: upscales the frame to the window)\n"
            "fsr_sharpness = %.2f ; 0..1, 1 = sharpest\n"
            "fps = %d            ; 30 (original) or 60 (takes effect after a restart)\n"
            "\n[performance]\n"
            "fps_counter = %d      ; HUD (F10 cycles): 0 = off, 1 = detailed, 2 = compact\n",
            s->fullscreen ? 1 : 0, s->scale, s->upscaler ? "fsr" : "off", (double)s->fsr_sharpness,
            s->fps == 60 ? 60 : 30, s->fps_counter);
    return fclose(f) == 0 ? 0 : -1;
}

static void export_var(const char* name, const char* value)
{
    if (getenv(name)) return;                      /* set by the caller: it wins */
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

void port_settings_export(const port_settings* s)
{
    char b[32];
    if (s->fullscreen) export_var("PS3RECOMP_VK_FULLSCREEN", "1");
    if (s->scale > 1) { snprintf(b, sizeof b, "%d", s->scale); export_var("PS3RECOMP_RES_SCALE", b); }
    if (s->upscaler) export_var("PS3RECOMP_FSR", "1");
    if (s->fps == 60) export_var("PS3RECOMP_FPS", "60");
    snprintf(b, sizeof b, "%.2f", (double)s->fsr_sharpness);
    export_var("PS3RECOMP_FSR_SHARPNESS", b);
}

const char* port_settings_path(void)
{
    static std::string p;
    if (p.empty()) {
        const char* u = getenv("PS3_USER_DIR");
        p = std::string(u && *u ? u : "user") + "/settings.ini";
    }
    return p.c_str();
}

port_settings* port_settings_get(void) { return &g_settings; }
