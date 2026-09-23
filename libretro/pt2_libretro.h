#pragma once

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>

/* The two halves of the core talking to each other: pt2_libretro.c drives
** libretro and owns the thread pt2-clone's main() runs on, sdl_shim.c is the
** SDL2 the tracker itself is linked against.
*/

bool pt2lr_shim_init(int sample_rate);

/* Wake every parked thread and stop the audio thread, so the tracker's main
** loop can notice that editor.programRunning went false.
*/
void pt2lr_shim_stop(void);

/* Let the tracker draw one frame and hand back its pixels (ARGB8888,
** SCREEN_W*SCREEN_H). Never NULL: a frame that does not arrive within
** `timeout_ms` leaves the previous one on screen.
*/
const uint32_t *pt2lr_take_frame(int timeout_ms);

/* Mix `frames` stereo sample pairs, now. Silent until the tracker has opened
** its audio device.
*/
void pt2lr_pull_audio(int16_t *out, int frames);

/* Wait for the threads pt2-clone started through SDL_CreateThread to return. */
void pt2lr_wait_threads(int timeout_ms);

void pt2lr_post_key(bool down, SDL_Scancode scancode, SDL_Keycode keycode);
void pt2lr_post_text(char ch);
void pt2lr_post_mouse(int x, int y, bool left, bool right);

/* Not the real chdir: the tracker looks for its config and its module
** directory by walking the process' working directory, which is the frontend's
** and must not move. See pt2_compat.h.
*/
int pt2lr_chdir(const char *path);

void pt2lr_log(const char *fmt, ...);
