/* See LICENSE for license details. */
#include "beamformer.h"

static os_library_handle g_cuda_lib_handle;

static char *compute_shader_paths[CS_LAST] = {
	[CS_HADAMARD] = "shaders/hadamard.glsl",
	[CS_HERCULES] = "shaders/2d_hercules.glsl",
	[CS_DEMOD]    = "shaders/demod.glsl",
	[CS_MIN_MAX]  = "shaders/min_max.glsl",
	[CS_UFORCES]  = "shaders/uforces.glsl",
};

#ifndef _DEBUG

#include "beamformer.c"
static void do_debug(void) { }

#else
#include <time.h>

static char *libname = "./beamformer.so";
static os_library_handle libhandle;

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
do_debug(void)
{
	static os_filetime updated_time;
	os_filetime test_time = get_filetime(libname);
	if (filetime_is_newer(test_time, updated_time)) {
		struct timespec sleep_time = {.tv_sec = 0, .tv_nsec = 100e6};
		nanosleep(&sleep_time, &sleep_time);
		os_close_library(libhandle);
		libhandle     = os_load_library(libname);
		do_beamformer = os_lookup_dynamic_symbol(libhandle, "do_beamformer");
		updated_time = test_time;
	}
}

#endif /* _DEBUG */

/* NOTE: cuda lib stubs */
INIT_CUDA_CONFIGURATION_FN(init_cuda_configuration_stub) {}
REGISTER_CUDA_BUFFERS_FN(register_cuda_buffers_stub) {}
CUDA_DECODE_FN(cuda_decode_stub) {}
CUDA_HILBERT_FN(cuda_hilbert_stub) {}

static void
gl_debug_logger(u32 src, u32 type, u32 id, u32 lvl, i32 len, const char *msg, const void *userctx)
{
	(void)src; (void)type; (void)id; (void)userctx;
	fputs("[GL DEBUG ", stderr);
	switch (lvl) {
	case GL_DEBUG_SEVERITY_HIGH:         fputs("HIGH]: ",         stderr); break;
	case GL_DEBUG_SEVERITY_MEDIUM:       fputs("MEDIUM]: ",       stderr); break;
	case GL_DEBUG_SEVERITY_LOW:          fputs("LOW]: ",          stderr); break;
	case GL_DEBUG_SEVERITY_NOTIFICATION: fputs("NOTIFICATION]: ", stderr); break;
	default:                             fputs("INVALID]: ",      stderr); break;
	}
	fwrite(msg, 1, len, stderr);
	fputc('\n', stderr);
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
		if (!compute_shader_paths[i])
			continue;

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

	Arena temp_memory = os_alloc_arena((Arena){0}, 8 * MEGABYTE);

	ctx.window_size  = (uv2){.w = 960, .h = 720};
	ctx.out_data_dim = (uv4){.x = 256, .y = 1024, .z = 1};

	SetConfigFlags(FLAG_VSYNC_HINT);
	InitWindow(ctx.window_size.w, ctx.window_size.h, "OGL Beamformer");
	/* NOTE: do this after initing so that the window starts out floating in tiling wm */
	SetWindowState(FLAG_WINDOW_RESIZABLE);

	ctx.font_size    = 32;
	ctx.font_spacing = 0;
	ctx.font         = LoadFontEx("assets/IBMPlexSans-Bold.ttf", ctx.font_size, 0, 0);

	ctx.is.idx            = -1;
	ctx.is.cursor_blink_t = 1;

	init_fragment_shader_ctx(&ctx.fsctx, ctx.out_data_dim);

	ctx.data_pipe = os_open_named_pipe(OS_PIPE_NAME);
	ctx.params    = os_open_shared_memory_area(OS_SMEM_NAME);
	/* TODO: properly handle this? */
	ASSERT(ctx.data_pipe.file != OS_INVALID_FILE);
	ASSERT(ctx.params);

	ctx.params->raw.output_points = ctx.out_data_dim;
	/* NOTE: default compute shader pipeline */
	ctx.params->compute_stages[0]    = CS_HADAMARD;
	ctx.params->compute_stages[1]    = CS_DEMOD;
	ctx.params->compute_stages[2]    = CS_UFORCES;
	ctx.params->compute_stages[3]    = CS_MIN_MAX;
	ctx.params->compute_stages_count = 4;

	/* NOTE: Determine which graphics vendor we are running on */
	{
		const u8 *vendor = glGetString(GL_VENDOR);
		if (!vendor)
			die("Failed to determine GL Vendor\n");
		switch (vendor[0]) {
		case 'A': ctx.gl_vendor_id = GL_VENDOR_AMD;       break;
		case 'I': ctx.gl_vendor_id = GL_VENDOR_INTEL;     break;
		case 'N': ctx.gl_vendor_id = GL_VENDOR_NVIDIA;    break;
		default:  die("Unknown GL Vendor: %s\n", vendor); break;
		}
	}

	switch (ctx.gl_vendor_id) {
	case GL_VENDOR_AMD:
	case GL_VENDOR_INTEL:
		break;
	case GL_VENDOR_NVIDIA:
		g_cuda_lib_handle = os_load_library(CUDA_LIB_NAME);
		#define LOOKUP_CUDA_FN(f) \
			f = os_lookup_dynamic_symbol(g_cuda_lib_handle, #f); \
			if (!f) f = f##_stub
		LOOKUP_CUDA_FN(init_cuda_configuration);
		LOOKUP_CUDA_FN(register_cuda_buffers);
		LOOKUP_CUDA_FN(cuda_decode);
		LOOKUP_CUDA_FN(cuda_hilbert);
		break;
	}

	/* NOTE: set up OpenGL debug logging */
	glDebugMessageCallback(gl_debug_logger, NULL);
#ifdef _DEBUG
	glEnable(GL_DEBUG_OUTPUT);
#endif

	/* NOTE: allocate space for Uniform Buffer but don't send anything yet */
	glCreateBuffers(1, &ctx.csctx.shared_ubo);
	glNamedBufferStorage(ctx.csctx.shared_ubo, sizeof(BeamformerParameters), 0, GL_DYNAMIC_STORAGE_BIT);

	glGenQueries(ARRAY_COUNT(ctx.csctx.timer_fences) * CS_LAST, (u32 *)ctx.csctx.timer_ids);

	/* NOTE: do not DO_COMPUTE on first frame */
	reload_shaders(&ctx, temp_memory);
	ctx.flags &= ~DO_COMPUTE;

	ctx.flags |= ALLOC_SSBOS|ALLOC_OUT_TEX;

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
	os_remove_shared_memory(OS_SMEM_NAME);

	/* NOTE: garbage code needed for Linux */
	os_close_named_pipe(ctx.data_pipe);
}
