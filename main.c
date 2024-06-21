/* See LICENSE for license details. */

#include <raylib.h>
#include <rlgl.h>

#include <stdio.h>
#include <stdlib.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>
#include <GL/glext.h>

#include "util.h"
#include "os_unix.c"

static char *compute_shader_paths[CS_LAST] = {
	//[CS_MIN_MAX] = "shaders/min_max.glsl",
	[CS_HADAMARD] = "shaders/hadamard.glsl",
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

static void
fill_hadamard(i32 *m, u32 dim)
{
	/* bit hack to check if dim is power of 2 */
	ASSERT(!(dim & (dim - 1)) && dim);

	#define IND(i, j) ((i) * dim + (j))
	m[0] = 1;
	for (u32 k = 1; k < dim; k *= 2) {
		for (u32 i = 0; i < k; i++) {
			for (u32 j = 0; j < k; j++) {
				i32 val = m[IND(i, j)];
				m[IND(i + k, j)]     =  val;
				m[IND(i, j + k)]     =  val;
				m[IND(i + k, j + k)] = -val;
			}
		}
	}
	#undef IND
}

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
init_compute_shader_ctx(ComputeShaderCtx *ctx, Arena a, uv3 rf_data_dim)
{
	for (u32 i = 0; i < ARRAY_COUNT(ctx->programs); i++) {
		char *shader_text = LoadFileText(compute_shader_paths[i]);
		u32 shader_id     = rlCompileShader(shader_text, RL_COMPUTE_SHADER);
		ctx->programs[i]  = rlLoadComputeShaderProgram(shader_id);
		glDeleteShader(shader_id);
		UnloadFileText(shader_text);
	}

	ctx->rf_data_dim_id  = glGetUniformLocation(ctx->programs[CS_UFORCES], "u_rf_data_dim");
	ctx->out_data_dim_id = glGetUniformLocation(ctx->programs[CS_UFORCES], "u_out_data_dim");

	ctx->rf_data_dim  = rf_data_dim;
	size rf_data_size = rf_data_dim.w * rf_data_dim.h * rf_data_dim.d * sizeof(f32);
	for (u32 i = 0; i < ARRAY_COUNT(ctx->rf_data_ssbos); i++)
		ctx->rf_data_ssbos[i] = rlLoadShaderBuffer(rf_data_size, NULL, GL_DYNAMIC_COPY);
	ctx->rf_data_idx = 0;

	/* NOTE: store hadamard in GPU once; it won't change for a particular imaging session */
	ctx->hadamard_dim       = (uv2){ .x = rf_data_dim.y, .y = rf_data_dim.y };
	size hadamard_elements  = ctx->hadamard_dim.x * ctx->hadamard_dim.y;
	i32  *hadamard          = alloc(&a, i32, hadamard_elements);
	fill_hadamard(hadamard, ctx->hadamard_dim.x);
	ctx->hadamard_ssbo = rlLoadShaderBuffer(hadamard_elements * sizeof(i32), hadamard, GL_DYNAMIC_COPY);
}

static void
init_fragment_shader_ctx(FragmentShaderCtx *ctx, uv3 out_data_dim)
{
	ctx->shader = LoadShader(NULL, "shaders/render.glsl");

	ctx->out_data_dim_id = glGetUniformLocation(ctx->shader.id, "u_out_data_dim");
	glUniform3uiv(ctx->out_data_dim_id, 1, out_data_dim.E);
	/* TODO: add min max uniform */

	/* output texture for image blitting */
	Texture2D new;
	new.width   = out_data_dim.w;
	new.height  = out_data_dim.h;
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
		}

		glDeleteShader(shader_id);
	}

	csctx->rf_data_dim_id  = glGetUniformLocation(csctx->programs[CS_UFORCES], "u_rf_data_dim");
	csctx->out_data_dim_id = glGetUniformLocation(csctx->programs[CS_UFORCES], "u_out_data_dim");

	Shader updated_fs       = LoadShader(NULL, "shaders/render.glsl");
	if (updated_fs.id != rlGetShaderIdDefault()) {
		UnloadShader(ctx->fsctx.shader);
		ctx->fsctx.shader = updated_fs;
		ctx->fsctx.out_data_dim_id = GetShaderLocation(updated_fs, "u_out_data_dim");
		glUniform3uiv(ctx->fsctx.out_data_dim_id, 1, ctx->out_data_dim.E);
	}
}

int
main(void)
{
	BeamformerCtx ctx = {0};

	Arena temp_memory = os_new_arena(256 * MEGABYTE);

	char *decoded_name          = "/tmp/decoded.bin";
	os_file_stats decoded_stats = os_get_file_stats(decoded_name);
	s8 raw_rf_data              = os_read_file(&temp_memory, decoded_name, decoded_stats.filesize);

	ctx.window_size  = (uv2){.w = 720, .h = 720};
	ctx.out_data_dim = (uv3){.w = 720, .h = 720, .d = 1};

	ctx.bg = PINK;
	ctx.fg = (Color){ .r = 0xea, .g = 0xe1, .b = 0xb4, .a = 0xff };

	SetConfigFlags(FLAG_VSYNC_HINT);
	InitWindow(ctx.window_size.w, ctx.window_size.h, "OGL Beamformer");

	ctx.font = GetFontDefault();

	size out_data_size = ctx.out_data_dim.w * ctx.out_data_dim.h * ctx.out_data_dim.d * sizeof(f32);
	ctx.out_data_ssbo  = rlLoadShaderBuffer(out_data_size, NULL, GL_DYNAMIC_COPY);
	init_compute_shader_ctx(&ctx.csctx, temp_memory, (uv3){.w = 4093, .h = 128, .d = 1});
	init_fragment_shader_ctx(&ctx.fsctx, ctx.out_data_dim);

	ctx.flags |= RELOAD_SHADERS;

	while(!WindowShouldClose()) {
		do_debug();

		if (ctx.flags & RELOAD_SHADERS) {
			ctx.flags &= ~RELOAD_SHADERS;
			reload_shaders(&ctx, temp_memory);
		}

		do_beamformer(&ctx, temp_memory, raw_rf_data);
	}
}
