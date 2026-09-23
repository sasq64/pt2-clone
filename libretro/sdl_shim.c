/*
** The SDL2 that pt2-clone is linked against inside the core.
**
** Only the calls the tracker actually makes are here, and they are backed by
** the core rather than by a window and an audio device of their own: the frame
** it presents is handed to the frontend, its audio callback is run once per
** retro_run for exactly the samples that go with that frame, and the event
** queue is fed from libretro input.
**
** The tracker keeps running its own main loop on its own thread. SDL_RenderPresent
** is where that thread parks between frames, which is what keeps the frontend
** in charge of the pace without anything in pt2-clone knowing about it.
*/

#include <SDL2/SDL.h>
#ifdef _WIN32
#include <SDL2/SDL_syswm.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pt2_header.h"
#include "pt2_config.h"
#include "pt2_structs.h"

#include "pt2_libretro.h"

#define FRAME_PIXELS (SCREEN_W * SCREEN_H)

static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t frame_ready_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t frame_wanted_cv = PTHREAD_COND_INITIALIZER;
static uint32_t shim_fb[FRAME_PIXELS];  /* written by the tracker thread */
static uint32_t core_fb[FRAME_PIXELS];  /* read by the frontend */
static bool frame_pending, frame_wanted;
static bool stopping;

static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
#define EVENT_QUEUE_SIZE 256
static SDL_Event event_queue[EVENT_QUEUE_SIZE];
static int event_head, event_tail;
static SDL_Keymod mod_state;
static int mouse_x, mouse_y;
static Uint32 mouse_buttons;

/* Audio device: the callback the tracker registered, run straight from
** pt2lr_pull_audio() -- see there for why it is not a thread of its own.
*/
static pthread_mutex_t audio_lock;           /* SDL_LockAudioDevice() */
static SDL_AudioCallback audio_cb;
static void *audio_cb_data;
static bool audio_paused = true;
static int chunk_frames;

static int output_rate = 48000;
static int live_threads;

/* Nanoseconds of audio the tracker has mixed: what SDL_GetPerformanceCounter()
** reports to everything but the tracker's own helper threads. See there.
*/
static uint64_t audio_clock_ns;

/* Set on the threads pt2-clone starts through SDL_CreateThread. */
static __thread bool helper_thread;  /* threads pt2-clone started through SDL_CreateThread */

/* Opaque handles. Nothing is behind them; the tracker only ever passes them
** back to us.
*/
static int window_handle, renderer_handle, texture_handle, cursor_handle;

void pt2lr_log(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[pt2] ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
}

int pt2lr_chdir(const char *path)
{
	(void)path;
	return -1;
}

static void deadline_in(struct timespec *ts, int ms)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_sec += ms / 1000;
	ts->tv_nsec += (ms % 1000) * 1000000L;
	if (ts->tv_nsec >= 1000000000L)
	{
		ts->tv_nsec -= 1000000000L;
		ts->tv_sec++;
	}
}

bool pt2lr_shim_init(int sample_rate)
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&audio_lock, &attr);
	pthread_mutexattr_destroy(&attr);

	output_rate = sample_rate;
	chunk_frames = sample_rate / VBLANK_HZ;
	return true;
}

void pt2lr_shim_stop(void)
{
	pthread_mutex_lock(&frame_lock);
	stopping = true;
	pthread_cond_broadcast(&frame_wanted_cv);
	pthread_cond_broadcast(&frame_ready_cv);
	pthread_mutex_unlock(&frame_lock);

}

const uint32_t *pt2lr_take_frame(int timeout_ms)
{
	struct timespec deadline;
	deadline_in(&deadline, timeout_ms);

	pthread_mutex_lock(&frame_lock);

	/* Whatever is already drawn was drawn before this run mixed its audio, so
	** it is a frame behind the samples going out with it. Ask for a new one and
	** wait for it: one frame drawn per run, showing that run's audio.
	*/
	frame_pending = false;

	frame_wanted = true;
	pthread_cond_signal(&frame_wanted_cv);

	while (!frame_pending && !stopping)
	{
		if (pthread_cond_timedwait(&frame_ready_cv, &frame_lock, &deadline) == ETIMEDOUT)
			break;
	}

	if (frame_pending)
	{
		memcpy(core_fb, shim_fb, sizeof (core_fb));
		frame_pending = false;
	}

	pthread_mutex_unlock(&frame_lock);
	return core_fb;
}

/* Mix this run's audio, now.
**
** The tracker's replayer runs inside its audio callback, so this call is what
** moves the song on, and mixing exactly one frame's worth of it here -- right
** before the frame that goes with it is taken -- is what makes the samples and
** the picture handed over by one retro_run the same instant. Anything buffered
** in between would shift the sound later than the picture that draws it.
**
** Calling the callback on this thread is safe because the tracker is parked in
** SDL_RenderPresent while we are in here, and no lockAudio() region in
** pt2-clone draws a frame.
*/
void pt2lr_pull_audio(int16_t *out, int frames)
{
	if (audio_cb == NULL || audio_paused)
	{
		memset(out, 0, (size_t)frames * 2 * sizeof (int16_t));
	}
	else
	{
		pthread_mutex_lock(&audio_lock);
		audio_cb(audio_cb_data, (Uint8 *)out, frames * 2 * (int)sizeof (int16_t));
		pthread_mutex_unlock(&audio_lock);
	}

	/* Moved on once the samples exist, so that the frame taken next sees a
	** clock that has reached them -- see SDL_GetPerformanceCounter().
	*/
	const uint64_t mixed = ((uint64_t)frames * 1000000000ULL) / (uint64_t)output_rate;
	__atomic_add_fetch(&audio_clock_ns, mixed, __ATOMIC_RELEASE);
}

void pt2lr_wait_threads(int timeout_ms)
{
	for (int waited = 0; waited < timeout_ms; waited += 2)
	{
		if (live_threads <= 0)
			return;
		SDL_Delay(2);
	}
	if (live_threads > 0)
		pt2lr_log("%d tracker thread(s) still running at shutdown", live_threads);
}

/* Input ------------------------------------------------------------------ */

static void push_event(const SDL_Event *event)
{
	int next = (event_head + 1) % EVENT_QUEUE_SIZE;
	if (next == event_tail)
		return; /* full: the tracker is not draining, drop the event */

	event_queue[event_head] = *event;
	event_head = next;
}

/* The tracker asks SDL for the modifier state rather than tracking key events
** itself (readKeyModifiers), so the modifiers are kept here.
*/
static void update_mods(bool down, SDL_Scancode scancode)
{
	SDL_Keymod bit = KMOD_NONE;

	switch (scancode)
	{
		case SDL_SCANCODE_LSHIFT: bit = KMOD_LSHIFT; break;
		case SDL_SCANCODE_RSHIFT: bit = KMOD_RSHIFT; break;
		case SDL_SCANCODE_LCTRL:  bit = KMOD_LCTRL;  break;
		case SDL_SCANCODE_RCTRL:  bit = KMOD_RCTRL;  break;
		case SDL_SCANCODE_LALT:   bit = KMOD_LALT;   break;
		case SDL_SCANCODE_RALT:   bit = KMOD_RALT;   break;
		case SDL_SCANCODE_LGUI:   bit = KMOD_LGUI;   break;
		case SDL_SCANCODE_RGUI:   bit = KMOD_RGUI;   break;
		case SDL_SCANCODE_CAPSLOCK:
			if (down)
				mod_state ^= KMOD_CAPS;
			return;
		default: return;
	}

	if (down)
		mod_state |= bit;
	else
		mod_state &= ~bit;
}

void pt2lr_post_key(bool down, SDL_Scancode scancode, SDL_Keycode keycode)
{
	SDL_Event event;
	memset(&event, 0, sizeof (event));

	event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	event.key.keysym.scancode = scancode;
	event.key.keysym.sym = keycode;

	pthread_mutex_lock(&event_lock);
	update_mods(down, scancode);
	event.key.keysym.mod = mod_state;
	push_event(&event);
	pthread_mutex_unlock(&event_lock);
}

void pt2lr_post_text(char ch)
{
	SDL_Event event;
	memset(&event, 0, sizeof (event));

	event.type = SDL_TEXTINPUT;
	event.text.text[0] = ch;

	pthread_mutex_lock(&event_lock);
	push_event(&event);
	pthread_mutex_unlock(&event_lock);
}

void pt2lr_post_mouse(int x, int y, bool left, bool right)
{
	pthread_mutex_lock(&event_lock);

	mouse_x = x;
	mouse_y = y;

	const Uint32 buttons = (left ? SDL_BUTTON_LMASK : 0) | (right ? SDL_BUTTON_RMASK : 0);
	const Uint32 changed = buttons ^ mouse_buttons;
	mouse_buttons = buttons;

	for (int i = 0; i < 2; i++)
	{
		const Uint8 button = (i == 0) ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT;
		const Uint32 mask = (i == 0) ? SDL_BUTTON_LMASK : SDL_BUTTON_RMASK;
		if (!(changed & mask))
			continue;

		SDL_Event event;
		memset(&event, 0, sizeof (event));
		event.type = (buttons & mask) ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
		event.button.button = button;
		event.button.state = (buttons & mask) ? SDL_PRESSED : SDL_RELEASED;
		event.button.clicks = 1;
		event.button.x = x;
		event.button.y = y;
		push_event(&event);
	}

	pthread_mutex_unlock(&event_lock);
}

int SDL_PollEvent(SDL_Event *event)
{
	int got = 0;

	pthread_mutex_lock(&event_lock);
	if (event_tail != event_head)
	{
		*event = event_queue[event_tail];
		event_tail = (event_tail + 1) % EVENT_QUEUE_SIZE;
		got = 1;
	}
	pthread_mutex_unlock(&event_lock);

	return got;
}

int SDL_PushEvent(SDL_Event *event)
{
	pthread_mutex_lock(&event_lock);
	push_event(event);
	pthread_mutex_unlock(&event_lock);
	return 1;
}

SDL_Keymod SDL_GetModState(void)
{
	pthread_mutex_lock(&event_lock);
	const SDL_Keymod mods = mod_state;
	pthread_mutex_unlock(&event_lock);
	return mods;
}

Uint32 SDL_GetMouseState(int *x, int *y)
{
	pthread_mutex_lock(&event_lock);
	if (x != NULL) *x = mouse_x;
	if (y != NULL) *y = mouse_y;
	const Uint32 buttons = mouse_buttons;
	pthread_mutex_unlock(&event_lock);
	return buttons;
}

Uint32 SDL_GetGlobalMouseState(int *x, int *y)
{
	return SDL_GetMouseState(x, y);
}

void SDL_WarpMouseInWindow(SDL_Window *window, int x, int y)
{
	(void)window;
	pthread_mutex_lock(&event_lock);
	mouse_x = x;
	mouse_y = y;
	pthread_mutex_unlock(&event_lock);
}

Uint8 SDL_EventState(Uint32 type, int state)
{
	(void)type;
	(void)state;
	return SDL_ENABLE;
}

void SDL_StartTextInput(void) { }
void SDL_StopTextInput(void) { }

/* Video ------------------------------------------------------------------ */

/* The one call the tracker makes between reading its config file and setting
** anything up from it, which is where the settings a core has no use for are
** turned off: the frontend owns the window and the audio device, so
** fullscreen, vsync and a hardware mouse pointer are not ours to ask for.
*/
SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
	(void)title; (void)x; (void)y; (void)w; (void)h; (void)flags;

	config.soundFrequency = output_rate;
	config.soundBufferSize = chunk_frames;
	config.startInFullscreen = false;
	config.hwMouse = false;     /* draw the PT pointer into the frame instead */
	config.vsyncOff = false;    /* the frontend paces us, see SDL_RenderPresent */
	config.integerScaling = false;

	return (SDL_Window *)&window_handle;
}

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags)
{
	(void)window; (void)index; (void)flags;
	return (SDL_Renderer *)&renderer_handle;
}

SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int access, int w, int h)
{
	(void)renderer; (void)format; (void)access; (void)w; (void)h;
	return (SDL_Texture *)&texture_handle;
}

int SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch)
{
	(void)texture; (void)rect;

	pthread_mutex_lock(&frame_lock);
	for (int y = 0; y < SCREEN_H; y++)
		memcpy(&shim_fb[y * SCREEN_W], (const uint8_t *)pixels + ((size_t)y * pitch), SCREEN_W * sizeof (uint32_t));
	pthread_mutex_unlock(&frame_lock);

	return 0;
}

/* Where the tracker's thread waits for the frontend to ask for the next frame. */
void SDL_RenderPresent(SDL_Renderer *renderer)
{
	(void)renderer;

	pthread_mutex_lock(&frame_lock);

	frame_pending = true;
	pthread_cond_signal(&frame_ready_cv);

	while (!frame_wanted && !stopping)
		pthread_cond_wait(&frame_wanted_cv, &frame_lock);
	frame_wanted = false;

	pthread_mutex_unlock(&frame_lock);
}

int SDL_RenderClear(SDL_Renderer *renderer) { (void)renderer; return 0; }

int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *src, const SDL_Rect *dst)
{
	(void)renderer; (void)texture; (void)src; (void)dst;
	return 0;
}

int SDL_SetRenderDrawColor(SDL_Renderer *r, Uint8 red, Uint8 g, Uint8 b, Uint8 a)
{
	(void)r; (void)red; (void)g; (void)b; (void)a;
	return 0;
}

int SDL_SetRenderDrawBlendMode(SDL_Renderer *r, SDL_BlendMode mode) { (void)r; (void)mode; return 0; }
int SDL_SetTextureBlendMode(SDL_Texture *t, SDL_BlendMode mode) { (void)t; (void)mode; return 0; }

void SDL_DestroyTexture(SDL_Texture *texture) { (void)texture; }
void SDL_DestroyRenderer(SDL_Renderer *renderer) { (void)renderer; }
void SDL_DestroyWindow(SDL_Window *window) { (void)window; }

Uint32 SDL_GetWindowFlags(SDL_Window *window)
{
	(void)window;
	return SDL_WINDOW_SHOWN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;
}

void SDL_GetWindowSize(SDL_Window *window, int *w, int *h)
{
	(void)window;
	if (w != NULL) *w = SCREEN_W;
	if (h != NULL) *h = SCREEN_H;
}

void SDL_GL_GetDrawableSize(SDL_Window *window, int *w, int *h)
{
	SDL_GetWindowSize(window, w, h);
}

void SDL_GetWindowPosition(SDL_Window *window, int *x, int *y)
{
	(void)window;
	if (x != NULL) *x = 0;
	if (y != NULL) *y = 0;
}

/* No window, so the single instancing pt2-clone does on Windows stops here --
** which is the point: it would otherwise have a second view hand its module to
** the first and quit.
*/
#ifdef _WIN32
SDL_bool SDL_GetWindowWMInfo(SDL_Window *window, SDL_SysWMinfo *info)
{
	(void)window; (void)info;
	return SDL_FALSE;
}
#endif

int SDL_GetWindowDisplayIndex(SDL_Window *window) { (void)window; return 0; }

int SDL_GetDesktopDisplayMode(int index, SDL_DisplayMode *mode)
{
	(void)index;
	memset(mode, 0, sizeof (*mode));
	mode->format = SDL_PIXELFORMAT_ARGB8888;
	mode->w = SCREEN_W;
	mode->h = SCREEN_H;
	mode->refresh_rate = VBLANK_HZ;
	return 0;
}

int SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags) { (void)window; (void)flags; return 0; }
void SDL_SetWindowTitle(SDL_Window *window, const char *title) { (void)window; (void)title; }
void SDL_ShowWindow(SDL_Window *window) { (void)window; }
void SDL_RaiseWindow(SDL_Window *window) { (void)window; }
void SDL_RestoreWindow(SDL_Window *window) { (void)window; }
void SDL_EnableScreenSaver(void) { }

int SDL_ShowCursor(int toggle) { return (toggle == SDL_QUERY) ? SDL_DISABLE : toggle; }
void SDL_SetCursor(SDL_Cursor *cursor) { (void)cursor; }
SDL_Cursor *SDL_GetDefaultCursor(void) { return (SDL_Cursor *)&cursor_handle; }
void SDL_FreeCursor(SDL_Cursor *cursor) { (void)cursor; }

SDL_Cursor *SDL_CreateColorCursor(SDL_Surface *surface, int hot_x, int hot_y)
{
	(void)surface; (void)hot_x; (void)hot_y;
	return (SDL_Cursor *)&cursor_handle;
}

/* Surfaces: only ever used to build the (unused) hardware mouse cursors, but
** the tracker fills them with real pixels, so they need real memory.
*/
SDL_Surface *SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth,
	Uint32 rmask, Uint32 gmask, Uint32 bmask, Uint32 amask)
{
	(void)flags; (void)depth;

	SDL_Surface *surface = (SDL_Surface *)calloc(1, sizeof (SDL_Surface));
	SDL_PixelFormat *format = (SDL_PixelFormat *)calloc(1, sizeof (SDL_PixelFormat));
	uint32_t *pixels = (uint32_t *)calloc((size_t)width * height, sizeof (uint32_t));

	if (surface == NULL || format == NULL || pixels == NULL)
	{
		free(surface);
		free(format);
		free(pixels);
		return NULL;
	}

	format->format = SDL_PIXELFORMAT_ARGB8888;
	format->BitsPerPixel = 32;
	format->BytesPerPixel = 4;
	format->Rmask = (rmask != 0) ? rmask : 0x00FF0000;
	format->Gmask = (gmask != 0) ? gmask : 0x0000FF00;
	format->Bmask = (bmask != 0) ? bmask : 0x000000FF;
	format->Amask = (amask != 0) ? amask : 0xFF000000;
	format->Rshift = 16;
	format->Gshift = 8;
	format->Bshift = 0;
	format->Ashift = 24;
	format->refcount = 1;

	surface->format = format;
	surface->w = width;
	surface->h = height;
	surface->pitch = width * 4;
	surface->pixels = pixels;
	surface->refcount = 1;

	return surface;
}

void SDL_FreeSurface(SDL_Surface *surface)
{
	if (surface == NULL)
		return;

	free(surface->pixels);
	free(surface->format);
	free(surface);
}

int SDL_LockSurface(SDL_Surface *surface) { (void)surface; return 0; }
void SDL_UnlockSurface(SDL_Surface *surface) { (void)surface; }
int SDL_SetColorKey(SDL_Surface *surface, int flag, Uint32 key) { (void)surface; (void)flag; (void)key; return 0; }
int SDL_SetSurfaceRLE(SDL_Surface *surface, int flag) { (void)surface; (void)flag; return 0; }
int SDL_SetSurfaceBlendMode(SDL_Surface *surface, SDL_BlendMode mode) { (void)surface; (void)mode; return 0; }

Uint32 SDL_MapRGB(const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b)
{
	(void)format;
	return 0xFF000000 | ((Uint32)r << 16) | ((Uint32)g << 8) | b;
}

/* Audio ------------------------------------------------------------------ */

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
	const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int allowed_changes)
{
	(void)device; (void)allowed_changes;

	if (iscapture || desired == NULL || desired->callback == NULL)
		return 0;

	audio_cb = desired->callback;
	audio_cb_data = desired->userdata;
	audio_paused = true;

	if (obtained != NULL)
	{
		*obtained = *desired;
		obtained->freq = output_rate;
		obtained->format = AUDIO_S16SYS;
		obtained->channels = 2;
		/* The buffer size is only ever read as a latency (calcAudioLatencyVars),
		** to hold the scopes and meters back until the samples behind them are
		** being heard. Here there is nothing to hold them back by: the samples
		** mixed in a retro_run leave with the frame drawn in that same call, and
		** what happens to them afterwards is the frontend's latency, which it
		** lines the picture up with itself.
		*/
		obtained->samples = 0;
		obtained->silence = 0;
		obtained->size = (Uint32)chunk_frames * 2 * sizeof (int16_t);
	}

	return 1;
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on)
{
	(void)dev;
	audio_paused = (pause_on != 0);
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID dev)
{
	(void)dev;

	pthread_mutex_lock(&audio_lock);
	audio_cb = NULL;
	pthread_mutex_unlock(&audio_lock);
}

void SDL_LockAudioDevice(SDL_AudioDeviceID dev) { (void)dev; pthread_mutex_lock(&audio_lock); }
void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev) { (void)dev; pthread_mutex_unlock(&audio_lock); }

int SDL_GetNumAudioDevices(int iscapture) { (void)iscapture; return 0; }
const char *SDL_GetAudioDeviceName(int index, int iscapture) { (void)index; (void)iscapture; return NULL; }
int SDL_GetNumAudioDrivers(void) { return 1; }
const char *SDL_GetAudioDriver(int index) { (void)index; return "libretro"; }

/* Threads ---------------------------------------------------------------- */

typedef struct
{
	SDL_ThreadFunction fn;
	void *data;
} shim_thread_t;

static void *thread_trampoline(void *arg)
{
	shim_thread_t *thread = (shim_thread_t *)arg;

	helper_thread = true;
	thread->fn(thread->data);

	free(thread);
	__atomic_sub_fetch(&live_threads, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

/* On Windows SDL_CreateThread is a macro handing SDL the CRT's _beginthreadex
** and _endthreadex, so the entry point behind it takes two arguments more than
** it does anywhere else. SDL_PASSED_BEGINTHREAD_ENDTHREAD is what the header
** sets when it does that.
*/
#ifdef SDL_PASSED_BEGINTHREAD_ENDTHREAD
#undef SDL_CreateThread
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data,
                             pfnSDL_CurrentBeginThread pfnBeginThread,
                             pfnSDL_CurrentEndThread pfnEndThread)
#else
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data)
#endif
{
	(void)name;
#ifdef SDL_PASSED_BEGINTHREAD_ENDTHREAD
	(void)pfnBeginThread; (void)pfnEndThread;
#endif

	shim_thread_t *thread = (shim_thread_t *)calloc(1, sizeof (shim_thread_t));
	if (thread == NULL)
		return NULL;

	thread->fn = fn;
	thread->data = data;

	pthread_t id;
	__atomic_add_fetch(&live_threads, 1, __ATOMIC_SEQ_CST);
	if (pthread_create(&id, NULL, thread_trampoline, thread) != 0)
	{
		__atomic_sub_fetch(&live_threads, 1, __ATOMIC_SEQ_CST);
		free(thread);
		return NULL;
	}

	pthread_detach(id);
	return (SDL_Thread *)(uintptr_t)1;
}

void SDL_DetachThread(SDL_Thread *thread) { (void)thread; }
int SDL_SetThreadPriority(SDL_ThreadPriority priority) { (void)priority; return 0; }

/* Everything else -------------------------------------------------------- */

int SDL_Init(Uint32 flags) { (void)flags; return 0; }
void SDL_Quit(void) { }
const char *SDL_GetError(void) { return ""; }
SDL_bool SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return SDL_TRUE; }
int SDL_setenv(const char *name, const char *value, int overwrite) { (void)name; (void)value; (void)overwrite; return 0; }
SDL_bool SDL_HasSSE(void) { return SDL_TRUE; }
SDL_bool SDL_HasSSE2(void) { return SDL_TRUE; }
void SDL_free(void *mem) { free(mem); }

void SDL_GetVersion(SDL_version *ver)
{
	SDL_VERSION(ver);
}

void SDL_Delay(Uint32 ms)
{
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

Uint64 SDL_GetPerformanceFrequency(void)
{
	return 1000000000ULL;
}

/* The tracker reads this counter for two unrelated jobs, and in a core they
** need different clocks.
**
** Its helper threads pace themselves with it (the scopes redraw at 64Hz), and
** that is real time. Everything else uses it to stamp the queue that holds the
** scopes and meters back until the audio behind them is due
** (pt2_visuals_sync.c) -- and here, real time is exactly the wrong answer: the
** song advances when the frontend runs us, which is not the rate a wall clock
** ticks at and not when the frontend plays what we hand it. Reporting how much
** audio has been mixed instead ties the queue to the samples themselves, so
** the frame a retro_run draws shows the samples that same call mixed, however
** fast or slow the frontend is going.
*/
Uint64 SDL_GetPerformanceCounter(void)
{
	if (helper_thread)
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return ((Uint64)ts.tv_sec * 1000000000ULL) + (Uint64)ts.tv_nsec;
	}

	return __atomic_load_n(&audio_clock_ns, __ATOMIC_ACQUIRE);
}

int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window)
{
	(void)flags; (void)window;
	pt2lr_log("%s: %s", (title != NULL) ? title : "message", (message != NULL) ? message : "");
	return 0;
}
