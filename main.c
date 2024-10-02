/* See LICENSE for license details. */
#include "beamformer.h"

static char *compute_shader_paths[CS_LAST] = {
	[CS_HADAMARD] = "shaders/hadamard.glsl",
	[CS_HERCULES] = "shaders/hercules.glsl",
	[CS_DEMOD]    = "shaders/demod.glsl",
	[CS_MIN_MAX]  = "shaders/min_max.glsl",
	[CS_SUM]      = "shaders/sum.glsl",
	[CS_UFORCES]  = "shaders/uforces.glsl",
};

#ifndef _DEBUG

#include "beamformer.c"
static void do_debug(void) { }

#else
static os_library_handle libhandle;

typedef void do_beamformer_fn(BeamformerCtx *, Arena);
static do_beamformer_fn *do_beamformer;

static void
do_debug(void)
{
	static os_filetime updated_time;
	os_file_stats test_stats = os_get_file_stats(OS_DEBUG_LIB_NAME);
	if (test_stats.filesize > 32 && os_filetime_is_newer(test_stats.timestamp, updated_time)) {
		os_unload_library(libhandle);
		libhandle     = os_load_library(OS_DEBUG_LIB_NAME, OS_DEBUG_LIB_TEMP_NAME);
		do_beamformer = os_lookup_dynamic_symbol(libhandle, "do_beamformer");
		updated_time  = test_stats.timestamp;
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

static void
get_gl_params(GLParams *gl)
{
	const u8 *vendor = glGetString(GL_VENDOR);
	if (!vendor)
		die("Failed to determine GL Vendor\n");
	switch (vendor[0]) {
	case 'A': gl->vendor_id = GL_VENDOR_AMD;          break;
	case 'I': gl->vendor_id = GL_VENDOR_INTEL;        break;
	case 'N': gl->vendor_id = GL_VENDOR_NVIDIA;       break;
	default:  die("Unknown GL Vendor: %s\n", vendor); break;
	}

	glGetIntegerv(GL_MAJOR_VERSION,                 &gl->version_major);
	glGetIntegerv(GL_MINOR_VERSION,                 &gl->version_minor);
	glGetIntegerv(GL_MAX_TEXTURE_SIZE,              &gl->max_2d_texture_dim);
	glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE,           &gl->max_3d_texture_dim);
	glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &gl->max_ssbo_size);
	glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE,        &gl->max_ubo_size);
}

static void
validate_gl_requirements(GLParams *gl)
{
	ASSERT(gl->max_ubo_size >= sizeof(BeamformerParameters));
	if (gl->version_major < 4 || (gl->version_major == 4 && gl->version_minor < 5))
		die("Only OpenGL Versions 4.5 or newer are supported!\n");
}

static void
dump_gl_params(GLParams *gl)
{
	(void)gl;
#ifdef _DEBUG
	fputs("---- GL Parameters ----\n", stdout);
	switch (gl->vendor_id) {
	case GL_VENDOR_AMD:    fputs("Vendor: AMD\n",    stdout); break;
	case GL_VENDOR_INTEL:  fputs("Vendor: Intel\n",  stdout); break;
	case GL_VENDOR_NVIDIA: fputs("Vendor: nVidia\n", stdout); break;
	}
	printf("Version: %d.%d\n", gl->version_major, gl->version_minor);
	printf("Max 1D/2D Texture Dimension: %d\n", gl->max_2d_texture_dim);
	printf("Max 3D Texture Dimension: %d\n", gl->max_3d_texture_dim);
	printf("Max SSBO Size: %d\n", gl->max_ssbo_size);
	printf("Max UBO Size: %d\n", gl->max_ubo_size);
	fputs("-----------------------\n", stdout);
#endif
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

	csctx->volume_export_pass_id       = glGetUniformLocation(csctx->programs[CS_HERCULES],
	                                                          "u_volume_export_pass");
	csctx->volume_export_dim_offset_id = glGetUniformLocation(csctx->programs[CS_HERCULES],
	                                                         "u_volume_export_dim_offset");
	csctx->xdc_transform_id            = glGetUniformLocation(csctx->programs[CS_UFORCES],
	                                                          "u_xdc_transform");
	csctx->xdc_index_id                = glGetUniformLocation(csctx->programs[CS_UFORCES],
	                                                          "u_xdc_index");

	csctx->mips_level_id   = glGetUniformLocation(csctx->programs[CS_MIN_MAX], "u_mip_map");

	csctx->sum_prescale_id = glGetUniformLocation(csctx->programs[CS_SUM], "u_prescale");

	Shader updated_fs = LoadShader(NULL, "shaders/render.glsl");
	if (updated_fs.id != rlGetShaderIdDefault()) {
		UnloadShader(ctx->fsctx.shader);
		ctx->fsctx.shader       = updated_fs;
		ctx->fsctx.db_cutoff_id = GetShaderLocation(updated_fs, "u_db_cutoff");
	}
}

static void
validate_cuda_lib(CudaLib *cl)
{
	if (!cl->init_cuda_configuration) cl->init_cuda_configuration = init_cuda_configuration_stub;
	if (!cl->register_cuda_buffers)   cl->register_cuda_buffers   = register_cuda_buffers_stub;
	if (!cl->cuda_decode)             cl->cuda_decode             = cuda_decode_stub;
	if (!cl->cuda_hilbert)            cl->cuda_hilbert            = cuda_hilbert_stub;
}

static void
check_and_load_cuda_lib(CudaLib *cl)
{
	os_file_stats current = os_get_file_stats(OS_CUDA_LIB_NAME);
	if (!os_filetime_is_newer(current.timestamp, cl->timestamp) || current.filesize < 32)
		return;

	TraceLog(LOG_INFO, "Loading CUDA lib: %s", OS_CUDA_LIB_NAME);

	cl->timestamp = current.timestamp;
	os_unload_library(cl->lib);
	cl->lib = os_load_library(OS_CUDA_LIB_NAME, OS_CUDA_LIB_TEMP_NAME);

	cl->init_cuda_configuration = os_lookup_dynamic_symbol(cl->lib, "init_cuda_configuration");
	cl->register_cuda_buffers   = os_lookup_dynamic_symbol(cl->lib, "register_cuda_buffers");
	cl->cuda_decode             = os_lookup_dynamic_symbol(cl->lib, "cuda_decode");
	cl->cuda_hilbert            = os_lookup_dynamic_symbol(cl->lib, "cuda_hilbert");

	validate_cuda_lib(cl);
}

int
main(void)
{
	BeamformerCtx ctx = {0};

	Arena temp_memory = os_alloc_arena((Arena){0}, 8 * MEGABYTE);

	ctx.window_size  = (uv2){.w = 1280, .h = 840};

	ctx.out_data_dim          = (uv4){.x = 1, .y = 1, .z = 1};
	ctx.export_ctx.volume_dim = (uv4){.x = 1, .y = 1, .z = 1};

	SetConfigFlags(FLAG_VSYNC_HINT);
	InitWindow(ctx.window_size.w, ctx.window_size.h, "OGL Beamformer");
	/* NOTE: do this after initing so that the window starts out floating in tiling wm */
	SetWindowState(FLAG_WINDOW_RESIZABLE);
	SetWindowMinSize(INFO_COLUMN_WIDTH * 2, ctx.window_size.h);

	/* NOTE: Gather information about the GPU */
	get_gl_params(&ctx.gl);
	dump_gl_params(&ctx.gl);
	validate_gl_requirements(&ctx.gl);

	/* TODO: build these into the binary */
	ctx.font       = LoadFontEx("assets/IBMPlexSans-Bold.ttf", 28, 0, 0);
	ctx.small_font = LoadFontEx("assets/IBMPlexSans-Bold.ttf", 22, 0, 0);

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

	/* NOTE: make sure function pointers are valid even if we are not using the cuda lib */
	validate_cuda_lib(&ctx.cuda_lib);

	/* NOTE: set up OpenGL debug logging */
	glDebugMessageCallback(gl_debug_logger, NULL);
#ifdef _DEBUG
	glEnable(GL_DEBUG_OUTPUT);
#endif

	/* NOTE: allocate space for Uniform Buffer but don't send anything yet */
	glCreateBuffers(1, &ctx.csctx.shared_ubo);
	glNamedBufferStorage(ctx.csctx.shared_ubo, sizeof(BeamformerParameters), 0, GL_DYNAMIC_STORAGE_BIT);

	glGenQueries(ARRAY_COUNT(ctx.csctx.timer_fences) * CS_LAST, (u32 *)ctx.csctx.timer_ids);
	glGenQueries(ARRAY_COUNT(ctx.export_ctx.timer_ids), ctx.export_ctx.timer_ids);

	/* NOTE: do not DO_COMPUTE on first frame */
	reload_shaders(&ctx, temp_memory);
	ctx.flags &= ~DO_COMPUTE;

	while(!WindowShouldClose()) {
		do_debug();
		if (ctx.gl.vendor_id == GL_VENDOR_NVIDIA)
			check_and_load_cuda_lib(&ctx.cuda_lib);

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
