/* See LICENSE for license details. */

#include <raylib.h>
#include <rlgl.h>

#include <stdio.h>
#include <stdlib.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>
#include <GL/glext.h>

#include "util.c"
#include "os_unix.c"

static char *compute_shader_paths[CS_LAST] = {
	//[CS_MIN_MAX] = "shaders/min_max.glsl",
	[CS_UFORCES] = "shaders/uforces.glsl",
};

#ifndef _DEBUG

#include "beamformer.c"
static void do_debug(void) { }

#else
#include <dlfcn.h>
#include <time.h>

static char *libname = "./beamformer.so";
static void *libhandle;

typedef void do_beamformer_fn(BeamformerCtx *, Arena, s8);
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

#if 0
static void
update_output_image_dimensions(BeamformerCtx *ctx, uv2 new_size)
{
	UnloadTexture(ctx->fsctx.output);
	rlUnloadShaderBuffer(ctx->csctx.out_img_ssbo);

	size out_img_size = new_size.w * new_size.h * sizeof(f32);
	ctx->csctx.out_img_ssbo = rlLoadShaderBuffer(out_img_size, NULL, GL_DYNAMIC_COPY);

	Texture2D t       = ctx->fsctx.output;
	t.width           = new_size.w;
	t.height          = new_size.h;
	t.id              = rlLoadTexture(0, t.width, t.height, t.format, t.mipmaps);
	ctx->fsctx.output = t;
}
#endif

static void
init_compute_shader_ctx(ComputeShaderCtx *ctx, uv3 rf_data_dim, uv2 out_img_dim)
{
	for (u32 i = 0; i < ARRAY_COUNT(ctx->programs); i++) {
		char *shader_text = LoadFileText(compute_shader_paths[i]);
		u32 shader_id     = rlCompileShader(shader_text, RL_COMPUTE_SHADER);
		ctx->programs[i]  = rlLoadComputeShaderProgram(shader_id);
		glDeleteShader(shader_id);
		UnloadFileText(shader_text);
	}

	ctx->u_rf_dim_id  = glGetUniformLocation(ctx->programs[CS_UFORCES], "u_rf_dim");
	ctx->u_out_dim_id = glGetUniformLocation(ctx->programs[CS_UFORCES], "u_out_dim");

	ctx->rf_data_dim = rf_data_dim;

	size rf_data_size = rf_data_dim.w * rf_data_dim.h * rf_data_dim.d * sizeof(f32);
	size out_img_size = out_img_dim.w * out_img_dim.h * sizeof(f32);

	ctx->rf_data_ssbo = rlLoadShaderBuffer(rf_data_size, NULL, GL_DYNAMIC_COPY);
	ctx->out_img_ssbo = rlLoadShaderBuffer(out_img_size, NULL, GL_DYNAMIC_COPY);
}

static void
init_fragment_shader_ctx(FragmentShaderCtx *ctx, uv2 window_size)
{
	ctx->shader       = LoadShader(NULL, "shaders/render.glsl");
	ctx->u_out_dim_id = glGetUniformLocation(ctx->shader.id, "u_out_img_dim");
	glUniform2uiv(ctx->u_out_dim_id, 1, window_size.E);
	/* TODO: add min max uniform */

	/* output texture for image blitting */
	Texture2D new;
	new.width   = window_size.w;
	new.height  = window_size.h;
	new.mipmaps = 1;
	new.format  = RL_PIXELFORMAT_UNCOMPRESSED_R32G32B32A32;
	new.id      = rlLoadTexture(0, new.width, new.height, new.format, new.mipmaps);
	ctx->output = new;
}

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
reload_shaders(Arena a, BeamformerCtx *ctx)
{
	u32 cs_programs[ARRAY_COUNT(((ComputeShaderCtx *)0)->programs)];

	ComputeShaderCtx *csctx = &ctx->csctx;
	for (u32 i = 0; i < ARRAY_COUNT(cs_programs); i++) {
		Arena tmp = a;
		os_file_stats fs = os_get_file_stats(compute_shader_paths[i]);
		s8 shader_text   = os_read_file(&tmp, compute_shader_paths[i], fs.filesize);
		u32 shader_id    = compile_shader(tmp, GL_COMPUTE_SHADER, shader_text);

		if (shader_id != rlGetShaderIdDefault()) {
			glDeleteProgram(csctx->programs[i]);
			csctx->programs[i] = rlLoadComputeShaderProgram(shader_id);
		}

		glDeleteShader(shader_id);
	}

	csctx->u_rf_dim_id  = glGetUniformLocation(csctx->programs[CS_UFORCES], "u_rf_dim");
	csctx->u_out_dim_id = glGetUniformLocation(csctx->programs[CS_UFORCES], "u_out_dim");

	Shader updated_fs       = LoadShader(NULL, "shaders/render.glsl");
	if (updated_fs.id != rlGetShaderIdDefault()) {
		UnloadShader(ctx->fsctx.shader);
		ctx->fsctx.shader       = updated_fs;
		ctx->fsctx.u_out_dim_id = GetShaderLocation(updated_fs, "u_out_img_dim");
		glUniform2ui(ctx->fsctx.u_out_dim_id, ctx->out_img_dim.x, ctx->out_img_dim.y);
	}
}

int
main(void)
{
	BeamformerCtx ctx = {0};

	Arena temp_arena = os_new_arena(256 * MEGABYTE);

	char *decoded_name          = "/tmp/decoded.bin";
	os_file_stats decoded_stats = os_get_file_stats(decoded_name);
	s8 raw_rf_data              = os_read_file(&temp_arena, decoded_name, decoded_stats.filesize);

	ctx.window_size = (uv2){.w = 720, .h = 720};
	ctx.out_img_dim = (uv2){.w = 720, .h = 720};

	ctx.bg = DARKGRAY;
	ctx.fg = (Color){ .r = 0xea, .g = 0xe1, .b = 0xb4, .a = 0xff };

	SetConfigFlags(FLAG_VSYNC_HINT);
	InitWindow(ctx.window_size.w, ctx.window_size.h, "OGL Beamformer");

	init_compute_shader_ctx(&ctx.csctx, (uv3){.w = 4093, .h = 128, .d = 1}, ctx.window_size);
	init_fragment_shader_ctx(&ctx.fsctx, ctx.window_size);

	ctx.flags |= RELOAD_SHADERS;

	while(!WindowShouldClose()) {
		do_debug();

		if (ctx.flags & RELOAD_SHADERS) {
			ctx.flags &= ~RELOAD_SHADERS;
			reload_shaders(temp_arena, &ctx);
		}

		do_beamformer(&ctx, temp_arena, raw_rf_data);
	}
}
