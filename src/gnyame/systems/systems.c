#include "gnyame/gnyame.h"

/*
 * One translation unit per system family, gathered here the way entities.c gathers the kinds.
 *
 * Nothing shared lives in this file yet. When something does — a query buffer every system reuses,
 * an ordering helper — this is where it goes.
 */
#include "gnyame/systems/system_camera.c"
#include "gnyame/systems/system_movement.c"
#include "gnyame/systems/system_sky.c"
#include "gnyame/systems/system_terrain3d.c"
