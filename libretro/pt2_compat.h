#pragma once

/* Force included (-include) into every pt2-clone translation unit, so the
** tracker's own sources need no edits at all. Everything here is about the two
** things a program may do that a shared library living inside someone else's
** process may not.
**
** The system headers come first on purpose: a function-like macro below would
** otherwise expand inside the declaration it is named after.
*/

/* Before SDL.h arrives with the includes below: on Windows and macOS its
** SDL_main.h renames main() to SDL_main unless this is already defined.
*/
#define SDL_MAIN_HANDLED

#include <signal.h>
#include <unistd.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "pt2_libretro.h"

/* main() becomes an ordinary function, which pt2_libretro.c runs on a thread
** of its own.
*/
#define main pt2_clone_main

/* The working directory belongs to the frontend, which resolves relative paths
** of its own while the core runs.
*/
#define chdir(path) pt2lr_chdir(path)

/* A crash handler is the host's business, and this one writes a backup module
** into whatever directory it lands in before handing the signal back.
*/
#define sigaction(signum, act, oldact) (0)

#ifdef _WIN32
#define SetUnhandledExceptionFilter(filter) ((void)0)
#define SetCurrentDirectoryW(path) (1)
#define _wchdir(path) (-1)

/* Process-wide, and it cannot be undone: what the host's windows are drawn at
** is not something a core may decide.
*/
#define SetProcessDPIAware() (TRUE)

/* A modal box put up from inside someone else's process is a hang, not a
** message. Everywhere else this is SDL_ShowSimpleMessageBox, which the shim
** logs.
*/
#define MessageBoxA(hwnd, text, caption, type) \
	(pt2lr_log("%s: %s", (caption), (text)), 0)
#endif
