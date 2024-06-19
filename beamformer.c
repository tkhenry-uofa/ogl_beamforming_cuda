/* See LICENSE for license details. */

#include <raylib.h>
#include <rlgl.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES 1
#include <GL/glcorearb.h>
#include <GL/glext.h>
#endif

#include "util.h"

static void
do_compute_shader(BeamformerCtx *ctx, u32 rf_ssbo_idx, enum compute_shaders shader)
{
	ComputeShaderCtx *csctx = &ctx->csctx;
	glUseProgram(csctx->programs[shader]);

	glUniform3uiv(csctx->rf_data_dim_id,  1, csctx->rf_data_dim.E);
	glUniform3uiv(csctx->out_data_dim_id, 1, ctx->out_data_dim.E);

	/* NOTE: Temporary flag for testing */
	u32 data_idx = ctx->flags & DO_DECODE? 2 : rf_ssbo_idx;

	switch (shader) {
	case CS_HADAMARD:
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, csctx->rf_data_ssbos[rf_ssbo_idx]);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, csctx->rf_data_ssbos[2]);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, csctx->hadamard_ssbo);
		glDispatchCompute(csctx->rf_data_dim.x, csctx->rf_data_dim.y, csctx->rf_data_dim.z);
		break;
	case CS_UFORCES:
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, csctx->rf_data_ssbos[data_idx]);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, ctx->out_data_ssbo);
		glDispatchCompute(ctx->out_data_dim.x, ctx->out_data_dim.y, ctx->out_data_dim.z);
		break;
	default: ASSERT(0);
	}
}

static void
draw_debug_overlay(BeamformerCtx *ctx, Arena arena, Font font)
{
	DrawFPS(20, 20);

	u32 fontsize  = 32;
	u32 fontspace = 4;
	s8 decode_txt  = s8alloc(&arena, 64);
	s8 compute_txt = s8alloc(&arena, 64);
	snprintf((char *)decode_txt.data,  decode_txt.len,  "Decoding: %d", !!(ctx->flags & DO_DECODE));
	snprintf((char *)compute_txt.data, compute_txt.len, "Compute:  %d", !!(ctx->flags & DO_COMPUTE));
	v2 decode_fs  = {.rl = MeasureTextEx(font, (char *)decode_txt.data,  fontsize, fontspace)};
	v2 compute_fs = {.rl = MeasureTextEx(font, (char *)compute_txt.data, fontsize, fontspace)};
	v2 dpos = {.x = 20, .y = ctx->window_size.y - decode_fs.y - compute_fs.y - fontspace};
	DrawTextEx(font, (char *)decode_txt.data, dpos.rl,  fontsize, fontspace, RED);
	dpos.y += 2 + decode_fs.y;
	DrawTextEx(font, (char *)compute_txt.data, dpos.rl, fontsize, fontspace, RED);
}


DEBUG_EXPORT void
do_beamformer(BeamformerCtx *ctx, Arena arena, s8 rf_data)
{
	uv2 ws = ctx->window_size;
	f32 dt = GetFrameTime();

	static v2 pos   = {.x = 32, .y = 128 };
	static v2 scale = {.x = 1.0, .y = 1.0};
	static u32 txt_idx = 0;
	static char *txt[2] = { "-_-", "^_^" };

	Font font = GetFontDefault();
	v2 fs     = { .rl = MeasureTextEx(font, txt[txt_idx], 60, 6) };

	pos.x += 130 * dt * scale.x;
	pos.y += 120 * dt * scale.y;

	if (pos.x > (ws.w - fs.x) || pos.x < 0) {
		txt_idx = !txt_idx;
		fs = (v2){ .rl = MeasureTextEx(font, txt[txt_idx], 60, 6) };
		CLAMP(pos.x, 0, ws.w - fs.x);
		scale.x *= -1.0;
	}

	if (pos.y > (ws.h - fs.y) || pos.y < 0) {
		txt_idx = !txt_idx;
		fs = (v2){ .rl = MeasureTextEx(font, txt[txt_idx], 60, 6) };
		CLAMP(pos.y, 0, ws.h - fs.y);
		scale.y *= -1.0;
	}

	/* NOTE: grab operating idx and swap it; other buffer can now be used for storage */
	u32 rf_ssbo_idx = atomic_fetch_xor_explicit(&ctx->csctx.rf_data_idx, 1, memory_order_relaxed);
	ASSERT(rf_ssbo_idx == 0 || rf_ssbo_idx == 1);

	/* NOTE: Load RF Data into GPU */
	/* TODO: This should be done in a seperate thread */
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx->csctx.rf_data_ssbos[!rf_ssbo_idx]);
	glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, rf_data.len, rf_data.data);

	if (ctx->flags & DO_COMPUTE) {
		if (ctx->flags & DO_DECODE)
			do_compute_shader(ctx, rf_ssbo_idx, CS_HADAMARD);
		do_compute_shader(ctx, rf_ssbo_idx, CS_UFORCES);
	}

	BeginDrawing();

	ClearBackground(ctx->bg);

	BeginShaderMode(ctx->fsctx.shader);
		glUseProgram(ctx->fsctx.shader.id);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ctx->out_data_ssbo);
		glUniform3uiv(ctx->fsctx.out_data_dim_id, 1, ctx->out_data_dim.E);
		DrawTexture(ctx->fsctx.output, 0, 0, WHITE);
	EndShaderMode();

	DrawTextEx(font, txt[txt_idx], pos.rl, 60, 6, BLACK);
	draw_debug_overlay(ctx, arena, font);

	EndDrawing();

	if (IsKeyPressed(KEY_R))
		ctx->flags |= RELOAD_SHADERS;
	if (IsKeyPressed(KEY_SPACE))
		ctx->flags ^= DO_COMPUTE;
	if (IsKeyPressed(KEY_D))
		ctx->flags ^= DO_DECODE;
}
