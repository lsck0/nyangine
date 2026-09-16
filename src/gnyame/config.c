#include "gnyame/gnyame.h"

GNY_Config NYA_CONFIG;

/** Reset by a code reload along with the rest of this DLL's data, which is what makes gnyame_run re-attach. */
NYA_INTERNAL b8 _gny_config_attached = false;

void gny_config_attach(void) {
    if (_gny_config_attached) return;
    _gny_config_attached = true;

    NYA_Error loaded = nya_config_watch(GNY_CONFIG_FILE, nya_reflect_of(GNY_Config), &NYA_CONFIG);

    // not fatal, like a missing settings file: NYA_CONFIG keeps its zeroed defaults.
    if (!loaded.ok) nya_log_warn("Could not load %s: %s", GNY_CONFIG_FILE, (NYA_ConstCString)loaded.message);
}
