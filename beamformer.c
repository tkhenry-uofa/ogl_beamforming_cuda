/* See LICENSE for license details. */

#define GRAPHICS_API_OPENGL_43
#include <raylib.h>
#include <rlgl.h>

#include "util.h"

static void
do_compute_shader(BeamformerCtx *ctx, u32 rf_ssbo_idx, enum compute_shaders shader)
{
	ComputeShaderCtx *csctx = &ctx->csctx;
	glUseProgram(csctx->programs[shader]);

	glUniform3uiv(csctx->rf_data_dim_id,  1, csctx->rf_data_dim.E);
	glBindImageTexture(ctx->out_texture_unit, ctx->out_texture, 0, GL_FALSE, 0,
	                   GL_WRITE_ONLY, GL_RG32F);
	glUniform1i(csctx->out_data_tex_id, ctx->out_texture_unit);

	u32 decoded_ssbo_idx = 2;
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
draw_debug_overlay(BeamformerCtx *ctx, Arena arena)
{
	DrawFPS(20, 20);

	u32 fontsize  = 32;
	u32 fontspace = 1;
	s8 db_txt      = s8alloc(&arena, 64);
	s8 compute_txt = s8alloc(&arena, 64);
	snprintf((char *)db_txt.data, db_txt.len, "Dynamic Range: %0.01f [db]", ctx->fsctx.db);
	snprintf((char *)compute_txt.data, compute_txt.len, "Compute: %d", !!(ctx->flags & DO_COMPUTE));

	v2 db_fs      = {.rl = MeasureTextEx(ctx->font, (char *)db_txt.data, fontsize, fontspace)};
	v2 compute_fs = {.rl = MeasureTextEx(ctx->font, (char *)compute_txt.data, fontsize, fontspace)};

	v2 scale = {.x = 90, .y = 20 };
	/* NOTE: Dynamic Range */
	{
		v2 dpos  = {.x = 20, .y = ctx->window_size.y - db_fs.y - compute_fs.y - 20};
		v2 dposa = {.x = dpos.x + db_fs.x / scale.x, .y = dpos.y + db_fs.y / scale.y };
		DrawTextEx(ctx->font, (char *)db_txt.data, dposa.rl, fontsize, fontspace, Fade(BLACK, 0.8));
		DrawTextEx(ctx->font, (char *)db_txt.data, dpos.rl,  fontsize, fontspace, RED);
	}

	/* NOTE: Compute Status */
	{
		v2 dpos  = {.x = 20, .y = ctx->window_size.y - compute_fs.y - 20};
		v2 dposa = {.x = dpos.x + compute_fs.x / scale.x, .y = dpos.y + compute_fs.y / scale.y };
		DrawTextEx(ctx->font, (char *)compute_txt.data, dposa.rl, fontsize, fontspace, Fade(BLACK, 0.8));
		DrawTextEx(ctx->font, (char *)compute_txt.data, dpos.rl,  fontsize, fontspace, RED);
	}
}


DEBUG_EXPORT void
do_beamformer(BeamformerCtx *ctx, Arena arena, s8 rf_data)
{
	uv2 ws = ctx->window_size;
	f32 dt = GetFrameTime();

	static v2 pos       = {.x = 32,  .y = 128};
	static v2 scale     = {.x = 1.0, .y = 1.0};
	static u32 txt_idx  = 0;
	static char *txt[2] = { "-_-", "^_^" };

	u32 fontsize  = 32;
	u32 fontspace = 1;

	static v2 fs[2];
	if (fs[0].x == 0) {
		fs[0] = (v2){ .rl = MeasureTextEx(ctx->font, txt[0], fontsize, fontspace) };
		fs[1] = (v2){ .rl = MeasureTextEx(ctx->font, txt[1], fontsize, fontspace) };
	}

	pos.x += 130 * dt * scale.x;
	pos.y += 120 * dt * scale.y;

	if (pos.x > (ws.w - fs[txt_idx].x) || pos.x < 0) {
		txt_idx = !txt_idx;
		CLAMP(pos.x, 0, ws.w - fs[txt_idx].x);
		scale.x *= -1.0;
	}

	if (pos.y > (ws.h - fs[txt_idx].y) || pos.y < 0) {
		txt_idx = !txt_idx;
		CLAMP(pos.y, 0, ws.h - fs[txt_idx].y);
		scale.y *= -1.0;
	}

	/* NOTE: grab operating idx and swap it; other buffer can now be used for storage */
	u32 rf_ssbo_idx = atomic_fetch_xor_explicit(&ctx->csctx.rf_data_idx, 1, memory_order_relaxed);
	ASSERT(rf_ssbo_idx == 0 || rf_ssbo_idx == 1);

	/* NOTE: Load RF Data into GPU */
	/* TODO: This should be done in a separate thread */
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx->csctx.rf_data_ssbos[!rf_ssbo_idx]);
	glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, rf_data.len, rf_data.data);

	if (ctx->flags & DO_COMPUTE) {
		do_compute_shader(ctx, rf_ssbo_idx, CS_HADAMARD);
		do_compute_shader(ctx, rf_ssbo_idx, CS_UFORCES);
		do_compute_shader(ctx, rf_ssbo_idx, CS_MIN_MAX);
	}

	/* NOTE: check mouse wheel for adjusting dynamic range of image */
	ctx->fsctx.db += GetMouseWheelMove();
	CLAMP(ctx->fsctx.db, -120, 0);

	/* NOTE: draw output image texture using render fragment shader */
	BeginTextureMode(ctx->fsctx.output);
		ClearBackground(ctx->bg);
		BeginShaderMode(ctx->fsctx.shader);
			glUseProgram(ctx->fsctx.shader.id);
			u32 otu = ctx->out_texture_unit;
			glBindImageTexture(otu + 1, ctx->out_texture, ctx->out_texture_mips - 1,
			                   GL_FALSE, 0, GL_READ_ONLY, GL_RG32F);
			glBindImageTexture(otu, ctx->out_texture, 0,
			                   GL_FALSE, 0, GL_READ_ONLY, GL_RG32F);
			glUniform1i(ctx->fsctx.out_data_tex_id, otu);
			glUniform1i(ctx->fsctx.mip_view_tex_id, otu + 1);
			glUniform1f(ctx->fsctx.db_cutoff_id, ctx->fsctx.db);
			DrawTexture(ctx->fsctx.output.texture, 0, 0, WHITE);
		EndShaderMode();
	EndTextureMode();

	BeginDrawing();
		ClearBackground(ctx->bg);

		Texture *rtext = &ctx->fsctx.output.texture;
		Rectangle rect = { 0.0f, 0.0f, (f32)rtext->width, -(f32)rtext->height };
		DrawTextureRec(*rtext, rect, (Vector2){0}, WHITE);

		DrawTextEx(ctx->font, txt[txt_idx], pos.rl, fontsize, fontspace, RED);
		draw_debug_overlay(ctx, arena);
	EndDrawing();

	if (IsKeyPressed(KEY_R))
		ctx->flags |= RELOAD_SHADERS;
	if (IsKeyPressed(KEY_SPACE))
		ctx->flags ^= DO_COMPUTE;
}
