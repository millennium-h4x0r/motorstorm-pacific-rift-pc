/* MotorStorm: Pacific Rift port. The player's settings: user/settings.ini (or
 * $PS3_USER_DIR/settings.ini), read at start by a windowed run and turned into
 * the environment variables the port already reads. A variable already set
 * wins over the file, so run.sh and the test scripts keep working as they did.
 * The in-game menu (ui.cpp) changes the values and saves them.
 *
 * Trimmed from the Apocalypse port's settings.h: only what Pacific Rift's
 * backend can act on today. */
#ifndef PORT_SETTINGS_H
#define PORT_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct port_settings {
    /* [video] */
    int fullscreen;     /* 0 window, 1 full screen (PS3RECOMP_VK_FULLSCREEN) */
    /* [performance] */
    int fps_counter;    /* the counter drawn by the UI (F10) */
} port_settings;

/* The defaults: a window, no FPS counter. */
void port_settings_defaults(port_settings* s);

/* Read `path` (missing or unreadable: the defaults). Returns 1 when the file
 * was read. Unknown keys and bad values are ignored. */
int port_settings_load(port_settings* s, const char* path);

/* Write `path`, creating its directory. 0 on success. */
int port_settings_save(const port_settings* s, const char* path);

/* Each value as the environment variable the port reads, unless that
 * variable is already set. */
void port_settings_export(const port_settings* s);

/* user/settings.ini, or $PS3_USER_DIR/settings.ini. */
const char* port_settings_path(void);

/* The process-wide settings, loaded once by main(). */
port_settings* port_settings_get(void);

#ifdef __cplusplus
}
#endif

#endif
