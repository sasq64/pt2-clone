/*
** A libretro core that is the ProTracker 2 clone itself: the tracker runs its
** own main loop on a thread of its own, and the SDL2 it is linked against
** (sdl_shim.c) hands its frames, its audio and its input to libretro instead
** of to a window.
**
** The module named by retro_load_game() is passed to the tracker the way the
** program takes one on the command line, which is also what makes it start
** playing on its own.
*/

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

#include "pt2_header.h"
#include "pt2_replayer.h"
#include "pt2_structs.h"

#include "pt2_libretro.h"

#define SAMPLE_RATE 48000
#define FRAMES_PER_RUN (SAMPLE_RATE / VBLANK_HZ)

/* How long a retro_run() waits for the tracker to draw. Long enough to cover a
** module load, short enough that the frontend never looks hung if the tracker
** stops drawing altogether.
*/
#define FRAME_TIMEOUT_MS 100

/* How long retro_unload_game() gives the tracker's own threads to notice that
** editor.programRunning went false.
*/
#define SHUTDOWN_TIMEOUT_MS 500

int pt2_clone_main(int argc, char *argv[]);

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_audio_sample_t audio_sample_cb;

static pthread_t tracker_thread;
static bool tracker_started, tracker_exited;
static char module_path[4096];

static int mouse_x = SCREEN_W / 2, mouse_y = SCREEN_H / 2;
static int16_t audio_frames[FRAMES_PER_RUN * 2];

/* retro_key to SDL scancode. The keycode the tracker also wants follows from
** the two: printable keys share ASCII with libretro, and every other SDL
** keycode is its scancode with SDLK_SCANCODE_MASK set.
*/
static const struct { unsigned key; SDL_Scancode scancode; } key_map[] =
{
	{ RETROK_a, SDL_SCANCODE_A }, { RETROK_b, SDL_SCANCODE_B }, { RETROK_c, SDL_SCANCODE_C },
	{ RETROK_d, SDL_SCANCODE_D }, { RETROK_e, SDL_SCANCODE_E }, { RETROK_f, SDL_SCANCODE_F },
	{ RETROK_g, SDL_SCANCODE_G }, { RETROK_h, SDL_SCANCODE_H }, { RETROK_i, SDL_SCANCODE_I },
	{ RETROK_j, SDL_SCANCODE_J }, { RETROK_k, SDL_SCANCODE_K }, { RETROK_l, SDL_SCANCODE_L },
	{ RETROK_m, SDL_SCANCODE_M }, { RETROK_n, SDL_SCANCODE_N }, { RETROK_o, SDL_SCANCODE_O },
	{ RETROK_p, SDL_SCANCODE_P }, { RETROK_q, SDL_SCANCODE_Q }, { RETROK_r, SDL_SCANCODE_R },
	{ RETROK_s, SDL_SCANCODE_S }, { RETROK_t, SDL_SCANCODE_T }, { RETROK_u, SDL_SCANCODE_U },
	{ RETROK_v, SDL_SCANCODE_V }, { RETROK_w, SDL_SCANCODE_W }, { RETROK_x, SDL_SCANCODE_X },
	{ RETROK_y, SDL_SCANCODE_Y }, { RETROK_z, SDL_SCANCODE_Z },

	{ RETROK_1, SDL_SCANCODE_1 }, { RETROK_2, SDL_SCANCODE_2 }, { RETROK_3, SDL_SCANCODE_3 },
	{ RETROK_4, SDL_SCANCODE_4 }, { RETROK_5, SDL_SCANCODE_5 }, { RETROK_6, SDL_SCANCODE_6 },
	{ RETROK_7, SDL_SCANCODE_7 }, { RETROK_8, SDL_SCANCODE_8 }, { RETROK_9, SDL_SCANCODE_9 },
	{ RETROK_0, SDL_SCANCODE_0 },

	{ RETROK_F1, SDL_SCANCODE_F1 }, { RETROK_F2, SDL_SCANCODE_F2 }, { RETROK_F3, SDL_SCANCODE_F3 },
	{ RETROK_F4, SDL_SCANCODE_F4 }, { RETROK_F5, SDL_SCANCODE_F5 }, { RETROK_F6, SDL_SCANCODE_F6 },
	{ RETROK_F7, SDL_SCANCODE_F7 }, { RETROK_F8, SDL_SCANCODE_F8 }, { RETROK_F9, SDL_SCANCODE_F9 },
	{ RETROK_F10, SDL_SCANCODE_F10 }, { RETROK_F11, SDL_SCANCODE_F11 }, { RETROK_F12, SDL_SCANCODE_F12 },

	{ RETROK_RETURN, SDL_SCANCODE_RETURN }, { RETROK_ESCAPE, SDL_SCANCODE_ESCAPE },
	{ RETROK_BACKSPACE, SDL_SCANCODE_BACKSPACE }, { RETROK_TAB, SDL_SCANCODE_TAB },
	{ RETROK_SPACE, SDL_SCANCODE_SPACE }, { RETROK_MINUS, SDL_SCANCODE_MINUS },
	{ RETROK_EQUALS, SDL_SCANCODE_EQUALS }, { RETROK_LEFTBRACKET, SDL_SCANCODE_LEFTBRACKET },
	{ RETROK_RIGHTBRACKET, SDL_SCANCODE_RIGHTBRACKET }, { RETROK_BACKSLASH, SDL_SCANCODE_BACKSLASH },
	{ RETROK_SEMICOLON, SDL_SCANCODE_SEMICOLON }, { RETROK_QUOTE, SDL_SCANCODE_APOSTROPHE },
	{ RETROK_BACKQUOTE, SDL_SCANCODE_GRAVE }, { RETROK_COMMA, SDL_SCANCODE_COMMA },
	{ RETROK_PERIOD, SDL_SCANCODE_PERIOD }, { RETROK_SLASH, SDL_SCANCODE_SLASH },
	{ RETROK_OEM_102, SDL_SCANCODE_NONUSBACKSLASH },

	{ RETROK_INSERT, SDL_SCANCODE_INSERT }, { RETROK_DELETE, SDL_SCANCODE_DELETE },
	{ RETROK_HOME, SDL_SCANCODE_HOME }, { RETROK_END, SDL_SCANCODE_END },
	{ RETROK_PAGEUP, SDL_SCANCODE_PAGEUP }, { RETROK_PAGEDOWN, SDL_SCANCODE_PAGEDOWN },
	{ RETROK_UP, SDL_SCANCODE_UP }, { RETROK_DOWN, SDL_SCANCODE_DOWN },
	{ RETROK_LEFT, SDL_SCANCODE_LEFT }, { RETROK_RIGHT, SDL_SCANCODE_RIGHT },

	{ RETROK_KP0, SDL_SCANCODE_KP_0 }, { RETROK_KP1, SDL_SCANCODE_KP_1 },
	{ RETROK_KP2, SDL_SCANCODE_KP_2 }, { RETROK_KP3, SDL_SCANCODE_KP_3 },
	{ RETROK_KP4, SDL_SCANCODE_KP_4 }, { RETROK_KP5, SDL_SCANCODE_KP_5 },
	{ RETROK_KP6, SDL_SCANCODE_KP_6 }, { RETROK_KP7, SDL_SCANCODE_KP_7 },
	{ RETROK_KP8, SDL_SCANCODE_KP_8 }, { RETROK_KP9, SDL_SCANCODE_KP_9 },
	{ RETROK_KP_PERIOD, SDL_SCANCODE_KP_PERIOD }, { RETROK_KP_DIVIDE, SDL_SCANCODE_KP_DIVIDE },
	{ RETROK_KP_MULTIPLY, SDL_SCANCODE_KP_MULTIPLY }, { RETROK_KP_MINUS, SDL_SCANCODE_KP_MINUS },
	{ RETROK_KP_PLUS, SDL_SCANCODE_KP_PLUS }, { RETROK_KP_ENTER, SDL_SCANCODE_KP_ENTER },

	{ RETROK_NUMLOCK, SDL_SCANCODE_NUMLOCKCLEAR }, { RETROK_CAPSLOCK, SDL_SCANCODE_CAPSLOCK },
	{ RETROK_LSHIFT, SDL_SCANCODE_LSHIFT }, { RETROK_RSHIFT, SDL_SCANCODE_RSHIFT },
	{ RETROK_LCTRL, SDL_SCANCODE_LCTRL }, { RETROK_RCTRL, SDL_SCANCODE_RCTRL },
	{ RETROK_LALT, SDL_SCANCODE_LALT }, { RETROK_RALT, SDL_SCANCODE_RALT },
	/* the two Amiga keys */
	{ RETROK_LSUPER, SDL_SCANCODE_LGUI }, { RETROK_RSUPER, SDL_SCANCODE_RGUI },
	{ RETROK_LMETA, SDL_SCANCODE_LGUI }, { RETROK_RMETA, SDL_SCANCODE_RGUI },
	{ RETROK_MENU, SDL_SCANCODE_MENU }, { RETROK_MODE, SDL_SCANCODE_MODE },
};

static SDL_Scancode scancode_of(unsigned key)
{
	for (size_t i = 0; i < sizeof (key_map) / sizeof (key_map[0]); i++)
	{
		if (key_map[i].key == key)
			return key_map[i].scancode;
	}
	return SDL_SCANCODE_UNKNOWN;
}

static void keyboard_event(bool down, unsigned keycode, uint32_t character, uint16_t modifiers)
{
	const SDL_Scancode scancode = scancode_of(keycode);
	if (scancode == SDL_SCANCODE_UNKNOWN)
		return;

	const SDL_Keycode sym = (keycode < 128) ? (SDL_Keycode)keycode : (SDL_Keycode)(scancode | SDLK_SCANCODE_MASK);
	pt2lr_post_key(down, scancode, sym);

	/* Text fields (song and sample names, file names) are fed by SDL_TEXTINPUT
	** rather than by the key event.
	*/
	if (down && character >= 0x20 && character < 0x7F)
		pt2lr_post_text((char)character);
	else if (down && keycode >= 0x20 && keycode < 0x7F)
		pt2lr_post_text((modifiers & RETROKMOD_SHIFT) ? (char)toupper((int)keycode) : (char)keycode);
}

static void poll_mouse(void)
{
	const int dx = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
	const int dy = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);

	mouse_x += dx;
	mouse_y += dy;

	if (mouse_x < 0) mouse_x = 0;
	if (mouse_y < 0) mouse_y = 0;
	if (mouse_x > SCREEN_W-1) mouse_x = SCREEN_W-1;
	if (mouse_y > SCREEN_H-1) mouse_y = SCREEN_H-1;

	pt2lr_post_mouse(mouse_x, mouse_y,
		input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) != 0,
		input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) != 0);
}

static void *tracker_main(void *data)
{
	(void)data;

	char program[] = "pt2-clone";
	char *argv[2] = { program, module_path };
	const int argc = (module_path[0] != '\0') ? 2 : 1;

	pt2_clone_main(argc, argv);

	tracker_exited = true;
	return NULL;
}

void retro_init(void) { }
void retro_deinit(void) { }

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof (*info));
	info->library_name = "ProTracker 2 clone";
	info->library_version = PROG_VER_STR;
	info->valid_extensions = "mod|stk|nst|m15|pp|ust|xpk";
	info->need_fullpath = true;
	info->block_extract = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof (*info));
	info->geometry.base_width = SCREEN_W;
	info->geometry.base_height = SCREEN_H;
	info->geometry.max_width = SCREEN_W;
	info->geometry.max_height = SCREEN_H;
	info->geometry.aspect_ratio = 0.0f;
	info->timing.fps = VBLANK_HZ;
	info->timing.sample_rate = SAMPLE_RATE;
}

void retro_set_environment(retro_environment_t cb)
{
	environ_cb = cb;

	bool no_game = true;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { audio_sample_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }

bool retro_load_game(const struct retro_game_info *info)
{
	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
	if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format))
	{
		pt2lr_log("frontend refused XRGB8888");
		return false;
	}

	struct retro_keyboard_callback keyboard = { keyboard_event };
	environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &keyboard);

	module_path[0] = '\0';
	if (info != NULL && info->path != NULL)
		snprintf(module_path, sizeof (module_path), "%s", info->path);

	if (!pt2lr_shim_init(SAMPLE_RATE))
		return false;

	if (pthread_create(&tracker_thread, NULL, tracker_main, NULL) != 0)
	{
		pt2lr_log("could not start the tracker thread");
		return false;
	}
	tracker_started = true;

	/* Anything the tracker cannot set up at all (out of memory, a module it
	** refuses) ends its main() right away, and there is no point handing the
	** frontend a core that will only ever draw black.
	*/
	for (int waited = 0; waited < 200 && !tracker_exited; waited += 10)
		SDL_Delay(10);

	if (tracker_exited)
	{
		pthread_join(tracker_thread, NULL);
		tracker_started = false;
		return false;
	}

	return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
	(void)type; (void)info; (void)num;
	return false;
}

void retro_unload_game(void)
{
	if (!tracker_started)
		return;

	editor.programRunning = false;
	ui.throwExit = true;

	/* The scope thread polls editor.programRunning; let it go before the main
	** loop gets to free what it is reading.
	*/
	pt2lr_wait_threads(SHUTDOWN_TIMEOUT_MS);
	pt2lr_shim_stop();

	pthread_join(tracker_thread, NULL);
	tracker_started = false;
	tracker_exited = false;
}

void retro_reset(void)
{
	if (tracker_started && song != NULL && song->loaded)
	{
		modStop();
		modPlay(DONT_SET_PATTERN, 0, DONT_SET_ROW);
	}
}

void retro_run(void)
{
	input_poll_cb();
	poll_mouse();

	/* Mix first, draw second. The replayer runs inside the audio callback, so
	** mixing this run's samples is what moves the song on, and the frame taken
	** afterwards is the one that shows where the song now is.
	*/
	pt2lr_pull_audio(audio_frames, FRAMES_PER_RUN);

	const uint32_t *frame = pt2lr_take_frame(FRAME_TIMEOUT_MS);
	video_cb(frame, SCREEN_W, SCREEN_H, SCREEN_W * sizeof (uint32_t));
	audio_batch_cb(audio_frames, FRAMES_PER_RUN);
}

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }

void retro_cheat_reset(void) { }
void retro_cheat_set(unsigned index, bool enabled, const char *code) { (void)index; (void)enabled; (void)code; }

unsigned retro_get_region(void) { return RETRO_REGION_PAL; }

void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
