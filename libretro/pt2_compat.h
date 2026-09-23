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

#ifdef _WIN32
/* The whole of windows.h, which is what pt2-clone's own include amounts to
** (its WIN32_MEAN_AND_LEAN is a typo). No unistd.h: MSVC has none, so
** pt2_hpc.c is free to name a function pointer usleep, and mingw's declaration
** of the real one collides with it.
*/
#include <windows.h>

/* pt2_hpc.c pulls a finer usleep out of ntdll and needs the type its calls
** return. The Windows SDK has it in winnt.h; mingw-w64 leaves it to headers
** that also declare half of ntdll, which is more than a function pointer named
** NtDelayExecution can live beside.
*/
#ifndef NTSTATUS
typedef LONG NTSTATUS;
#endif
#else
#include <signal.h>
#include <unistd.h>
#endif

#include "pt2_libretro.h"

/* main() becomes an ordinary function, which pt2_libretro.c runs on a thread
** of its own.
*/
#define main pt2_clone_main

/* The working directory belongs to the frontend, which resolves relative paths
** of its own while the core runs. Windows walks it through the wide calls
** below instead, and never calls chdir() at all.
*/
#ifndef _WIN32
#define chdir(path) pt2lr_chdir(path)

/* A crash handler is the host's business, and this one writes a backup module
** into whatever directory it lands in before handing the signal back.
*/
#define sigaction(signum, act, oldact) (0)
#endif

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
