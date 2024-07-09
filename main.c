/* See LICENSE for license details. */
#include "beamformer.h"

static char *compute_shader_paths[CS_LAST] = {
	[CS_MIN_MAX]  = "shaders/min_max.glsl",
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

static void
fill_hadamard(i32 *m, u32 dim)
{
	ASSERT(dim && ISPOWEROF2(dim));

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
init_compute_shader_ctx(ComputeShaderCtx *ctx, Arena a, uv3 rf_data_dim)
{
	ctx->rf_data_dim     = rf_data_dim;
	size rf_raw_size     = rf_data_dim.w * rf_data_dim.h * rf_data_dim.d * sizeof(i16);
	size rf_decoded_size = rf_data_dim.w * rf_data_dim.h * rf_data_dim.d * sizeof(i32);

	glGenBuffers(ARRAY_COUNT(ctx->rf_data_ssbos), ctx->rf_data_ssbos);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx->rf_data_ssbos[0]);
	glBufferStorage(GL_SHADER_STORAGE_BUFFER, rf_raw_size, 0,
	                GL_DYNAMIC_STORAGE_BIT|GL_MAP_WRITE_BIT);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx->rf_data_ssbos[1]);
	glBufferStorage(GL_SHADER_STORAGE_BUFFER, rf_decoded_size, 0, GL_DYNAMIC_STORAGE_BIT);

	/* NOTE: store hadamard in GPU once; it won't change for a particular imaging session */
	ctx->hadamard_dim       = (uv2){ .x = rf_data_dim.d, .y = rf_data_dim.d };
	size hadamard_elements  = ctx->hadamard_dim.x * ctx->hadamard_dim.y;
	i32  *hadamard          = alloc(&a, i32, hadamard_elements);
	fill_hadamard(hadamard, ctx->hadamard_dim.x);
	ctx->hadamard_ssbo = rlLoadShaderBuffer(hadamard_elements * sizeof(i32), hadamard, GL_STATIC_DRAW);
}

static void
init_fragment_shader_ctx(FragmentShaderCtx *ctx, uv3 out_data_dim)
{
	ctx->output = LoadRenderTexture(out_data_dim.w, out_data_dim.h);
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
		}

		glDeleteShader(shader_id);
	}

	csctx->rf_data_dim_id  = glGetUniformLocation(csctx->programs[CS_UFORCES], "u_rf_data_dim");
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

	ctx.window_size  = (uv2){.w = 2048, .h = 2048};
	ctx.out_data_dim = (uv3){.w = 2048, .h = 2048, .d = 1};

	ctx.bg = PINK;
	ctx.fg = (Color){ .r = 0xea, .g = 0xe1, .b = 0xb4, .a = 0xff };

	SetConfigFlags(FLAG_VSYNC_HINT);
	InitWindow(ctx.window_size.w, ctx.window_size.h, "OGL Beamformer");

	ctx.font = GetFontDefault();

	/* NOTE: allocate storage for beamformed output data;
	 * this is shared between compute and fragment shaders */
	{
		uv3 odim = ctx.out_data_dim;
		u32 max_dim = MAX(odim.x, MAX(odim.y, odim.z));
		/* TODO: does this actually matter or is 0 fine? */
		ctx.out_texture_unit = 0;
		ctx.out_texture_mips = _tzcnt_u32(max_dim) + 1;
		glActiveTexture(GL_TEXTURE0 + ctx.out_texture_unit);
		glGenTextures(1, &ctx.out_texture);
		glBindTexture(GL_TEXTURE_3D, ctx.out_texture);
		glTexStorage3D(GL_TEXTURE_3D, ctx.out_texture_mips, GL_RG32F, odim.x, odim.y, odim.z);
	}

	init_compute_shader_ctx(&ctx.csctx, temp_memory, (uv3){.w = 3456, .h = 128, .d = 8});
	init_fragment_shader_ctx(&ctx.fsctx, ctx.out_data_dim);

	ctx.data_pipe = os_open_named_pipe("/tmp/beamformer_data_fifo");
	ctx.params    = os_open_shared_memory_area("/ogl_beamformer_parameters");
	/* TODO: properly handle this? */
	ASSERT(ctx.data_pipe.file != OS_INVALID_FILE);
	ASSERT(ctx.params);

	ctx.flags |= RELOAD_SHADERS;

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
