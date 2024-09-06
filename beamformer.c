/* See LICENSE for license details. */
#include "beamformer.h"
#include "ui.c"

static void
alloc_output_image(BeamformerCtx *ctx)
{
	BeamformerParameters *bp = &ctx->params->raw;
	ctx->out_data_dim.x = round_down_power_of_2(ORONE(bp->output_points.x));
	ctx->out_data_dim.y = round_down_power_of_2(ORONE(bp->output_points.y));
	ctx->out_data_dim.z = round_down_power_of_2(ORONE(bp->output_points.z));
	bp->output_points   = ctx->out_data_dim;

	/* NOTE: allocate storage for beamformed output data;
	 * this is shared between compute and fragment shaders */
	uv4 odim    = ctx->out_data_dim;
	u32 max_dim = MAX(odim.x, MAX(odim.y, odim.z));
	/* TODO: does this actually matter or is 0 fine? */
	ctx->out_texture_unit = 0;
	ctx->out_texture_mips = _tzcnt_u32(max_dim) + 1;
	glActiveTexture(GL_TEXTURE0 + ctx->out_texture_unit);
	glDeleteTextures(1, &ctx->out_texture);
	glGenTextures(1, &ctx->out_texture);
	glBindTexture(GL_TEXTURE_3D, ctx->out_texture);
	glTexStorage3D(GL_TEXTURE_3D, ctx->out_texture_mips, GL_RG32F, odim.x, odim.y, odim.z);

	UnloadRenderTexture(ctx->fsctx.output);
	/* TODO: select odim.x vs odim.y */
	ctx->fsctx.output = LoadRenderTexture(odim.x, odim.z);
	GenTextureMipmaps(&ctx->fsctx.output.texture);
	//SetTextureFilter(ctx->fsctx.output.texture, TEXTURE_FILTER_ANISOTROPIC_8X);
	//SetTextureFilter(ctx->fsctx.output.texture, TEXTURE_FILTER_TRILINEAR);
	SetTextureFilter(ctx->fsctx.output.texture, TEXTURE_FILTER_BILINEAR);

	ctx->flags &= ~ALLOC_OUT_TEX;
}

static void
alloc_shader_storage(BeamformerCtx *ctx, Arena a)
{
	ComputeShaderCtx *cs     = &ctx->csctx;
	BeamformerParameters *bp = &ctx->params->raw;
	uv4 dec_data_dim         = bp->dec_data_dim;
	uv2 rf_raw_dim           = bp->rf_raw_dim;
	size rf_raw_size         = rf_raw_dim.x * rf_raw_dim.y * sizeof(i16);
	size rf_decoded_size     = dec_data_dim.x * dec_data_dim.y * dec_data_dim.z * sizeof(f32) * 2;
	ctx->csctx.rf_raw_dim    = rf_raw_dim;
	ctx->csctx.dec_data_dim  = dec_data_dim;

	glDeleteBuffers(ARRAY_COUNT(cs->rf_data_ssbos), cs->rf_data_ssbos);
	glCreateBuffers(ARRAY_COUNT(cs->rf_data_ssbos), cs->rf_data_ssbos);

	i32 storage_flags = GL_DYNAMIC_STORAGE_BIT;
	switch (ctx->gl_vendor_id) {
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
	switch (ctx->gl_vendor_id) {
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

	ctx->flags &= ~ALLOC_SSBOS;
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
	case CS_MIN_MAX:
		glBindImageTexture(ctx->out_texture_unit, ctx->out_texture, 0, GL_FALSE, 0,
		                   GL_WRITE_ONLY, GL_RG32F);
		glUniform1i(csctx->out_data_tex_id, ctx->out_texture_unit);
		for (u32 i = 1; i < ctx->out_texture_mips; i++) {
			u32 otu = ctx->out_texture_unit;
			glBindImageTexture(otu + 1, ctx->out_texture, i - 1,
			                   GL_FALSE, 0, GL_READ_ONLY, GL_RG32F);
			glBindImageTexture(otu + 2, ctx->out_texture, i,
			                   GL_FALSE, 0, GL_WRITE_ONLY, GL_RG32F);
			glUniform1i(csctx->out_data_tex_id, otu + 1);
			glUniform1i(csctx->mip_view_tex_id, otu + 2);
			glUniform1i(csctx->mips_level_id, i);

			u32 width  = ctx->out_data_dim.x >> i;
			u32 height = ctx->out_data_dim.y >> i;
			u32 depth  = ctx->out_data_dim.z >> i;
			glDispatchCompute(ORONE(width / 32), ORONE(height), ORONE(depth / 32));
			glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
		}
		break;
	case CS_HERCULES:
	case CS_UFORCES:
		glActiveTexture(GL_TEXTURE0 + ctx->out_texture_unit);
		glBindTexture(GL_TEXTURE_3D, ctx->out_texture);
		glBindImageTexture(ctx->out_texture_unit, ctx->out_texture, 0, GL_TRUE, 0,
		                   GL_WRITE_ONLY, GL_RG32F);
		glUniform1i(csctx->out_data_tex_id, ctx->out_texture_unit);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, csctx->rf_data_ssbos[input_ssbo_idx]);
		glDispatchCompute(ORONE(ctx->out_data_dim.x / 32),
		                  ctx->out_data_dim.y,
		                  ORONE(ctx->out_data_dim.z / 32));
		break;
	default: ASSERT(0);
	}

	glEndQuery(GL_TIME_ELAPSED);
}

static void
check_compute_timers(ComputeShaderCtx *cs, BeamformerParametersFull *bp)
{
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
	ctx->dt = GetFrameTime();

	if (IsWindowResized()) {
		ctx->window_size.h = GetScreenHeight();
		ctx->window_size.w = GetScreenWidth();
	}

	/* NOTE: Store the compute time for the last frame. */
	check_compute_timers(&ctx->csctx, ctx->params);

	BeamformerParameters *bp = &ctx->params->raw;
	/* NOTE: Check for and Load RF Data into GPU */
	if (os_poll_pipe(ctx->data_pipe)) {
		ComputeShaderCtx *cs = &ctx->csctx;
		if (!uv4_equal(cs->dec_data_dim, bp->dec_data_dim) || ctx->flags & ALLOC_SSBOS)
			alloc_shader_storage(ctx, arena);

		if (!uv4_equal(ctx->out_data_dim, bp->output_points) || ctx->flags & ALLOC_OUT_TEX)
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
		switch (ctx->gl_vendor_id) {
		case GL_VENDOR_INTEL:
			/* TODO: intel complains about this buffer being busy even with
			 * MAP_UNSYNCHRONIZED_BIT */
		case GL_VENDOR_AMD:
			break;
		case GL_VENDOR_NVIDIA:
			glNamedBufferSubData(cs->raw_data_ssbo, raw_index * rf_raw_size,
			                     rf_raw_size, rf_data_buf);
		}
		if (rlen == rf_raw_size) ctx->flags |= DO_COMPUTE;
		else                     ctx->partial_transfer_count++;
	}

	if (ctx->flags & DO_COMPUTE) {
		if (ctx->params->upload) {
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

	/* NOTE: draw output image texture using render fragment shader */
	BeginTextureMode(ctx->fsctx.output);
		ClearBackground(PINK);
		BeginShaderMode(ctx->fsctx.shader);
			FragmentShaderCtx *fs = &ctx->fsctx;
			glUseProgram(fs->shader.id);
			glUniform1i(fs->out_data_tex_id, ctx->out_texture_unit);
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
