/* See LICENSE for license details. */
static struct {
	s8  label;
	s8  path;
	b32 needs_header;
} compute_shaders[CS_LAST] = {
	[CS_HADAMARD] = {s8("Hadamard"), s8("shaders/hadamard.glsl"), 1},
	[CS_HERCULES] = {s8("HERCULES"), s8("shaders/hercules.glsl"), 1},
	[CS_DEMOD]    = {s8("Demod"),    s8("shaders/demod.glsl"),    1},
	[CS_MIN_MAX]  = {s8("Min/Max"),  s8("shaders/min_max.glsl"),  0},
	[CS_SUM]      = {s8("Sum"),      s8("shaders/sum.glsl"),      0},
	[CS_UFORCES]  = {s8("UFORCES"),  s8("shaders/uforces.glsl"),  1},
};

#ifndef _DEBUG

#include "beamformer.c"
#define do_debug(...)

#else
static void *debug_lib;

/* TODO: move this to a header */
typedef void do_beamformer_fn(BeamformerCtx *, Arena *);
static do_beamformer_fn *do_beamformer;

static void
do_debug(Stream *error_stream)
{
	static f32 updated_time;
	FileStats test_stats = os_get_file_stats(OS_DEBUG_LIB_NAME);
	if (test_stats.filesize > 32 && test_stats.timestamp > updated_time) {
		os_unload_library(debug_lib);
		debug_lib = os_load_library(OS_DEBUG_LIB_NAME, OS_DEBUG_LIB_TEMP_NAME, error_stream);
		do_beamformer = os_lookup_dynamic_symbol(debug_lib, "do_beamformer", error_stream);
		updated_time  = test_stats.timestamp;
	}
}

#endif /* _DEBUG */

static void
gl_debug_logger(u32 src, u32 type, u32 id, u32 lvl, i32 len, const char *msg, const void *userctx)
{
	(void)src; (void)type; (void)id;

	Stream *e = (Stream *)userctx;
	stream_append_s8(e, s8("[GL DEBUG "));
	switch (lvl) {
	case GL_DEBUG_SEVERITY_HIGH:         stream_append_s8(e, s8("HIGH]: "));         break;
	case GL_DEBUG_SEVERITY_MEDIUM:       stream_append_s8(e, s8("MEDIUM]: "));       break;
	case GL_DEBUG_SEVERITY_LOW:          stream_append_s8(e, s8("LOW]: "));          break;
	case GL_DEBUG_SEVERITY_NOTIFICATION: stream_append_s8(e, s8("NOTIFICATION]: ")); break;
	default:                             stream_append_s8(e, s8("INVALID]: "));      break;
	}
	stream_append_s8(e, (s8){.len = len, .data = (u8 *)msg});
	stream_append_byte(e, '\n');
	os_write_err_msg(stream_to_s8(e));
	e->widx = 0;
}

static void
get_gl_params(GLParams *gl, Stream *err)
{
	char *vendor = (char *)glGetString(GL_VENDOR);
	if (!vendor) {
		stream_append_s8(err, s8("Failed to determine GL Vendor\n"));
		os_fatal(stream_to_s8(err));
	}
	switch (vendor[0]) {
	case 'A': gl->vendor_id = GL_VENDOR_AMD;                      break;
	case 'I': gl->vendor_id = GL_VENDOR_INTEL;                    break;
	case 'N': gl->vendor_id = GL_VENDOR_NVIDIA;                   break;
	default:
		stream_append_s8(err, s8("Unknown GL Vendor: "));
		stream_append_s8(err, cstr_to_s8(vendor));
		stream_append_byte(err, '\n');
		os_fatal(stream_to_s8(err));
		break;
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
	/* NOTE: nVidia's driver seems to misreport the version */
	b32 invalid = 0;
	if (gl->version_major < 4)
		invalid = 1;

	switch (gl->vendor_id) {
	case GL_VENDOR_AMD:
	case GL_VENDOR_INTEL:
		if (gl->version_major == 4 && gl->version_minor < 5)
			invalid = 1;
		break;
	case GL_VENDOR_NVIDIA:
		if (gl->version_major == 4 && gl->version_minor < 3)
			invalid = 1;
		break;
	}

	if (invalid)
		os_fatal(s8("Only OpenGL Versions 4.5 or newer are supported!\n"));
}

static void
dump_gl_params(GLParams *gl, Arena a)
{
	(void)gl; (void)a;
#ifdef _DEBUG
	Stream s = stream_alloc(&a, 1 * MEGABYTE);
	stream_append_s8(&s, s8("---- GL Parameters ----\n"));
	switch (gl->vendor_id) {
	case GL_VENDOR_AMD:    stream_append_s8(&s, s8("Vendor: AMD\n"));    break;
	case GL_VENDOR_INTEL:  stream_append_s8(&s, s8("Vendor: Intel\n"));  break;
	case GL_VENDOR_NVIDIA: stream_append_s8(&s, s8("Vendor: nVidia\n")); break;
	}
	stream_append_s8(&s, s8("Version: "));
	stream_append_i64(&s, gl->version_major);
	stream_append_byte(&s, '.');
	stream_append_i64(&s, gl->version_minor);
	stream_append_s8(&s, s8("\nMax 1D/2D Texture Dimension: "));
	stream_append_i64(&s, gl->max_2d_texture_dim);
	stream_append_s8(&s, s8("\nMax 3D Texture Dimension: "));
	stream_append_i64(&s, gl->max_3d_texture_dim);
	stream_append_s8(&s, s8("\nMax SSBO Size: "));
	stream_append_i64(&s, gl->max_ssbo_size);
	stream_append_s8(&s, s8("\nMax UBO Size: "));
	stream_append_i64(&s, gl->max_ubo_size);
	stream_append_s8(&s, s8("\n-----------------------\n"));
	if (!s.errors)
		os_write_err_msg(stream_to_s8(&s));
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
reload_shaders(BeamformerCtx *ctx, Arena a)
{
	ComputeShaderCtx *csctx = &ctx->csctx;
	s8 header_in_arena = push_s8(&a, s8(COMPUTE_SHADER_HEADER));
	for (u32 i = 0; i < ARRAY_COUNT(csctx->programs); i++) {
		if (!compute_shaders[i].path.len)
			continue;

		Arena tmp = a;
		FileStats fs   = os_get_file_stats((char *)compute_shaders[i].path.data);
		s8 shader_text = os_read_file(&tmp, (char *)compute_shaders[i].path.data, fs.filesize);
		if (shader_text.len == -1) {
			stream_append_s8(&ctx->error_stream, s8("failed to read shader: "));
			stream_append_s8(&ctx->error_stream, compute_shaders[i].path);
			stream_append_byte(&ctx->error_stream, '\n');
			/* TODO: maybe we don't need to fail here */
			os_fatal(stream_to_s8(&ctx->error_stream));
		}
		/* NOTE: arena works as stack (since everything here is 1 byte aligned) */
		if (compute_shaders[i].needs_header) {
			shader_text.data -= header_in_arena.len;
			shader_text.len  += header_in_arena.len;
			ASSERT(shader_text.data == header_in_arena.data);
		}

		u32 shader_id  = compile_shader(tmp, GL_COMPUTE_SHADER, shader_text);

		if (shader_id) {
			glDeleteProgram(csctx->programs[i]);
			csctx->programs[i] = rlLoadComputeShaderProgram(shader_id);
			glUseProgram(csctx->programs[csctx->programs[i]]);
			glBindBufferBase(GL_UNIFORM_BUFFER, 0, csctx->shared_ubo);
			LABEL_GL_OBJECT(GL_PROGRAM, csctx->programs[i], compute_shaders[i].label);
		}

		glDeleteShader(shader_id);
	}

	#define X(idx, name) csctx->name##_id = glGetUniformLocation(csctx->programs[idx], "u_" #name);
	CS_UNIFORMS
	#undef X

	Shader updated_fs = LoadShader(NULL, "shaders/render.glsl");
	if (updated_fs.id != rlGetShaderIdDefault()) {
		UnloadShader(ctx->fsctx.shader);
		LABEL_GL_OBJECT(GL_PROGRAM, updated_fs.id, s8("Render"));
		ctx->fsctx.shader       = updated_fs;
		ctx->fsctx.db_cutoff_id = GetShaderLocation(updated_fs, "u_db_cutoff");
		ctx->fsctx.threshold_id = GetShaderLocation(updated_fs, "u_threshold");
	}
}

static void
validate_cuda_lib(CudaLib *cl)
{
	#define X(name) if (!cl->name) cl->name = name ## _stub;
	CUDA_LIB_FNS
	#undef X
}

static void
check_and_load_cuda_lib(CudaLib *cl, Stream *error_stream)
{
	FileStats current = os_get_file_stats(OS_CUDA_LIB_NAME);
	if (cl->timestamp == current.timestamp || current.filesize < 32)
		return;

	TraceLog(LOG_INFO, "Loading CUDA lib: %s", OS_CUDA_LIB_NAME);

	cl->timestamp = current.timestamp;
	os_unload_library(cl->lib);
	cl->lib = os_load_library(OS_CUDA_LIB_NAME, OS_CUDA_LIB_TEMP_NAME, error_stream);
	#define X(name) cl->name = os_lookup_dynamic_symbol(cl->lib, #name, error_stream);
	CUDA_LIB_FNS
	#undef X
	validate_cuda_lib(cl);
}

static void
setup_beamformer(BeamformerCtx *ctx, Arena temp_memory)
{
	ctx->window_size  = (uv2){.w = 1280, .h = 840};

	SetConfigFlags(FLAG_VSYNC_HINT);
	InitWindow(ctx->window_size.w, ctx->window_size.h, "OGL Beamformer");
	/* NOTE: do this after initing so that the window starts out floating in tiling wm */
	SetWindowState(FLAG_WINDOW_RESIZABLE);
	SetWindowMinSize(INFO_COLUMN_WIDTH * 2, ctx->window_size.h);

	/* NOTE: Gather information about the GPU */
	get_gl_params(&ctx->gl, &ctx->error_stream);
	dump_gl_params(&ctx->gl, temp_memory);
	validate_gl_requirements(&ctx->gl);

	/* TODO: build these into the binary */
	ctx->font       = LoadFontEx("assets/IBMPlexSans-Bold.ttf", 28, 0, 0);
	ctx->small_font = LoadFontEx("assets/IBMPlexSans-Bold.ttf", 22, 0, 0);

	ctx->fsctx.db        = -50.0f;
	ctx->fsctx.threshold =  40.0f;

	ctx->data_pipe = os_open_named_pipe(OS_PIPE_NAME);
	ctx->params    = os_open_shared_memory_area(OS_SMEM_NAME, sizeof(*ctx->params));
	/* TODO: properly handle this? */
	ASSERT(ctx->data_pipe.file != INVALID_FILE);
	ASSERT(ctx->params);

	/* NOTE: default compute shader pipeline */
	ctx->params->compute_stages[0]    = CS_HADAMARD;
	ctx->params->compute_stages[1]    = CS_DEMOD;
	ctx->params->compute_stages[2]    = CS_UFORCES;
	ctx->params->compute_stages[3]    = CS_MIN_MAX;
	ctx->params->compute_stages_count = 4;

	/* NOTE: make sure function pointers are valid even if we are not using the cuda lib */
	validate_cuda_lib(&ctx->cuda_lib);

	/* NOTE: set up OpenGL debug logging */
	glDebugMessageCallback(gl_debug_logger, &ctx->error_stream);
#ifdef _DEBUG
	glEnable(GL_DEBUG_OUTPUT);
#endif

	/* NOTE: allocate space for Uniform Buffer but don't send anything yet */
	glCreateBuffers(1, &ctx->csctx.shared_ubo);
	glNamedBufferStorage(ctx->csctx.shared_ubo, sizeof(BeamformerParameters), 0, GL_DYNAMIC_STORAGE_BIT);
	LABEL_GL_OBJECT(GL_BUFFER, ctx->csctx.shared_ubo, s8("Beamformer_Parameters"));

	glGenQueries(ARRAY_COUNT(ctx->csctx.timer_fences) * CS_LAST, (u32 *)ctx->csctx.timer_ids);
	glGenQueries(ARRAY_COUNT(ctx->partial_compute_ctx.timer_ids), ctx->partial_compute_ctx.timer_ids);

	reload_shaders(ctx, temp_memory);
}

static void
do_program_step(BeamformerCtx *ctx, Arena *memory)
{
	do_debug(&ctx->error_stream);
	if (ctx->gl.vendor_id == GL_VENDOR_NVIDIA)
		check_and_load_cuda_lib(&ctx->cuda_lib, &ctx->error_stream);

	if (ctx->flags & RELOAD_SHADERS) {
		ctx->flags &= ~RELOAD_SHADERS;
		reload_shaders(ctx, *memory);
	}

	do_beamformer(ctx, memory);
}
