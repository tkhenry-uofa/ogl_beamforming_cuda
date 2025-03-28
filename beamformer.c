/* See LICENSE for license details. */
#include "beamformer.h"

static f32 dt_for_frame;
static u32 cycle_t;

#ifndef _DEBUG
#define start_renderdoc_capture(...)
#define end_renderdoc_capture(...)
#else
static renderdoc_start_frame_capture_fn *start_frame_capture;
static renderdoc_end_frame_capture_fn   *end_frame_capture;
#define start_renderdoc_capture(gl) if (start_frame_capture) start_frame_capture(gl, 0)
#define end_renderdoc_capture(gl)   if (end_frame_capture)   end_frame_capture(gl, 0)
#endif

static iz
decoded_data_size(ComputeShaderCtx *cs)
{
	uv4 dim    = cs->dec_data_dim;
	iz  result = 2 * sizeof(f32) * dim.x * dim.y * dim.z;
	return result;
}

static uv3
make_valid_test_dim(uv3 in)
{
	uv3 result;
	result.x = MAX(in.x, 1);
	result.y = MAX(in.y, 1);
	result.z = MAX(in.z, 1);
	return result;
}

static BeamformFrameIterator
beamform_frame_iterator(BeamformerCtx *ctx, u32 start_index, u32 needed_frames)
{
	start_index = start_index % ARRAY_COUNT(ctx->beamform_frames);

	BeamformFrameIterator result;
	result.frames        = ctx->beamform_frames;
	result.offset        = start_index;
	result.capacity      = ARRAY_COUNT(ctx->beamform_frames);
	result.cursor        = 0;
	result.needed_frames = needed_frames;
	return result;
}

static BeamformFrame *
frame_next(BeamformFrameIterator *bfi)
{
	BeamformFrame *result = 0;
	if (bfi->cursor != bfi->needed_frames) {
		u32 index = (bfi->offset + bfi->cursor++) % bfi->capacity;
		result    = bfi->frames + index;
	}
	return result;
}

static void
alloc_beamform_frame(GLParams *gp, BeamformFrame *out, ComputeShaderStats *out_stats,
                     uv3 out_dim, s8 name, Arena arena)
{
	out->ready_to_present = 0;

	out->dim.x = MAX(1, round_down_power_of_2(ORONE(out_dim.x)));
	out->dim.y = MAX(1, round_down_power_of_2(ORONE(out_dim.y)));
	out->dim.z = MAX(1, round_down_power_of_2(ORONE(out_dim.z)));

	if (gp) {
		out->dim.x = MIN(out->dim.x, gp->max_3d_texture_dim);
		out->dim.y = MIN(out->dim.y, gp->max_3d_texture_dim);
		out->dim.z = MIN(out->dim.z, gp->max_3d_texture_dim);
	}

	/* NOTE: allocate storage for beamformed output data;
	 * this is shared between compute and fragment shaders */
	u32 max_dim = MAX(out->dim.x, MAX(out->dim.y, out->dim.z));
	out->mips   = ctz_u32(max_dim) + 1;

	Stream label = arena_stream(&arena);
	stream_append_s8(&label, name);
	stream_append_byte(&label, '[');
	stream_append_hex_u64(&label, out->id);
	stream_append_byte(&label, ']');

	glDeleteTextures(1, &out->texture);
	glCreateTextures(GL_TEXTURE_3D, 1, &out->texture);
	glTextureStorage3D(out->texture, out->mips, GL_RG32F, out->dim.x, out->dim.y, out->dim.z);
	LABEL_GL_OBJECT(GL_TEXTURE, out->texture, stream_to_s8(&label));

	if (out_stats) {
		glDeleteQueries(ARRAY_COUNT(out_stats->timer_ids), out_stats->timer_ids);
		glCreateQueries(GL_TIME_ELAPSED, ARRAY_COUNT(out_stats->timer_ids), out_stats->timer_ids);
	}
}

static void
alloc_shader_storage(BeamformerCtx *ctx, Arena a)
{
	ComputeShaderCtx *cs     = &ctx->csctx;
	BeamformerParameters *bp = &ctx->params->raw;

	uv4 dec_data_dim = bp->dec_data_dim;
	u32 rf_raw_size  = ctx->params->raw_data_size;
	cs->dec_data_dim = dec_data_dim;
	cs->rf_raw_size  = rf_raw_size;

	glDeleteBuffers(ARRAY_COUNT(cs->rf_data_ssbos), cs->rf_data_ssbos);
	glCreateBuffers(ARRAY_COUNT(cs->rf_data_ssbos), cs->rf_data_ssbos);

	i32 storage_flags = GL_DYNAMIC_STORAGE_BIT;
	switch (ctx->gl.vendor_id) {
	case GL_VENDOR_AMD:
	case GL_VENDOR_ARM:
	case GL_VENDOR_INTEL:
		if (cs->raw_data_ssbo)
			glUnmapNamedBuffer(cs->raw_data_ssbo);
		storage_flags |= GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT;
	case GL_VENDOR_NVIDIA:
		/* NOTE: register_cuda_buffers will handle the updated ssbo */
		break;
	}

	glDeleteBuffers(1, &cs->raw_data_ssbo);
	glCreateBuffers(1, &cs->raw_data_ssbo);
	glNamedBufferStorage(cs->raw_data_ssbo, rf_raw_size, 0, storage_flags);
	LABEL_GL_OBJECT(GL_BUFFER, cs->raw_data_ssbo, s8("Raw_RF_SSBO"));

	iz rf_decoded_size = decoded_data_size(cs);
	Stream label = stream_alloc(&a, 256);
	stream_append_s8(&label, s8("Decoded_RF_SSBO_"));
	u32 s_widx = label.widx;
	for (u32 i = 0; i < ARRAY_COUNT(cs->rf_data_ssbos); i++) {
		glNamedBufferStorage(cs->rf_data_ssbos[i], rf_decoded_size, 0, 0);
		stream_append_u64(&label, i);
		s8 rf_label = stream_to_s8(&label);
		LABEL_GL_OBJECT(GL_BUFFER, cs->rf_data_ssbos[i], rf_label);
		stream_reset(&label, s_widx);
	}

	i32 map_flags = GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT|GL_MAP_UNSYNCHRONIZED_BIT;
	switch (ctx->gl.vendor_id) {
	case GL_VENDOR_AMD:
	case GL_VENDOR_ARM:
	case GL_VENDOR_INTEL:
		cs->raw_data_arena.beg = glMapNamedBufferRange(cs->raw_data_ssbo, 0,
		                                               rf_raw_size, map_flags);
		cs->raw_data_arena.end = cs->raw_data_arena.beg + rf_raw_size;
		break;
	case GL_VENDOR_NVIDIA:
		cs->raw_data_arena = ctx->os.alloc_arena(cs->raw_data_arena, rf_raw_size);
		ctx->cuda_lib.register_cuda_buffers(cs->rf_data_ssbos, ARRAY_COUNT(cs->rf_data_ssbos),
		                                    cs->raw_data_ssbo);
		ctx->cuda_lib.init_cuda_configuration(bp->rf_raw_dim.E, bp->dec_data_dim.E,
		                                      bp->channel_mapping);
		break;
	}

	/* NOTE: store hadamard in GPU once; it won't change for a particular imaging session */
	iz   hadamard_elements = dec_data_dim.z * dec_data_dim.z;
	i32  *hadamard         = alloc(&a, i32, hadamard_elements);
	i32  *tmp              = alloc(&a, i32, hadamard_elements);
	fill_hadamard_transpose(hadamard, tmp, dec_data_dim.z);
	glDeleteTextures(1, &cs->hadamard_texture);
	glCreateTextures(GL_TEXTURE_2D, 1, &cs->hadamard_texture);
	glTextureStorage2D(cs->hadamard_texture, 1, GL_R8I, dec_data_dim.z, dec_data_dim.z);
	glTextureSubImage2D(cs->hadamard_texture, 0, 0, 0, dec_data_dim.z, dec_data_dim.z,
	                    GL_RED_INTEGER, GL_INT, hadamard);
	LABEL_GL_OBJECT(GL_TEXTURE, cs->hadamard_texture, s8("Hadamard_Matrix"));
}

static BeamformWork *
beamform_work_queue_pop(BeamformWorkQueue *q)
{
	BeamformWork *result = 0;

	static_assert(ISPOWEROF2(ARRAY_COUNT(q->work_items)), "queue capacity must be a power of 2");
	u64 val  = atomic_load(&q->queue);
	u64 mask = ARRAY_COUNT(q->work_items) - 1;
	u32 widx = val       & mask;
	u32 ridx = val >> 32 & mask;

	if (ridx != widx)
		result = q->work_items + ridx;

	return result;
}

static void
beamform_work_queue_pop_commit(BeamformWorkQueue *q)
{
	atomic_add(&q->queue, 0x100000000ULL);
}

DEBUG_EXPORT BEAMFORM_WORK_QUEUE_PUSH_FN(beamform_work_queue_push)
{
	BeamformWork *result = 0;

	static_assert(ISPOWEROF2(ARRAY_COUNT(q->work_items)), "queue capacity must be a power of 2");
	u64 val  = atomic_load(&q->queue);
	u64 mask = ARRAY_COUNT(q->work_items) - 1;
	u32 widx = val       & mask;
	u32 ridx = val >> 32 & mask;
	u32 next = (widx + 1) & mask;

	if (val & 0x80000000)
		atomic_and(&q->queue, ~0x80000000);

	if (next != ridx) {
		result = q->work_items + widx;
		zero_struct(result);
	}

	return result;
}

DEBUG_EXPORT BEAMFORM_WORK_QUEUE_PUSH_COMMIT_FN(beamform_work_queue_push_commit)
{
	atomic_add(&q->queue, 1);
}

static b32
fill_frame_compute_work(BeamformerCtx *ctx, BeamformWork *work)
{
	b32 result = 0;
	if (work) {
		result = 1;
		u32 frame_id      = atomic_inc(&ctx->next_render_frame_index, 1);
		u32 frame_index   = frame_id % ARRAY_COUNT(ctx->beamform_frames);
		work->type        = BW_COMPUTE;
		work->frame.store = ctx->beamform_frames + frame_index;
		work->frame.stats = ctx->beamform_frame_compute_stats + frame_index;
		work->frame.store->ready_to_present = 0;
		work->frame.store->id = frame_id;
	}
	return result;
}

static void
export_frame(BeamformerCtx *ctx, iptr handle, BeamformFrame *frame)
{
	uv3 dim            = frame->dim;
	iz  out_size       = dim.x * dim.y * dim.z * 2 * sizeof(f32);
	ctx->export_buffer = ctx->os.alloc_arena(ctx->export_buffer, out_size);
	glGetTextureImage(frame->texture, 0, GL_RG, GL_FLOAT, out_size, ctx->export_buffer.beg);
	s8 raw = {.len = out_size, .data = ctx->export_buffer.beg};
	if (!ctx->os.write_file(handle, raw))
		ctx->os.write_file(ctx->os.stderr, s8("failed to export frame\n"));
	ctx->os.close(handle);
}

static void
do_sum_shader(ComputeShaderCtx *cs, u32 *in_textures, u32 in_texture_count, f32 in_scale,
              u32 out_texture, uv3 out_data_dim)
{
	/* NOTE: zero output before summing */
	glClearTexImage(out_texture, 0, GL_RED, GL_FLOAT, 0);
	glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);

	glBindImageTexture(0, out_texture, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RG32F);
	glUniform1f(cs->sum_prescale_id, in_scale);
	for (u32 i = 0; i < in_texture_count; i++) {
		glBindImageTexture(1, in_textures[i], 0, GL_TRUE, 0, GL_READ_ONLY, GL_RG32F);
		glDispatchCompute(ORONE(out_data_dim.x / 32),
		                  ORONE(out_data_dim.y),
		                  ORONE(out_data_dim.z / 32));
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	}
}

struct compute_cursor {
	iv3 cursor;
	iv3 dispatch;
	iv3 target;
	u32 points_per_dispatch;
	u32 completed_points;
	u32 total_points;
};

static struct compute_cursor
start_compute_cursor(uv3 dim, u32 max_points)
{
	struct compute_cursor result = {0};
	u32 invocations_per_dispatch = DAS_LOCAL_SIZE_X * DAS_LOCAL_SIZE_Y * DAS_LOCAL_SIZE_Z;

	result.dispatch.y = MIN(max_points / invocations_per_dispatch, MAX(dim.y / DAS_LOCAL_SIZE_Y, 1));

	u32 remaining     = max_points / result.dispatch.y;
	result.dispatch.x = MIN(remaining / invocations_per_dispatch, MAX(dim.x / DAS_LOCAL_SIZE_X, 1));
	result.dispatch.z = MIN(remaining / (invocations_per_dispatch * result.dispatch.x),
	                        MAX(dim.z / DAS_LOCAL_SIZE_Z, 1));

	result.target.x = MAX(dim.x / result.dispatch.x / DAS_LOCAL_SIZE_X, 1);
	result.target.y = MAX(dim.y / result.dispatch.y / DAS_LOCAL_SIZE_Y, 1);
	result.target.z = MAX(dim.z / result.dispatch.z / DAS_LOCAL_SIZE_Z, 1);

	result.points_per_dispatch = 1;
	result.points_per_dispatch *= result.dispatch.x * DAS_LOCAL_SIZE_X;
	result.points_per_dispatch *= result.dispatch.y * DAS_LOCAL_SIZE_Y;
	result.points_per_dispatch *= result.dispatch.z * DAS_LOCAL_SIZE_Z;

	result.total_points = dim.x * dim.y * dim.z;

	return result;
}

static iv3
step_compute_cursor(struct compute_cursor *cursor)
{
	cursor->cursor.x += 1;
	if (cursor->cursor.x >= cursor->target.x) {
		cursor->cursor.x  = 0;
		cursor->cursor.y += 1;
		if (cursor->cursor.y >= cursor->target.y) {
			cursor->cursor.y  = 0;
			cursor->cursor.z += 1;
		}
	}

	cursor->completed_points += cursor->points_per_dispatch;

	iv3 result = cursor->cursor;
	result.x *= cursor->dispatch.x * DAS_LOCAL_SIZE_X;
	result.y *= cursor->dispatch.y * DAS_LOCAL_SIZE_Y;
	result.z *= cursor->dispatch.z * DAS_LOCAL_SIZE_Z;

	return result;
}

static b32
compute_cursor_finished(struct compute_cursor *cursor)
{
	b32 result = cursor->completed_points >= cursor->total_points;
	return result;
}

static void
do_compute_shader(BeamformerCtx *ctx, Arena arena, BeamformFrame *frame, ComputeShaderID shader)
{
	ComputeShaderCtx *csctx = &ctx->csctx;

	glUseProgram(csctx->programs[shader]);

	u32 output_ssbo_idx = !csctx->last_output_ssbo_index;
	u32 input_ssbo_idx  = csctx->last_output_ssbo_index;

	switch (shader) {
	case CS_DECODE:
	case CS_DECODE_FLOAT:
	case CS_DECODE_FLOAT_COMPLEX:
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, csctx->raw_data_ssbo);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, csctx->rf_data_ssbos[output_ssbo_idx]);
		glBindImageTexture(0, csctx->hadamard_texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R8I);
		glDispatchCompute(ORONE(csctx->dec_data_dim.x / 32),
		                  ORONE(csctx->dec_data_dim.y / 32),
		                  ORONE(csctx->dec_data_dim.z));
		csctx->last_output_ssbo_index = !csctx->last_output_ssbo_index;
		break;
	case CS_CUDA_DECODE:
		ctx->cuda_lib.cuda_decode(0, output_ssbo_idx, 0);
		csctx->last_output_ssbo_index = !csctx->last_output_ssbo_index;
		break;
	case CS_CUDA_HILBERT:
		ctx->cuda_lib.cuda_hilbert(input_ssbo_idx, output_ssbo_idx);
		csctx->last_output_ssbo_index = !csctx->last_output_ssbo_index;
		break;
	case CS_DEMOD:
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, csctx->rf_data_ssbos[input_ssbo_idx]);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, csctx->rf_data_ssbos[output_ssbo_idx]);
		glDispatchCompute(ORONE(csctx->dec_data_dim.x / 32),
		                  ORONE(csctx->dec_data_dim.y / 32),
		                  ORONE(csctx->dec_data_dim.z));
		csctx->last_output_ssbo_index = !csctx->last_output_ssbo_index;
		break;
	case CS_MIN_MAX: {
		u32 texture = frame->texture;
		for (u32 i = 1; i < frame->mips; i++) {
			glBindImageTexture(0, texture, i - 1, GL_TRUE, 0, GL_READ_ONLY,  GL_RG32F);
			glBindImageTexture(1, texture, i - 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RG32F);
			glUniform1i(csctx->mips_level_id, i);

			u32 width  = frame->dim.x >> i;
			u32 height = frame->dim.y >> i;
			u32 depth  = frame->dim.z >> i;
			glDispatchCompute(ORONE(width / 32), ORONE(height), ORONE(depth / 32));
			glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
		}
	} break;
	case CS_DAS: {
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, csctx->rf_data_ssbos[input_ssbo_idx]);
		glBindImageTexture(0, frame->texture, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RG32F);

		#if 1
		/* TODO(rnp): compute max_points_per_dispatch based on something like a
		 * transmit_count * channel_count product */
		u32 max_points_per_dispatch = KB(64);
		struct compute_cursor cursor = start_compute_cursor(frame->dim, max_points_per_dispatch);
		f32 percent_per_step = (f32)cursor.points_per_dispatch / (f32)cursor.total_points;
		csctx->processing_progress = -percent_per_step;
		for (iv3 offset = {0};
		     !compute_cursor_finished(&cursor);
		     offset = step_compute_cursor(&cursor))
		{
			csctx->processing_progress += percent_per_step;
			/* IMPORTANT(rnp): prevents OS from coalescing and killing our shader */
			glFinish();
			glUniform3iv(csctx->voxel_offset_id, 1, offset.E);
			glDispatchCompute(cursor.dispatch.x, cursor.dispatch.y, cursor.dispatch.z);
		}
		#else
		/* NOTE(rnp): use this for testing tiling code. The performance of the above path
		 * should be the same as this path if everything is working correctly */
		iv3 compute_dim_offset = {0};
		glUniform3iv(csctx->voxel_offset_id, 1, compute_dim_offset.E);
		glDispatchCompute(ORONE(frame->dim.x / 32),
		                  ORONE(frame->dim.y),
		                  ORONE(frame->dim.z / 32));
		#endif
		glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT|GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	} break;
	case CS_SUM: {
		u32 aframe_index = ctx->averaged_frame_index % ARRAY_COUNT(ctx->averaged_frames);
		BeamformFrame *aframe    = ctx->averaged_frames + aframe_index;
		aframe->ready_to_present = 0;
		/* TODO(rnp): hack we need a better way of specifying which frames to sum;
		 * this is fine for rolling averaging but what if we want to do something else */
		ASSERT(frame >= ctx->beamform_frames);
		ASSERT(frame < ctx->beamform_frames + ARRAY_COUNT(ctx->beamform_frames));
		u32 base_index   = (u32)(frame - ctx->beamform_frames);
		u32 to_average   = ctx->params->raw.output_points.w;
		u32 frame_count  = 0;
		u32 *in_textures = alloc(&arena, u32, MAX_BEAMFORMED_SAVED_FRAMES);
		BeamformFrameIterator bfi = beamform_frame_iterator(ctx, 1 + base_index - to_average,
		                                                    to_average);
		for (BeamformFrame *it = frame_next(&bfi); it; it = frame_next(&bfi))
			in_textures[frame_count++] = it->texture;

		ASSERT(to_average == frame_count);

		do_sum_shader(csctx, in_textures, frame_count, 1 / (f32)frame_count,
		              aframe->texture, aframe->dim);
		aframe->min_coordinate = frame->min_coordinate;
		aframe->max_coordinate = frame->max_coordinate;
		aframe->compound_count = frame->compound_count;
		aframe->das_shader_id  = frame->das_shader_id;
	} break;
	default: ASSERT(0);
	}
}

static u32
compile_shader(OS *os, Arena a, u32 type, s8 shader, s8 name)
{
	u32 sid = glCreateShader(type);
	glShaderSource(sid, 1, (const char **)&shader.data, (int *)&shader.len);
	glCompileShader(sid);

	i32 res = 0;
	glGetShaderiv(sid, GL_COMPILE_STATUS, &res);

	if (res == GL_FALSE) {
		Stream buf = arena_stream(&a);
		stream_append_s8(&buf, name);
		stream_append_s8(&buf, s8(": failed to compile\n"));

		i32 len = 0, out_len = 0;
		glGetShaderiv(sid, GL_INFO_LOG_LENGTH, &len);
		glGetShaderInfoLog(sid, len, &out_len, (char *)(buf.data + buf.widx));
		stream_commit(&buf, out_len);
		glDeleteShader(sid);
		os->write_file(os->stderr, stream_to_s8(&buf));

		sid = 0;
	}

	return sid;
}

static u32
link_program(OS *os, Arena a, u32 shader_id)
{
	i32 success = 0;
	u32 result  = glCreateProgram();
	glAttachShader(result, shader_id);
	glLinkProgram(result);
	glGetProgramiv(result, GL_LINK_STATUS, &success);
	if (success == GL_FALSE) {
		i32 len    = 0;
		Stream buf = arena_stream(&a);
		stream_append_s8(&buf, s8("shader link error: "));
		glGetProgramInfoLog(result, buf.cap - buf.widx, &len, (c8 *)(buf.data + buf.widx));
		stream_reset(&buf, len);
		stream_append_byte(&buf, '\n');
		os->write_file(os->stderr, stream_to_s8(&buf));
		glDeleteProgram(result);
		result = 0;
	}
	return result;
}

static s8
push_compute_shader_header(Arena *a, ComputeShaderID shader)
{
	s8 result = {.data = a->beg};

	#define X(name, type, size, gltype, glsize, comment) "\t" #gltype " " #name #glsize "; " comment "\n"
	push_s8(a, s8("#version 460 core\n\n"
	              "layout(std140, binding = 0) uniform parameters {\n"
	              BEAMFORMER_PARAMS_HEAD
	              BEAMFORMER_UI_PARAMS
	              BEAMFORMER_PARAMS_TAIL
	              "};\n\n"));
	#undef X

	switch (shader) {
	case CS_DAS: {
		push_s8(a, s8("layout("
		              "local_size_x = " str(DAS_LOCAL_SIZE_X) ", "
		              "local_size_y = " str(DAS_LOCAL_SIZE_Y) ", "
		              "local_size_z = " str(DAS_LOCAL_SIZE_Z) ") "
		              "in;\n\n"));
		#define X(type, id, pretty, fixed_tx) push_s8(a, s8("#define DAS_ID_" #type " " #id "\n"));
		DAS_TYPES
		#undef X
	} break;
	case CS_DECODE_FLOAT:
	case CS_DECODE_FLOAT_COMPLEX: {
		if (shader == CS_DECODE_FLOAT) push_s8(a, s8("#define INPUT_DATA_TYPE_FLOAT\n\n"));
		else                           push_s8(a, s8("#define INPUT_DATA_TYPE_FLOAT_COMPLEX\n\n"));
	} /* FALLTHROUGH */
	case CS_DECODE: {
		#define X(type, id, pretty) push_s8(a, s8("#define DECODE_MODE_" #type " " #id "\n"));
		DECODE_TYPES
		#undef X
	} break;
	default: break;
	}
	s8 end = push_s8(a, s8("\n#line 1\n"));
	result.len = end.data + end.len - result.data;
	return result;
}

static b32
reload_compute_shader(BeamformerCtx *ctx, s8 path, s8 extra, ComputeShaderReloadContext *csr, Arena tmp)
{
	ComputeShaderCtx *cs = &ctx->csctx;
	b32 result = 0;

	/* NOTE: arena works as stack (since everything here is 1 byte aligned) */
	s8 header = {.data = tmp.beg};
	if (csr->needs_header)
		header = push_compute_shader_header(&tmp, csr->shader);

	s8 shader_text = ctx->os.read_whole_file(&tmp, (c8 *)path.data);
	shader_text.data -= header.len;
	shader_text.len  += header.len;

	if (shader_text.data == header.data) {
		u32 shader_id  = compile_shader(&ctx->os, tmp, GL_COMPUTE_SHADER, shader_text, path);
		if (shader_id) {
			u32 new_program = link_program(&ctx->os, tmp, shader_id);
			if (new_program) {
				Stream buf = arena_stream(&tmp);
				stream_append_s8(&buf, s8("loaded: "));
				stream_append_s8(&buf, path);
				stream_append_s8(&buf, extra);
				stream_append_byte(&buf, '\n');
				ctx->os.write_file(ctx->os.stderr, stream_to_s8(&buf));
				glDeleteProgram(cs->programs[csr->shader]);
				cs->programs[csr->shader] = new_program;
				glUseProgram(cs->programs[csr->shader]);
				glBindBufferBase(GL_UNIFORM_BUFFER, 0, cs->shared_ubo);
				LABEL_GL_OBJECT(GL_PROGRAM, cs->programs[csr->shader], csr->label);
				result = 1;
			}
			glDeleteShader(shader_id);
		}
	} else {
		Stream buf = arena_stream(&tmp);
		stream_append_s8(&buf, s8("failed to load: "));
		stream_append_s8(&buf, path);
		stream_append_s8(&buf, extra);
		stream_append_byte(&buf, '\n');
		ctx->os.write_file(ctx->os.stderr, stream_to_s8(&buf));
	}

	return result;
}

DEBUG_EXPORT BEAMFORMER_COMPLETE_COMPUTE_FN(beamformer_complete_compute)
{
	BeamformerCtx *ctx   = (BeamformerCtx *)user_context;
	BeamformWorkQueue *q = ctx->beamform_work_queue;
	BeamformWork *work   = beamform_work_queue_pop(q);
	ComputeShaderCtx *cs = &ctx->csctx;

	BeamformerParameters *bp = &ctx->params->raw;

	while (work) {
		b32 can_commit = 1;
		switch (work->type) {
		case BW_RELOAD_SHADER: {
			ComputeShaderReloadContext *csr = work->reload_shader_ctx;
			b32 success = reload_compute_shader(ctx, csr->path, s8(""), csr, arena);
			if (csr->shader == CS_DECODE) {
				/* TODO(rnp): think of a better way of doing this */
				csr->shader = CS_DECODE_FLOAT_COMPLEX;
				success &= reload_compute_shader(ctx, csr->path, s8(" (F32C)"), csr, arena);
				csr->shader = CS_DECODE_FLOAT;
				success &= reload_compute_shader(ctx, csr->path, s8(" (F32)"),  csr, arena);
				csr->shader = CS_DECODE;
			}

			if (success) {
				if (ctx->csctx.raw_data_ssbo) {
					can_commit = 0;
					fill_frame_compute_work(ctx, work);
				}

				/* TODO(rnp): remove this */
				#define X(idx, name) cs->name##_id = glGetUniformLocation(cs->programs[idx], "u_" #name);
				CS_UNIFORMS
				#undef X
			}
		} break;
		case BW_LOAD_RF_DATA: {
			if (cs->rf_raw_size != ctx->params->raw_data_size ||
			    !uv4_equal(cs->dec_data_dim, bp->dec_data_dim))
			{
				alloc_shader_storage(ctx, arena);
			}

			void *rf_data_buf = cs->raw_data_arena.beg;
			iz rlen = ctx->os.read_file(work->file_handle, rf_data_buf, cs->rf_raw_size);
			if (rlen != cs->rf_raw_size) {
				stream_append_s8(&ctx->error_stream, s8("Partial Read Occurred: "));
				stream_append_i64(&ctx->error_stream, rlen);
				stream_append_byte(&ctx->error_stream, '/');
				stream_append_i64(&ctx->error_stream, cs->rf_raw_size);
				stream_append_byte(&ctx->error_stream, '\n');
				ctx->os.write_file(ctx->os.stderr, stream_to_s8(&ctx->error_stream));
				ctx->error_stream.widx = 0;
			} else {
				switch (ctx->gl.vendor_id) {
				case GL_VENDOR_AMD:
				case GL_VENDOR_ARM:
				case GL_VENDOR_INTEL:
					break;
				case GL_VENDOR_NVIDIA:
					glNamedBufferSubData(cs->raw_data_ssbo, 0, rlen, rf_data_buf);
				}
			}
			ctx->ready_for_rf = 1;
		} break;
		case BW_COMPUTE: {
			atomic_store(&cs->processing_compute, 1);
			start_renderdoc_capture(gl_context);

			BeamformerWorkFrame *frame = &work->frame;
			if (ctx->params->upload) {
				glNamedBufferSubData(cs->shared_ubo, 0, sizeof(ctx->params->raw),
				                     &ctx->params->raw);
				ctx->params->upload = 0;
			}

			if (cs->programs[CS_DAS])
				glProgramUniform1ui(cs->programs[CS_DAS], cs->cycle_t_id, cycle_t++);

			uv3 try_dim = make_valid_test_dim(ctx->params->raw.output_points.xyz);
			if (!uv3_equal(try_dim, frame->store->dim))
				alloc_beamform_frame(&ctx->gl, frame->store, frame->stats, try_dim,
				                     s8("Beamformed_Data"), arena);

			if (ctx->params->raw.output_points.w > 1) {
				if (!uv3_equal(try_dim, ctx->averaged_frames[0].dim)) {
					alloc_beamform_frame(&ctx->gl, ctx->averaged_frames + 0,
					                     ctx->averaged_frame_compute_stats + 0,
					                     try_dim, s8("Averaged Frame"), arena);
					alloc_beamform_frame(&ctx->gl, ctx->averaged_frames + 1,
					                     ctx->averaged_frame_compute_stats + 1,
					                     try_dim, s8("Averaged Frame"), arena);
				}
			}

			frame->store->in_flight      = 1;
			frame->store->min_coordinate = ctx->params->raw.output_min_coordinate;
			frame->store->max_coordinate = ctx->params->raw.output_max_coordinate;
			frame->store->das_shader_id  = ctx->params->raw.das_shader_id;
			frame->store->compound_count = ctx->params->raw.dec_data_dim.z;

			b32 did_sum_shader = 0;
			u32 stage_count = ctx->params->compute_stages_count;
			ComputeShaderID *stages = ctx->params->compute_stages;
			for (u32 i = 0; i < stage_count; i++) {
				did_sum_shader |= stages[i] == CS_SUM;
				frame->stats->timer_active[stages[i]] = 1;
				glBeginQuery(GL_TIME_ELAPSED, frame->stats->timer_ids[stages[i]]);
				do_compute_shader(ctx, arena, frame->store, stages[i]);
				glEndQuery(GL_TIME_ELAPSED);
			}
			/* NOTE(rnp): block until work completes so that we can record timings */
			glFinish();
			cs->processing_progress = 1;

			for (u32 i = 0; i < ARRAY_COUNT(frame->stats->timer_ids); i++) {
				u64 ns = 0;
				if (frame->stats->timer_active[i]) {
					glGetQueryObjectui64v(frame->stats->timer_ids[i],
					                      GL_QUERY_RESULT, &ns);
					frame->stats->timer_active[i] = 0;
				}
				frame->stats->times[i] = (f32)ns / 1e9;
			}

			if (did_sum_shader) {
				u32 aframe_index = (ctx->averaged_frame_index %
				                    ARRAY_COUNT(ctx->averaged_frames));
				ctx->averaged_frames[aframe_index].ready_to_present = 1;
				/* TODO(rnp): not really sure what to do here */
				mem_copy(ctx->averaged_frame_compute_stats[aframe_index].times,
				         frame->stats->times, sizeof(frame->stats->times));
				atomic_inc(&ctx->averaged_frame_index, 1);
			}
			frame->store->ready_to_present = 1;
			cs->processing_compute         = 0;

			end_renderdoc_capture(gl_context);
		} break;
		case BW_SAVE_FRAME: {
			BeamformFrame *frame = work->output_frame_ctx.frame.store;
			ASSERT(frame->ready_to_present);
			export_frame(ctx, work->output_frame_ctx.file_handle, frame);
		} break;
		}

		if (can_commit) {
			beamform_work_queue_pop_commit(q);
			work = beamform_work_queue_pop(q);
		}
	}
}

#include "ui.c"

DEBUG_EXPORT BEAMFORMER_FRAME_STEP_FN(beamformer_frame_step)
{
	dt_for_frame = GetFrameTime();

	if (IsWindowResized()) {
		ctx->window_size.h = GetScreenHeight();
		ctx->window_size.w = GetScreenWidth();
	}

	if (input->executable_reloaded) {
		ui_init(ctx, ctx->ui_backing_store);
		DEBUG_DECL(start_frame_capture = ctx->os.start_frame_capture);
		DEBUG_DECL(end_frame_capture   = ctx->os.end_frame_capture);
	}

	if (ctx->start_compute && !input->pipe_data_available) {
		if (ctx->beamform_frames[ctx->display_frame_index].ready_to_present) {
			BeamformWork *work = beamform_work_queue_push(ctx->beamform_work_queue);
			if (fill_frame_compute_work(ctx, work)) {
				beamform_work_queue_push_commit(ctx->beamform_work_queue);
				ctx->os.wake_thread(ctx->os.compute_worker.sync_handle);
				ctx->start_compute = 0;
			}
		}
	}

	BeamformerParameters *bp = &ctx->params->raw;
	if (ctx->ready_for_rf && input->pipe_data_available) {
		BeamformWork *work = beamform_work_queue_push(ctx->beamform_work_queue);
		if (work) {
			ctx->start_compute = 1;
			ctx->ready_for_rf  = 0;

			work->type        = BW_LOAD_RF_DATA;
			work->file_handle = input->pipe_handle;
			beamform_work_queue_push_commit(ctx->beamform_work_queue);

			BeamformWork *compute = beamform_work_queue_push(ctx->beamform_work_queue);
			if (fill_frame_compute_work(ctx, compute))
				beamform_work_queue_push_commit(ctx->beamform_work_queue);

			if (compute && ctx->params->export_next_frame) {
				BeamformWork *export = beamform_work_queue_push(ctx->beamform_work_queue);
				if (export) {
					/* TODO: we don't really want the beamformer opening/closing files */
					iptr f = ctx->os.open_for_write(ctx->params->export_pipe_name);
					export->type = BW_SAVE_FRAME;
					export->output_frame_ctx.file_handle = f;
					if (ctx->params->raw.output_points.w > 1) {
						u32 a_index = !(ctx->averaged_frame_index %
						                ARRAY_COUNT(ctx->averaged_frames));
						BeamformFrame      *aframe = ctx->averaged_frames + a_index;
						ComputeShaderStats *astats = ctx->averaged_frame_compute_stats + a_index;
						export->output_frame_ctx.frame.store = aframe;
						export->output_frame_ctx.frame.stats = astats;
					} else {
						export->output_frame_ctx.frame = compute->frame;
					}
					beamform_work_queue_push_commit(ctx->beamform_work_queue);
				}
				ctx->params->export_next_frame = 0;
			}

			if (ctx->params->upload) {
				/* TODO(rnp): clean this up */
				ctx->ui_read_params = 1;
			}
		}
	}

	BeamformFrameIterator bfi = beamform_frame_iterator(ctx, ctx->display_frame_index,
	                                                    ctx->next_render_frame_index - ctx->display_frame_index);
	for (BeamformFrame *frame = frame_next(&bfi); frame; frame = frame_next(&bfi)) {
		if (frame->in_flight && frame->ready_to_present) {
			frame->in_flight         = 0;
			ctx->display_frame_index = frame - bfi.frames;
		}
	}

	if (ctx->start_compute) {
		ctx->start_compute = 0;
		ctx->os.wake_thread(ctx->os.compute_worker.sync_handle);
	}

	BeamformFrame      *frame_to_draw;
	ComputeShaderStats *frame_compute_stats;
	if (bp->output_points.w > 1) {
		u32 a_index = !(ctx->averaged_frame_index % ARRAY_COUNT(ctx->averaged_frames));
		frame_to_draw       = ctx->averaged_frames + a_index;
		frame_compute_stats = ctx->averaged_frame_compute_stats + a_index;
	} else {
		frame_to_draw       = ctx->beamform_frames + ctx->display_frame_index;
		frame_compute_stats = ctx->beamform_frame_compute_stats + ctx->display_frame_index;
	}

	draw_ui(ctx, input, frame_to_draw, frame_compute_stats);

	ctx->fsctx.updated = 0;

	if (WindowShouldClose())
		ctx->should_exit = 1;
}
