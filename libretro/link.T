/* The SDL2 replacement in sdl_shim.c must not be visible outside the core:
** SDL's own headers declare those functions with default visibility, and a
** frontend that has the real libSDL2 loaded would otherwise resolve them
** against each other. Keeping everything but the entry points local binds our
** calls to our definitions.
*/
{
	global: retro_*;
	local: *;
};
