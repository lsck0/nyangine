#pragma once

#include "nyangine/nyangine.h"

NYA_CString     GNY_WINDOW_MAIN_TITLE = "gnyame";
NYA_WindowFlags GNY_WINDOW_MAIN_FLAGS = NYA_WINDOW_RESIZABLE;

/* A preference, not a guarantee. A compositor is free to hand back something else entirely. */
u32 GNY_WINDOW_MAIN_WIDTH  = 1280;
u32 GNY_WINDOW_MAIN_HEIGHT = 720;

/** Valid until the window is closed, after which every lookup through it returns null. */
extern NYA_WindowHandle GNY_WINDOW_MAIN;

void gny_window_main_create(void);
