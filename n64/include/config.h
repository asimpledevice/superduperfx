/* N64 wrapper around the desktop host's config.h.
 *
 * The shared runtime includes "config.h" for the settings struct. The desktop
 * header also pulls in SDL through sdl_compat.h, which the N64 build does not
 * have. Pre-defining that header's include guard skips it, and the two SDL
 * type names config.h itself uses are defined here. */
#ifndef SNESRECOMP_N64_CONFIG_WRAPPER_H
#define SNESRECOMP_N64_CONFIG_WRAPPER_H

#include <stdint.h>

#define SNESRECOMP_DESKTOP_SDL_COMPAT_H 1
typedef int32_t SDL_Keycode;
typedef uint16_t SDL_Keymod;

#include "../../runner/src/desktop/config.h"

#endif
