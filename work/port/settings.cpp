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

static port_settings g_settings = { 0, 0 };

void port_settings_defaults(port_settings* s)
{
    s->fullscreen = 0;
    s->fps_counter = 0;
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
        const char* k = trim(t);
        const char* v = trim(eq + 1);
        if (!strcmp(section, "video") && !strcmp(k, "fullscreen")) s->fullscreen = as_bool(v, s->fullscreen);
        else if (!strcmp(section, "performance") && !strcmp(k, "fps_counter"))
            s->fps_counter = as_bool(v, s->fps_counter);
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
            "\n[performance]\n"
            "fps_counter = %d\n",
            s->fullscreen ? 1 : 0, s->fps_counter ? 1 : 0);
    return fclose(f) == 0 ? 0 : -1;
}

void port_settings_export(const port_settings* s)
{
#ifdef _WIN32
    if (s->fullscreen && !getenv("PS3RECOMP_VK_FULLSCREEN")) _putenv_s("PS3RECOMP_VK_FULLSCREEN", "1");
#else
    if (s->fullscreen) setenv("PS3RECOMP_VK_FULLSCREEN", "1", 0);
#endif
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
