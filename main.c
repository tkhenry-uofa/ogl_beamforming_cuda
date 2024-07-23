/* See LICENSE for license details. */
#include "beamformer.h"

static char *compute_shader_paths[CS_LAST] = {
	[CS_HADAMARD] = "shaders/hadamard.glsl",
	[CS_LPF]      = "shaders/lpf.glsl",
	[CS_MIN_MAX]  = "shaders/min_max.glsl",
	[CS_UFORCES]  = "shaders/uforces.glsl",
};

#ifndef _DEBUG

#include "beamformer.c"
static void do_debug(void) { }

#else
#include <dlfcn.h>
#include <time.h>

static char *libname = "./beamformer.so";
static void *libhandle;

typedef void do_beamformer_fn(BeamformerCtx *, Arena);
static do_beamformer_fn *do_beamformer;

static os_filetime
get_filetime(char *name)
{
	os_file_stats fstats = os_get_file_stats(name);
	return fstats.timestamp;
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
	static os_filetime updated_time;
	os_filetime test_time = get_filetime(libname);
	if (filetime_is_newer(test_time, updated_time)) {
		struct timespec sleep_time = {.tv_sec = 0, .tv_nsec = 100e6};
		nanosleep(&sleep_time, &sleep_time);
		load_library(libname);
		updated_time = test_time;
	}
}

#endif /* _DEBUG */

static u32
compile_shader(Arena a, u32 type, s8 shader)
{
	u32 sid = glCreateShader(type);
	glShaderSource(sid, 1, (const char **)&shader.data, (int *)&shader.len);
	glCompileShader(sid);

	i32 res = 0;
	glGetShaderiv(sid, GL_COMPILE_STATUS, &res);

	char *stype;
	switch (type) {
	case GL_COMPUTE_SHADER:  stype = "Compute";  break;
	case GL_FRAGMENT_SHADER: stype = "Fragment"; break;
	}

	if (res == GL_FALSE) {
		TraceLog(LOG_WARNING, "SHADER: [ID %u] %s shader failed to compile", sid, stype);
		i32 len = 0;
		glGetShaderiv(sid, GL_INFO_LOG_LENGTH, &len);
		s8 err = s8alloc(&a, len);
		glGetShaderInfoLog(sid, len, (int *)&err.len, (char *)err.data);
		TraceLog(LOG_WARNING, "SHADER: [ID %u] Compile error: %s", sid, (char *)err.data);
		glDeleteShader(sid);
	} else {
		TraceLog(LOG_INFO, "SHADER: [ID %u] %s shader compiled successfully", sid, stype);
	}

	return sid;
}

static void
init_fragment_shader_ctx(FragmentShaderCtx *ctx, uv4 out_data_dim)
{
	ctx->output = LoadRenderTexture(out_data_dim.x, out_data_dim.y);
	ctx->db     = -50.0f;
}

static void
reload_shaders(BeamformerCtx *ctx, Arena a)
{
	ComputeShaderCtx *csctx = &ctx->csctx;
	for (u32 i = 0; i < ARRAY_COUNT(csctx->programs); i++) {
		Arena tmp = a;
		os_file_stats fs = os_get_file_stats(compute_shader_paths[i]);
		s8 shader_text   = os_read_file(&tmp, compute_shader_paths[i], fs.filesize);
		u32 shader_id    = compile_shader(tmp, GL_COMPUTE_SHADER, shader_text);

		if (shader_id) {
			glDeleteProgram(csctx->programs[i]);
			csctx->programs[i] = rlLoadComputeShaderProgram(shader_id);
			ctx->flags |= DO_COMPUTE;
		}

		glDeleteShader(shader_id);
	}

	//csctx->lpf_order_id    = glGetUniformLocation(csctx->programs[CS_LPF],     "u_lpf_order");
	csctx->out_data_tex_id = glGetUniformLocation(csctx->programs[CS_UFORCES], "u_out_data_tex");
	csctx->mip_view_tex_id = glGetUniformLocation(csctx->programs[CS_MIN_MAX], "u_mip_view_tex");
	csctx->mips_level_id   = glGetUniformLocation(csctx->programs[CS_MIN_MAX], "u_mip_map");

	Shader updated_fs = LoadShader(NULL, "shaders/render.glsl");
	if (updated_fs.id != rlGetShaderIdDefault()) {
		UnloadShader(ctx->fsctx.shader);
		ctx->fsctx.shader          = updated_fs;
		ctx->fsctx.out_data_tex_id = GetShaderLocation(updated_fs, "u_out_data_tex");
		ctx->fsctx.db_cutoff_id    = GetShaderLocation(updated_fs, "u_db_cutoff");
	}
}

int
main(void)
{
	BeamformerCtx ctx = {0};

	Arena temp_memory = os_new_arena(256 * MEGABYTE);

	ctx.window_size  = (uv2){.w = 960, .h = 720};
	ctx.out_data_dim = (uv4){.x = 256, .y = 1024, .z = 1};

	SetConfigFlags(FLAG_VSYNC_HINT|FLAG_WINDOW_RESIZABLE);
	InitWindow(ctx.window_size.w, ctx.window_size.h, "OGL Beamformer");

	ctx.font_size    = 32;
	ctx.font_spacing = 0;
	ctx.font         = GetFontDefault();

	init_fragment_shader_ctx(&ctx.fsctx, ctx.out_data_dim);

	ctx.data_pipe = os_open_named_pipe("/tmp/beamformer_data_fifo");
	ctx.params    = os_open_shared_memory_area("/ogl_beamformer_parameters");
	/* TODO: properly handle this? */
	ASSERT(ctx.data_pipe.file != OS_INVALID_FILE);
	ASSERT(ctx.params);

	ctx.params->raw.output_points = ctx.out_data_dim;

	/* NOTE: allocate space for Uniform Buffer Object but don't send anything yet */
	glGenBuffers(1, &ctx.csctx.shared_ubo);
	glBindBuffer(GL_UNIFORM_BUFFER, ctx.csctx.shared_ubo);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(BeamformerParameters), 0, GL_STATIC_DRAW);

	ctx.flags |= RELOAD_SHADERS|ALLOC_SSBOS|ALLOC_OUT_TEX|UPLOAD_FILTER;

	while(!WindowShouldClose()) {
		do_debug();

		if (ctx.flags & RELOAD_SHADERS) {
			ctx.flags &= ~RELOAD_SHADERS;
			reload_shaders(&ctx, temp_memory);
		}

		do_beamformer(&ctx, temp_memory);
	}

	/* NOTE: make sure this will get cleaned up after external
	 * programs release their references */
	os_remove_shared_memory("/ogl_beamformer_parameters");

	/* NOTE: garbage code needed for Linux */
	os_close_named_pipe(ctx.data_pipe);
}
