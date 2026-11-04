/* The port's screens over the game window (ui.cpp). */
#ifndef PORT_UI_H
#define PORT_UI_H

/* Hook the in-game menu (F1), the FPS counter (F10) into rsx_ui. After the
 * settings are loaded, before the frame clock starts the backend. Does
 * nothing in a build without the UI; an offscreen run never starts it. */
void port_ui_init(void);

#endif
