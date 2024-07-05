/* See LICENSE for license details. */
#include "beamformer.h"

static void
do_compute_shader(BeamformerCtx *ctx, enum compute_shaders shader)
{
	ComputeShaderCtx *csctx = &ctx->csctx;
	glUseProgram(csctx->programs[shader]);

	glUniform3uiv(csctx->rf_data_dim_id,  1, csctx->rf_data_dim.E);
	glBindImageTexture(ctx->out_texture_unit, ctx->out_texture, 0, GL_FALSE, 0,
	                   GL_WRITE_ONLY, GL_RG32F);
	glUniform1i(csctx->out_data_tex_id, ctx->out_texture_unit);

	u32 rf_ssbo_idx      = 0;
	u32 decoded_ssbo_idx = 1;
	switch (shader) {
	case CS_HADAMARD:
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, csctx->rf_data_ssbos[rf_ssbo_idx]);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, csctx->rf_data_ssbos[decoded_ssbo_idx]);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, csctx->hadamard_ssbo);
		glDispatchCompute(csctx->rf_data_dim.x / 32, csctx->rf_data_dim.y / 32, csctx->rf_data_dim.z);
		break;
	case CS_MIN_MAX:
		for (u32 i = 1; i < ctx->out_texture_mips; i++) {
			u32 otu = ctx->out_texture_unit;
			glBindImageTexture(otu + 1, ctx->out_texture, i - 1,
			                   GL_FALSE, 0, GL_READ_ONLY, GL_RG32F);
			glBindImageTexture(otu + 2, ctx->out_texture, i,
			                   GL_FALSE, 0, GL_WRITE_ONLY, GL_RG32F);
			glUniform1i(csctx->out_data_tex_id, otu + 1);
			glUniform1i(csctx->mip_view_tex_id, otu + 2);
			glUniform1i(csctx->mips_level_id, i);

			#define ORONE(x) ((x)? (x) : 1)
			u32 width  = ctx->out_data_dim.w >> i;
			u32 height = ctx->out_data_dim.h >> i;
			u32 depth  = ctx->out_data_dim.d >> i;
			glDispatchCompute(ORONE(width / 32), ORONE(height / 32), ORONE(depth));
			glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
		}
		break;
	case CS_UFORCES:
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, csctx->rf_data_ssbos[decoded_ssbo_idx]);
		glDispatchCompute(ctx->out_data_dim.x / 32, ctx->out_data_dim.y / 32, ctx->out_data_dim.z);
		break;
	default: ASSERT(0);
	}
}

static void
draw_debug_overlay(BeamformerCtx *ctx, Arena arena, f32 dt)
{
	DrawFPS(20, 20);

	uv2 ws = ctx->window_size;
	u32 fontsize  = 32;
	u32 fontspace = 1;

	s8 partial_txt = s8alloc(&arena, 64);
	s8 db_txt      = s8alloc(&arena, 64);
	snprintf((char *)partial_txt.data, partial_txt.len, "Partial Transfers: %u", ctx->partial_transfer_count);
	snprintf((char *)db_txt.data, db_txt.len, "Dynamic Range: %0.01f [db]", ctx->fsctx.db);

	v2 partial_fs = {.rl = MeasureTextEx(ctx->font, (char *)partial_txt.data, fontsize, fontspace)};
	v2 db_fs      = {.rl = MeasureTextEx(ctx->font, (char *)db_txt.data, fontsize, fontspace)};

	v2 scale = {.x = 90, .y = 20};
	v2 pos   = {.x = 20, .y = ws.h - db_fs.y - partial_fs.y - 20};
	/* NOTE: Dynamic Range */
	{
		v2 dposa = {.x = pos.x + db_fs.x / scale.x, .y = pos.y + db_fs.y / scale.y };
		DrawTextEx(ctx->font, (char *)db_txt.data, dposa.rl, fontsize, fontspace, Fade(BLACK, 0.8));
		DrawTextEx(ctx->font, (char *)db_txt.data, pos.rl,  fontsize, fontspace, RED);
		pos.y += db_fs.y;
	}
	/* NOTE: Partial Tranfers */
	{
		v2 dposa = {.x = pos.x + partial_fs.x / scale.x, .y = pos.y + partial_fs.y / scale.y };
		DrawTextEx(ctx->font, (char *)partial_txt.data, dposa.rl, fontsize, fontspace, Fade(BLACK, 0.8));
		DrawTextEx(ctx->font, (char *)partial_txt.data, pos.rl,  fontsize, fontspace, RED);
		pos.y += partial_fs.y;
	}

	{
		static v2 pos       = {.x = 32,  .y = 128};
		static v2 scale     = {.x = 1.0, .y = 1.0};
		static u32 txt_idx  = 0;
		static char *txt[2] = { "-_-", "^_^" };
		static v2 ts[2];
		if (ts[0].x == 0) {
			ts[0] = (v2){ .rl = MeasureTextEx(ctx->font, txt[0], fontsize, fontspace) };
			ts[1] = (v2){ .rl = MeasureTextEx(ctx->font, txt[1], fontsize, fontspace) };
		}

		pos.x += 130 * dt * scale.x;
		pos.y += 120 * dt * scale.y;

		if (pos.x > (ws.w - ts[txt_idx].x) || pos.x < 0) {
			txt_idx = !txt_idx;
			CLAMP(pos.x, 0, ws.w - ts[txt_idx].x);
			scale.x *= -1.0;
		}

		if (pos.y > (ws.h - ts[txt_idx].y) || pos.y < 0) {
			txt_idx = !txt_idx;
			CLAMP(pos.y, 0, ws.h - ts[txt_idx].y);
			scale.y *= -1.0;
		}

		DrawTextEx(ctx->font, txt[txt_idx], pos.rl, fontsize, fontspace, RED);
	}
}


DEBUG_EXPORT void
do_beamformer(BeamformerCtx *ctx, Arena arena, s8 rf_data)
{
	f32 dt = GetFrameTime();

	/* NOTE: Check for and Load RF Data into GPU */
	if (os_poll_pipe(ctx->data_pipe)) {
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx->csctx.rf_data_ssbos[0]);
		void *rf_data_buf = glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_WRITE_ONLY);
		ASSERT(rf_data_buf);
		uv3  rf_data_dim  = ctx->csctx.rf_data_dim;
		size rf_raw_size  = rf_data_dim.w * rf_data_dim.h * rf_data_dim.d * sizeof(i32);
		size rlen         = os_read_pipe_data(ctx->data_pipe, rf_data_buf, rf_raw_size);
		glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
		if (rlen == rf_raw_size) {
			/* NOTE: this will skip partially read data */
			do_compute_shader(ctx, CS_HADAMARD);
			do_compute_shader(ctx, CS_UFORCES);
			do_compute_shader(ctx, CS_MIN_MAX);
		} else {
			ctx->partial_transfer_count++;
		}
	}

	/* NOTE: check mouse wheel for adjusting dynamic range of image */
	ctx->fsctx.db += GetMouseWheelMove();
	CLAMP(ctx->fsctx.db, -120, 0);

	/* NOTE: draw output image texture using render fragment shader */
	BeginTextureMode(ctx->fsctx.output);
		ClearBackground(ctx->bg);
		BeginShaderMode(ctx->fsctx.shader);
			FragmentShaderCtx *fs = &ctx->fsctx;
			glUseProgram(fs->shader.id);
			glUniform1i(fs->out_data_tex_id, ctx->out_texture_unit);
			glUniform1f(fs->db_cutoff_id, fs->db);
			DrawTexture(fs->output.texture, 0, 0, WHITE);
		EndShaderMode();
	EndTextureMode();

	BeginDrawing();
		ClearBackground(ctx->bg);

		Texture *output   = &ctx->fsctx.output.texture;
		Rectangle win_r   = { 0.0f, 0.0f, (f32)ctx->window_size.w, (f32)ctx->window_size.h };
		Rectangle tex_r   = { 0.0f, 0.0f, (f32)output->width, -(f32)output->height };
		NPatchInfo tex_np = { tex_r, 0, 0, 0, 0, NPATCH_NINE_PATCH };
		DrawTextureNPatch(*output, tex_np, win_r, (Vector2){0}, 0, WHITE);

		draw_debug_overlay(ctx, arena, dt);
	EndDrawing();

	if (IsKeyPressed(KEY_R))
		ctx->flags |= RELOAD_SHADERS;
}
