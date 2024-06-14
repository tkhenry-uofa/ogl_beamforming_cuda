/* See LICENSE for license details. */

#include <raylib.h>
#include <rlgl.h>

#include "util.c"

#ifndef _DEBUG

#include "beamformer.c"
static void do_debug(void) { }

#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *libname = "./beamformer.so";
static void *libhandle;

typedef void (do_beamformer_fn)(BeamformerCtx*);
static do_beamformer_fn *do_beamformer;

static struct timespec
get_filetime(const char *name)
{
	struct stat sb;
	if (stat(name, &sb) < 0)
		return (struct timespec){0};
	return sb.st_mtim;
}

static b32
filetime_is_newer(struct timespec a, struct timespec b)
{
	return (a.tv_sec - b.tv_sec) + (a.tv_nsec - b.tv_nsec);
}

static void
load_library(const char *lib)
{
	/* NOTE: glibc is buggy gnuware so we need to check this */
	if (libhandle)
		dlclose(libhandle);
	libhandle = dlopen(lib, RTLD_NOW|RTLD_LOCAL);
	if (!libhandle)
		TraceLog(LOG_ERROR, "do_debug: dlopen: %s\n", dlerror());

	do_beamformer = dlsym(libhandle, "do_beamformer");
	if (!do_beamformer)
		TraceLog(LOG_ERROR, "do_debug: dlsym: %s\n", dlerror());
}

static void
do_debug(void)
{
	static struct timespec updated_time;
	struct timespec test_time = get_filetime(libname);
	if (filetime_is_newer(test_time, updated_time)) {
		struct timespec sleep_time = {.tv_sec = 0, .tv_nsec = 100e6};
		nanosleep(&sleep_time, &sleep_time);
		load_library(libname);
		updated_time = test_time;
	}
}

#endif /* _DEBUG */

int
main(void)
{
	BeamformerCtx ctx = {0};

	ctx.window_size = (uv2){.w = 720, .h = 720};

	ctx.bg = DARKGRAY;
	ctx.fg = (Color){ .r = 0xea, .g = 0xe1, .b = 0xb4, .a = 0xff };

	SetConfigFlags(FLAG_VSYNC_HINT);
	InitWindow(ctx.window_size.w, ctx.window_size.h, "OGL Beamformer");

	while(!WindowShouldClose()) {
		do_debug();

		BeginDrawing();
		do_beamformer(&ctx);
		EndDrawing();
	}
}
