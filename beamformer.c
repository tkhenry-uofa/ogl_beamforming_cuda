/* See LICENSE for license details. */
#include "beamformer.h"

static f32 dt_for_frame;

#include "ui.c"

static size
decoded_data_size(ComputeShaderCtx *cs)
{
	uv4  dim    = cs->dec_data_dim;
	size result = 2 * sizeof(f32) * dim.x * dim.y * dim.z;
	return result;
}

static void
alloc_output_image(BeamformerCtx *ctx)
{
	BeamformerParameters *bp = &ctx->params->raw;
	ComputeShaderCtx *cs     = &ctx->csctx;

	u32 max_3d_dim      = ctx->gl.max_3d_texture_dim;
	ctx->out_data_dim.x = CLAMP(round_down_power_of_2(ORONE(bp->output_points.x)), 1, max_3d_dim);
	ctx->out_data_dim.y = CLAMP(round_down_power_of_2(ORONE(bp->output_points.y)), 1, max_3d_dim);
	ctx->out_data_dim.z = CLAMP(round_down_power_of_2(ORONE(bp->output_points.z)), 1, max_3d_dim);
	ctx->out_data_dim.w = CLAMP(bp->output_points.w, 0, ARRAY_COUNT(cs->sum_textures));
	bp->output_points   = ctx->out_data_dim;

	/* NOTE: allocate storage for beamformed output data;
	 * this is shared between compute and fragment shaders */
	uv4 odim    = ctx->out_data_dim;
	u32 max_dim = MAX(odim.x, MAX(odim.y, odim.z));
	ctx->out_texture_mips = _tzcnt_u32(max_dim) + 1;

	glActiveTexture(GL_TEXTURE0);
	glDeleteTextures(1, &ctx->out_texture);
	glGenTextures(1, &ctx->out_texture);
	glBindTexture(GL_TEXTURE_3D, ctx->out_texture);
	glTexStorage3D(GL_TEXTURE_3D, ctx->out_texture_mips, GL_RG32F, odim.x, odim.y, odim.z);

	glDeleteTextures(ARRAY_COUNT(cs->sum_textures), cs->sum_textures);
	glGenTextures(odim.w, cs->sum_textures);
	for (u32 i = 0; i < odim.w; i++) {
		glBindTexture(GL_TEXTURE_3D, cs->sum_textures[i]);
		glTexStorage3D(GL_TEXTURE_3D, ctx->out_texture_mips, GL_RG32F, odim.x, odim.y, odim.z);
	}

	bp->array_count = CLAMP(bp->array_count, 1, ARRAY_COUNT(cs->array_textures));
	glDeleteTextures(ARRAY_COUNT(cs->array_textures), cs->array_textures);
	glGenTextures(bp->array_count, cs->array_textures);
	for (u32 i = 0; i < bp->array_count; i++) {
		glBindTexture(GL_TEXTURE_3D, cs->array_textures[i]);
		glTexStorage3D(GL_TEXTURE_3D, ctx->out_texture_mips, GL_RG32F, odim.x, odim.y, odim.z);
	}

	UnloadRenderTexture(ctx->fsctx.output);
	/* TODO: select odim.x vs odim.y */
	ctx->fsctx.output = LoadRenderTexture(odim.x, odim.z);
	GenTextureMipmaps(&ctx->fsctx.output.texture);
	//SetTextureFilter(ctx->fsctx.output.texture, TEXTURE_FILTER_ANISOTROPIC_8X);
	//SetTextureFilter(ctx->fsctx.output.texture, TEXTURE_FILTER_TRILINEAR);
	SetTextureFilter(ctx->fsctx.output.texture, TEXTURE_FILTER_BILINEAR);
}

static void
alloc_shader_storage(BeamformerCtx *ctx, Arena a)
{
	ComputeShaderCtx *cs     = &ctx->csctx;
	BeamformerParameters *bp = &ctx->params->raw;
	uv4 dec_data_dim         = bp->dec_data_dim;
	uv2 rf_raw_dim           = bp->rf_raw_dim;
	ctx->csctx.dec_data_dim  = dec_data_dim;
	ctx->csctx.rf_raw_dim    = rf_raw_dim;
	size rf_raw_size         = rf_raw_dim.x * rf_raw_dim.y * sizeof(i16);
	size rf_decoded_size     = decoded_data_size(cs);

	glDeleteBuffers(ARRAY_COUNT(cs->rf_data_ssbos), cs->rf_data_ssbos);
	glCreateBuffers(ARRAY_COUNT(cs->rf_data_ssbos), cs->rf_data_ssbos);

	i32 storage_flags = GL_DYNAMIC_STORAGE_BIT;
	switch (ctx->gl.vendor_id) {
	case GL_VENDOR_INTEL:
	case GL_VENDOR_AMD:
		if (cs->raw_data_ssbo)
			glUnmapNamedBuffer(cs->raw_data_ssbo);
		storage_flags |= GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT;
	case GL_VENDOR_NVIDIA:
		/* NOTE: register_cuda_buffers will handle the updated ssbo */
		break;
	}

	size full_rf_buf_size = ARRAY_COUNT(cs->raw_data_fences) * rf_raw_size;
	glDeleteBuffers(1, &cs->raw_data_ssbo);
	glCreateBuffers(1, &cs->raw_data_ssbo);
	glNamedBufferStorage(cs->raw_data_ssbo, full_rf_buf_size, 0, storage_flags);

	for (u32 i = 0; i < ARRAY_COUNT(cs->rf_data_ssbos); i++)
		glNamedBufferStorage(cs->rf_data_ssbos[i], rf_decoded_size, 0, 0);

	i32 map_flags = GL_MAP_WRITE_BIT|GL_MAP_PERSISTENT_BIT|GL_MAP_UNSYNCHRONIZED_BIT;
	switch (ctx->gl.vendor_id) {
	case GL_VENDOR_INTEL:
	case GL_VENDOR_AMD:
		cs->raw_data_arena.beg = glMapNamedBufferRange(cs->raw_data_ssbo, 0,
		                                               full_rf_buf_size, map_flags);
		break;
	case GL_VENDOR_NVIDIA:
		cs->raw_data_arena = os_alloc_arena(cs->raw_data_arena, full_rf_buf_size);
		ctx->cuda_lib.register_cuda_buffers(cs->rf_data_ssbos, ARRAY_COUNT(cs->rf_data_ssbos),
		                                    cs->raw_data_ssbo);
		ctx->cuda_lib.init_cuda_configuration(bp->rf_raw_dim.E, bp->dec_data_dim.E,
		                                      bp->channel_mapping, bp->channel_offset > 0);
		break;
	}

	/* NOTE: store hadamard in GPU once; it won't change for a particular imaging session */
	cs->hadamard_dim       = (uv2){.x = dec_data_dim.z, .y = dec_data_dim.z};
	size hadamard_elements = dec_data_dim.z * dec_data_dim.z;
	i32  *hadamard         = alloc(&a, i32, hadamard_elements);
	fill_hadamard(hadamard, dec_data_dim.z);
	glDeleteBuffers(1, &cs->hadamard_ssbo);
	glCreateBuffers(1, &cs->hadamard_ssbo);
	glNamedBufferStorage(cs->hadamard_ssbo, hadamard_elements * sizeof(i32), hadamard, 0);
}

static m3
observation_direction_to_xdc_space(v3 direction, v3 origin, v3 corner1, v3 corner2)
{
	v3 edge1      = sub_v3(corner1, origin);
	v3 edge2      = sub_v3(corner2, origin);
	v3 xdc_normal = cross(edge1, edge2);
	xdc_normal.z  = ABS(xdc_normal.z);

	v3 e1 = normalize_v3(sub_v3(xdc_normal, direction));
	v3 e2 = {.y = 1};
	v3 e3 = normalize_v3(cross(e2, e1));

	m3 result = {
		.c[0] = (v3){.x = e3.x, .y = e2.x, .z = e1.x},
		.c[1] = (v3){.x = e3.y, .y = e2.y, .z = e1.y},
		.c[2] = (v3){.x = e3.z, .y = e2.z, .z = e1.z},
	};
	return result;
}

static b32
do_volume_computation_step(BeamformerCtx *ctx, enum compute_shaders shader)
{
	ComputeShaderCtx *cs = &ctx->csctx;
	ExportCtx        *e  = &ctx->export_ctx;

	b32 done = 0;

	/* NOTE: we start this elsewhere on the first dispatch so that we can include
	 * times such as decoding/demodulation/etc. */
	if (!(e->state & ES_TIMER_ACTIVE)) {
		glQueryCounter(e->timer_ids[0], GL_TIMESTAMP);
		e->state |= ES_TIMER_ACTIVE;
	}

	glUseProgram(cs->programs[shader]);
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, cs->shared_ubo);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, e->rf_data_ssbo);

	glBindImageTexture(0, e->volume_texture, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R32F);
	glUniform1i(cs->volume_export_pass_id, 1);

	/* NOTE: We must tile this otherwise GL will kill us for taking too long */
	/* TODO: this could be based on multiple dimensions */
	u32 dispatch_count = e->volume_dim.z / 32;
	uv4 dim_offset = {.z = !!dispatch_count * 32 * e->dispatch_index++};
	glUniform3iv(cs->volume_export_dim_offset_id, 1, (i32 *)dim_offset.E);
	glDispatchCompute(ORONE(e->volume_dim.x / 32), e->volume_dim.y, 1);
	if (e->dispatch_index >= dispatch_count) {
		e->dispatch_index  = 0;
		e->state          &= ~ES_COMPUTING;
		done               = 1;
	}

	glQueryCounter(e->timer_ids[1], GL_TIMESTAMP);

	return done;
}

static void
do_sum_shader(ComputeShaderCtx *cs, u32 *in_textures, u32 in_texture_count, u32 out_texture, uv4 out_data_dim)
{
	/* NOTE: zero output before summing */
	glClearTexImage(out_texture, 0, GL_RED, GL_FLOAT, 0);

	glBindImageTexture(0, out_texture, 0, GL_TRUE, 0, GL_READ_WRITE, GL_RG32F);
	glUniform1f(cs->sum_prescale_id, 1 / (f32)in_texture_count);
	for (u32 i = 0; i < in_texture_count; i++) {
		glBindImageTexture(1, in_textures[i], 0, GL_TRUE, 0, GL_READ_ONLY, GL_RG32F);
		glDispatchCompute(ORONE(out_data_dim.x / 32),
		                  ORONE(out_data_dim.y),
		                  ORONE(out_data_dim.z / 32));
		glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
	}
}

static void
do_compute_shader(BeamformerCtx *ctx, enum compute_shaders shader)
{
	ComputeShaderCtx *csctx = &ctx->csctx;
	uv2  rf_raw_dim         = ctx->params->raw.rf_raw_dim;
	size rf_raw_size        = rf_raw_dim.x * rf_raw_dim.y * sizeof(i16);

	glBeginQuery(GL_TIME_ELAPSED, csctx->timer_ids[csctx->timer_index][shader]);

	glUseProgram(csctx->programs[shader]);
	glBindBufferBase(GL_UNIFORM_BUFFER, 0, csctx->shared_ubo);

	u32 output_ssbo_idx = !csctx->last_output_ssbo_index;
	u32 input_ssbo_idx  = csctx->last_output_ssbo_index;
	switch (shader) {
	case CS_HADAMARD:
		glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, csctx->raw_data_ssbo,
		                  csctx->raw_data_index * rf_raw_size, rf_raw_size);

		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, csctx->rf_data_ssbos[output_ssbo_idx]);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, csctx->hadamard_ssbo);
		glDispatchCompute(ORONE(csctx->dec_data_dim.x / 32),
		                  ORONE(csctx->dec_data_dim.y / 32),
		                  ORONE(csctx->dec_data_dim.z));
		csctx->raw_data_fences[csctx->raw_data_index] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		csctx->last_output_ssbo_index = !csctx->last_output_ssbo_index;
		break;
	case CS_CUDA_DECODE:
		ctx->cuda_lib.cuda_decode(csctx->raw_data_index * rf_raw_size, output_ssbo_idx);
		csctx->raw_data_fences[csctx->raw_data_index] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
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
		u32 texture = ctx->out_texture;
		for (u32 i = 1; i < ctx->out_texture_mips; i++) {
			glBindImageTexture(0, texture, i - 1, GL_TRUE, 0, GL_READ_ONLY,  GL_RG32F);
			glBindImageTexture(1, texture, i - 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RG32F);
			glUniform1i(csctx->mips_level_id, i);

			u32 width  = ctx->out_data_dim.x >> i;
			u32 height = ctx->out_data_dim.y >> i;
			u32 depth  = ctx->out_data_dim.z >> i;
			glDispatchCompute(ORONE(width / 32), ORONE(height), ORONE(depth / 32));
			glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
		}
	} break;
	case CS_HERCULES:
	case CS_UFORCES: {
		if (ctx->export_ctx.state & ES_START) {
			/* NOTE: on the first frame of compute make a copy of the rf data */
			size rf_size           = decoded_data_size(csctx);
			ctx->export_ctx.state &= ~ES_START;
			ctx->export_ctx.state |= ES_COMPUTING;
			glCopyNamedBufferSubData(csctx->rf_data_ssbos[input_ssbo_idx],
			                         ctx->export_ctx.rf_data_ssbo, 0, 0, rf_size);
		}

		BeamformerParameters *bp = &ctx->params->raw;

		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, csctx->rf_data_ssbos[input_ssbo_idx]);
		glUniform3iv(csctx->volume_export_dim_offset_id, 1, (i32 []){0, 0, 0});
		glUniform1i(csctx->volume_export_pass_id, 0);

		for (u32 i = 0; i < bp->array_count; i++) {
			u32 texture;
			if (bp->array_count == 1) {
				if (ctx->out_data_dim.w > 0) {
					texture = csctx->sum_textures[csctx->sum_texture_index];
				} else {
					texture = ctx->out_texture;
				}
			} else {
				texture = csctx->array_textures[i];
			}

			m3 xdc_transform = observation_direction_to_xdc_space((v3){.z = 1},
			                                                      bp->xdc_origin[i].xyz,
			                                                      bp->xdc_corner1[i].xyz,
			                                                      bp->xdc_corner2[i].xyz);
			glBindImageTexture(0, texture, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RG32F);
			glUniform1i(csctx->xdc_index_id, i);
			glUniformMatrix3fv(csctx->xdc_transform_id, 1, GL_FALSE, xdc_transform.E);
			glDispatchCompute(ORONE(ctx->out_data_dim.x / 32),
			                  ctx->out_data_dim.y,
			                  ORONE(ctx->out_data_dim.z / 32));
		}
		if (bp->array_count > 1) {
			glUseProgram(csctx->programs[CS_SUM]);
			glBindBufferBase(GL_UNIFORM_BUFFER, 0, csctx->shared_ubo);
			if (ctx->out_data_dim.w > 0) {
				do_sum_shader(csctx, csctx->array_textures, bp->array_count,
				              csctx->sum_textures[csctx->sum_texture_index],
				              ctx->out_data_dim);
			} else {
				do_sum_shader(csctx, csctx->array_textures, bp->array_count,
				              ctx->out_texture, ctx->out_data_dim);
			}
		}
	} break;
	case CS_SUM: {
		u32 frame_count = ctx->out_data_dim.w;
		if (frame_count) {
			do_sum_shader(csctx, csctx->sum_textures, frame_count, ctx->out_texture, ctx->out_data_dim);
			csctx->sum_texture_index = (csctx->sum_texture_index + 1) % frame_count;
		}
	} break;
	default: ASSERT(0);
	}

	glEndQuery(GL_TIME_ELAPSED);
}

static void
check_compute_timers(ComputeShaderCtx *cs, ExportCtx *e, BeamformerParametersFull *bp)
{
	/* NOTE: volume generation running timer */
	if (e->state & ES_TIMER_ACTIVE) {
		u64 start_ns = 0, end_ns = 0;
		glGetQueryObjectui64v(e->timer_ids[0], GL_QUERY_RESULT, &start_ns);
		glGetQueryObjectui64v(e->timer_ids[1], GL_QUERY_RESULT, &end_ns);
		u64 elapsed_ns = end_ns - start_ns;
		e->runtime    += (f32)elapsed_ns * 1e-9;
		e->state      &= ~ES_TIMER_ACTIVE;
	}

	/* NOTE: main timers for display portion of the program */
	u32 last_idx = (cs->timer_index - 1) % ARRAY_COUNT(cs->timer_fences);
	if (!cs->timer_fences[last_idx])
		return;

	i32 status = glClientWaitSync(cs->timer_fences[last_idx], 0, 0);
	if (status == GL_TIMEOUT_EXPIRED || status == GL_WAIT_FAILED)
		return;
	glDeleteSync(cs->timer_fences[last_idx]);
	cs->timer_fences[last_idx] = NULL;

	for (u32 i = 0; i < bp->compute_stages_count; i++) {
		u64 ns = 0;
		i32 idx = bp->compute_stages[i];
		glGetQueryObjectui64v(cs->timer_ids[last_idx][idx], GL_QUERY_RESULT, &ns);
		cs->last_frame_time[idx] = (f32)ns / 1e9;
	}
}

DEBUG_EXPORT void
do_beamformer(BeamformerCtx *ctx, Arena arena)
{
	dt_for_frame = GetFrameTime();

	if (IsWindowResized()) {
		ctx->window_size.h = GetScreenHeight();
		ctx->window_size.w = GetScreenWidth();
	}

	/* NOTE: Store the compute time for the last frame. */
	check_compute_timers(&ctx->csctx, &ctx->export_ctx, ctx->params);

	BeamformerParameters *bp = &ctx->params->raw;
	/* NOTE: Check for and Load RF Data into GPU */
	if (os_poll_pipe(ctx->data_pipe)) {
		ComputeShaderCtx *cs = &ctx->csctx;
		if (!uv4_equal(cs->dec_data_dim, bp->dec_data_dim))
			alloc_shader_storage(ctx, arena);

		if (!uv4_equal(ctx->out_data_dim, bp->output_points))
			alloc_output_image(ctx);

		cs->raw_data_index = (cs->raw_data_index + 1) % ARRAY_COUNT(cs->raw_data_fences);
		i32 raw_index = ctx->csctx.raw_data_index;
		/* NOTE: if this times out it means the command queue is more than 3 frames behind.
		 * In that case we need to re-evaluate the buffer size */
		if (ctx->csctx.raw_data_fences[raw_index]) {
			i32 result = glClientWaitSync(cs->raw_data_fences[raw_index], 0, 10000);
			if (result == GL_TIMEOUT_EXPIRED) {
				//ASSERT(0);
			}
			glDeleteSync(cs->raw_data_fences[raw_index]);
			cs->raw_data_fences[raw_index] = NULL;
		}

		uv2  rf_raw_dim   = cs->rf_raw_dim;
		size rf_raw_size  = rf_raw_dim.x * rf_raw_dim.y * sizeof(i16);

		void *rf_data_buf = cs->raw_data_arena.beg + raw_index * rf_raw_size;
		size rlen         = os_read_pipe_data(ctx->data_pipe, rf_data_buf, rf_raw_size);
		if (rlen != rf_raw_size) {
			ctx->partial_transfer_count++;
		} else {
			ctx->flags |= DO_COMPUTE;
			switch (ctx->gl.vendor_id) {
			case GL_VENDOR_INTEL:
				/* TODO: intel complains about this buffer being busy even with
				 * MAP_UNSYNCHRONIZED_BIT */
			case GL_VENDOR_AMD:
				break;
			case GL_VENDOR_NVIDIA:
				glNamedBufferSubData(cs->raw_data_ssbo, raw_index * rf_raw_size,
				                     rf_raw_size, rf_data_buf);
			}
		}
	}

	/* NOTE: we are starting a volume computation on this frame so make some space */
	if (ctx->export_ctx.state & ES_START) {
		ExportCtx *e = &ctx->export_ctx;
		e->runtime   = 0;
		uv4 edim     = e->volume_dim;

		/* NOTE: get a timestamp here which will include decoding/demodulating/etc. */
		glQueryCounter(e->timer_ids[0], GL_TIMESTAMP);
		e->state |= ES_TIMER_ACTIVE;

		glDeleteTextures(1, &e->volume_texture);
		glCreateTextures(GL_TEXTURE_3D, 1, &e->volume_texture);
		glTextureStorage3D(e->volume_texture, 1, GL_R32F, edim.x, edim.y, edim.z);

		glDeleteBuffers(1, &e->rf_data_ssbo);
		glCreateBuffers(1, &e->rf_data_ssbo);
		glNamedBufferStorage(e->rf_data_ssbo, decoded_data_size(&ctx->csctx), 0, 0);
	}

	if (ctx->flags & DO_COMPUTE || ctx->export_ctx.state & ES_START) {
		if (ctx->params->upload && !(ctx->export_ctx.state & ES_COMPUTING)) {
			glNamedBufferSubData(ctx->csctx.shared_ubo, 0, sizeof(*bp), bp);
			ctx->params->upload = 0;
		}

		u32 stages = ctx->params->compute_stages_count;
		for (u32 i = 0; i < stages; i++) {
			do_compute_shader(ctx, ctx->params->compute_stages[i]);
		}
		ctx->flags &= ~DO_COMPUTE;
		ctx->flags |= GEN_MIPMAPS;

		u32 tidx = ctx->csctx.timer_index;
		glDeleteSync(ctx->csctx.timer_fences[tidx]);
		ctx->csctx.timer_fences[tidx] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		ctx->csctx.timer_index = (tidx + 1) % ARRAY_COUNT(ctx->csctx.timer_fences);
	}

	if (ctx->export_ctx.state & ES_COMPUTING) {
		/* TODO: this could probably be adapted to do FORCES as well */
		b32 done = do_volume_computation_step(ctx, CS_HERCULES);
		if (done) {
			ExportCtx *e         = &ctx->export_ctx;
			uv4 dim              = e->volume_dim;
			size volume_out_size = dim.x * dim.y * dim.z * sizeof(f32);
			e->volume_buf        = os_alloc_arena(e->volume_buf, volume_out_size);
			glGetTextureImage(e->volume_texture, 0, GL_RED, GL_FLOAT, volume_out_size,
			                  e->volume_buf.beg);
			s8 raw = {.len = volume_out_size, .data = e->volume_buf.beg};
			if (!os_write_file("raw_volume.bin", raw))
				TraceLog(LOG_WARNING, "failed to write output volume\n");
		}
	}

	/* NOTE: draw output image texture using render fragment shader */
	BeginTextureMode(ctx->fsctx.output);
		ClearBackground(PINK);
		BeginShaderMode(ctx->fsctx.shader);
			FragmentShaderCtx *fs = &ctx->fsctx;
			glUseProgram(fs->shader.id);
			glBindTextureUnit(0, ctx->out_texture);
			glUniform1f(fs->db_cutoff_id, fs->db);
			DrawTexture(fs->output.texture, 0, 0, WHITE);
		EndShaderMode();
	EndTextureMode();

	/* NOTE: regenerate mipmaps only when the output has actually changed */
	if (ctx->flags & GEN_MIPMAPS) {
		/* NOTE: shut up raylib's reporting on mipmap gen */
		SetTraceLogLevel(LOG_NONE);
		GenTextureMipmaps(&ctx->fsctx.output.texture);
		SetTraceLogLevel(LOG_INFO);
		ctx->flags &= ~GEN_MIPMAPS;
	}

	draw_ui(ctx, arena);

	if (IsKeyPressed(KEY_R))
		ctx->flags |= RELOAD_SHADERS;
}
