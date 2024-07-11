/* See LICENSE for license details. */
#include "beamformer.h"

static void
alloc_shader_storage(BeamformerCtx *ctx, Arena a)
{
	uv4 rf_data_dim        = ctx->params->rf_data_dim;
	ctx->csctx.rf_data_dim = rf_data_dim;
	size rf_raw_size       = ctx->params->channel_data_stride * rf_data_dim.y * rf_data_dim.z * sizeof(i16);
	size rf_decoded_size   = rf_data_dim.x * rf_data_dim.y * rf_data_dim.z * sizeof(f32);

	glDeleteBuffers(ARRAY_COUNT(ctx->csctx.rf_data_ssbos), ctx->csctx.rf_data_ssbos);
	glGenBuffers(ARRAY_COUNT(ctx->csctx.rf_data_ssbos), ctx->csctx.rf_data_ssbos);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx->csctx.rf_data_ssbos[0]);
	glBufferStorage(GL_SHADER_STORAGE_BUFFER, rf_raw_size, 0,
	                GL_DYNAMIC_STORAGE_BIT|GL_MAP_WRITE_BIT);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx->csctx.rf_data_ssbos[1]);
	glBufferStorage(GL_SHADER_STORAGE_BUFFER, rf_decoded_size, 0, GL_DYNAMIC_STORAGE_BIT);

	/* NOTE: store hadamard in GPU once; it won't change for a particular imaging session */
	ctx->csctx.hadamard_dim = (uv2){ .x = rf_data_dim.z, .y = rf_data_dim.z };
	size hadamard_elements  = ctx->csctx.hadamard_dim.x * ctx->csctx.hadamard_dim.y;
	i32  *hadamard          = alloc(&a, i32, hadamard_elements);
	fill_hadamard(hadamard, ctx->csctx.hadamard_dim.x);

	rlUnloadShaderBuffer(ctx->csctx.hadamard_ssbo);
	ctx->csctx.hadamard_ssbo = rlLoadShaderBuffer(hadamard_elements * sizeof(i32), hadamard,
	                                              GL_STATIC_DRAW);

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
	ctx->fsctx.output = LoadRenderTexture(odim.x, odim.y);
}

static void
do_compute_shader(BeamformerCtx *ctx, enum compute_shaders shader)
{
	ComputeShaderCtx *csctx = &ctx->csctx;
	glUseProgram(csctx->programs[shader]);

	glBindImageTexture(ctx->out_texture_unit, ctx->out_texture, 0, GL_FALSE, 0,
	                   GL_WRITE_ONLY, GL_RG32F);
	glUniform1i(csctx->out_data_tex_id, ctx->out_texture_unit);

	glBindBufferBase(GL_UNIFORM_BUFFER, 0, csctx->shared_ubo);

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
			u32 width  = ctx->out_data_dim.x >> i;
			u32 height = ctx->out_data_dim.y >> i;
			u32 depth  = ctx->out_data_dim.z >> i;
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
draw_settings_ui(BeamformerCtx *ctx, Arena arena, f32 dt, v2 upper_left, v2 bottom_right)
{
	struct listing {
		char *prefix;
		char *suffix;
		f32  *data;
		f32  data_scale;
		b32  editable;
	} listings[] = {
		{ "Sampling Rate:", " [MHz]", &ctx->params->sampling_frequency, 1e-6, 0 },
		{ "Speed of Sound", " [m/s]", &ctx->params->speed_of_sound,     1,    1 },
		{ "Min X Point:",   " [mm]",  &ctx->params->output_min_xz.x,    1e3,  1 },
		{ "Max X Point:",   " [mm]",  &ctx->params->output_max_xz.x,    1e3,  1 },
		{ "Min Z Point:",   " [mm]",  &ctx->params->output_min_xz.y,    1e3,  1 },
		{ "Max Z Point:",   " [mm]",  &ctx->params->output_max_xz.y,    1e3,  1 },
		{ "Dynamic Range:", " [dB]",  &ctx->fsctx.db,                   1,    0 },
	};

	f32 line_pad  = 10;
	f32 right_pad = 10;

	v2 pos  = upper_left;
	pos.y  += 30;
	pos.x  += 10;

	s8 txt  = s8alloc(&arena, 64);

	for (u32 i = 0; i < ARRAY_COUNT(listings); i++) {
		struct listing *l = listings + i;
		DrawTextEx(ctx->font, l->prefix, pos.rl, ctx->font_size, ctx->font_spacing, ctx->fg);

		snprintf((char *)txt.data, txt.len, "%0.02f", *l->data * l->data_scale);
		v2 suffix_s = {.rl = MeasureTextEx(ctx->font, l->suffix, ctx->font_size,
		                                   ctx->font_spacing)};
		v2 txt_s    = {.rl = MeasureTextEx(ctx->font, (char *)txt.data, ctx->font_size,
		                                   ctx->font_spacing)};

		v2 rpos  = {.x = bottom_right.x - right_pad - txt_s.x - suffix_s.x, .y = pos.y};
		DrawTextEx(ctx->font, (char *)txt.data, rpos.rl, ctx->font_size,
		           ctx->font_spacing, ctx->fg);

		rpos.x += txt_s.x;
		DrawTextEx(ctx->font, l->suffix, rpos.rl, ctx->font_size, ctx->font_spacing, ctx->fg);
		pos.y += txt_s.y + line_pad;
	}
}

static void
draw_debug_overlay(BeamformerCtx *ctx, Arena arena, f32 dt)
{
	DrawFPS(20, 20);

	uv2 ws = ctx->window_size;
	u32 fontsize  = ctx->font_size;
	u32 fontspace = ctx->font_spacing;

	s8 partial_txt = s8alloc(&arena, 64);
	snprintf((char *)partial_txt.data, partial_txt.len, "Partial Transfers: %u", ctx->partial_transfer_count);

	v2 partial_fs = {.rl = MeasureTextEx(ctx->font, (char *)partial_txt.data, fontsize, fontspace)};

	v2 pos   = {.x = 20, .y = ws.h - partial_fs.y - 20};
	/* NOTE: Partial Tranfers */
	{
		DrawTextEx(ctx->font, (char *)partial_txt.data, pos.rl,  fontsize, fontspace, ctx->fg);
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
do_beamformer(BeamformerCtx *ctx, Arena arena)
{
	f32 dt = GetFrameTime();

	if (IsWindowResized()) {
		f32 aspect_ratio = 3.0f/4.0f;
		ctx->window_size.h = GetScreenHeight();
		ctx->window_size.w = ctx->window_size.h * aspect_ratio;
		SetWindowSize(ctx->window_size.w, ctx->window_size.h);
	}

	/* NOTE: Check for and Load RF Data into GPU */
	if (os_poll_pipe(ctx->data_pipe)) {
		if (!uv4_equal(ctx->csctx.rf_data_dim, ctx->params->rf_data_dim))
			alloc_shader_storage(ctx, arena);

		glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx->csctx.rf_data_ssbos[0]);
		void *rf_data_buf = glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_WRITE_ONLY);
		ASSERT(rf_data_buf);
		uv4  rf_data_dim  = ctx->csctx.rf_data_dim;
		size rf_raw_size  = rf_data_dim.x * rf_data_dim.y * rf_data_dim.z * sizeof(i16);
		size rlen         = os_read_pipe_data(ctx->data_pipe, rf_data_buf, rf_raw_size);
		glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
		if (rlen == rf_raw_size) {
			ctx->flags |= DO_COMPUTE;
		} else {
			ctx->partial_transfer_count++;
		}
	}

	if (ctx->flags & DO_COMPUTE) {
		if (ctx->flags & UPLOAD_UBO) {
			glBindBuffer(GL_UNIFORM_BUFFER, ctx->csctx.shared_ubo);
			void *ubo = glMapBuffer(GL_UNIFORM_BUFFER, GL_WRITE_ONLY);
			mem_copy((s8){.data = (u8 *)ctx->params, .len = sizeof(BeamformerParameters)},
			         (s8){.data = (u8 *)ubo,         .len = sizeof(BeamformerParameters)});
			glUnmapBuffer(GL_UNIFORM_BUFFER);
			ctx->flags &= ~UPLOAD_UBO;
		}
		do_compute_shader(ctx, CS_HADAMARD);
		do_compute_shader(ctx, CS_UFORCES);
		do_compute_shader(ctx, CS_MIN_MAX);
		ctx->flags &= ~DO_COMPUTE;
	}

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

	/* NOTE: Draw UI */
	BeginDrawing();
		ClearBackground(ctx->bg);

		Texture *output   = &ctx->fsctx.output.texture;

		v2 output_dim = {
			.x = ctx->params->output_max_xz.x - ctx->params->output_min_xz.x,
			.y = ctx->params->output_max_xz.y - ctx->params->output_min_xz.y,
		};

		f32 aspect_ratio = output_dim.x / output_dim.y;

		v2 line_step_mm = {.x = 3, .y = 5};
		uv2 line_count  = {
			.x = output_dim.x * 1e3/line_step_mm.x + 1,
			.y = output_dim.y * 1e3/line_step_mm.y + 1,
		};

		s8 txt = s8alloc(&arena, 64);
		snprintf((char *)txt.data, txt.len, "%+0.01f mm", -88.8f);
		v2 txt_s = {.rl = MeasureTextEx(ctx->font, (char *)txt.data,
		                                ctx->font_size, ctx->font_spacing)};

		/* NOTE: start this on far right and add space for scale-bar text */
		f32 pad = txt_s.x + 80;

		v2 view_size = (v2){
			.x = ((f32)ctx->window_size.h - pad) * aspect_ratio,
			.y = ((f32)ctx->window_size.h - pad),
		};
		v2 view_pos = (v2){
			.x = ((f32)ctx->window_size.w - view_size.x) - pad + 40,
			.y = txt_s.y,
		};

		/* NOTE: Horizontal Scale Bar */
		{
			f32 x_inc    = view_size.x / (line_count.x - 1);
			v2 start_pos = {
				.x = view_pos.x,
				.y = view_pos.y + view_size.y,
			};

			f32 x_mm     = ctx->params->output_min_xz.x * 1e3;
			f32 x_mm_inc = x_inc * output_dim.x * 1e3 / view_size.x;

			v2 end_pos  = start_pos;
			end_pos.y  += 20;

			v2 txt_pos  = end_pos;
			txt_pos.y  += 10;
			txt_pos.x   += txt_s.y/2;

			for (u32 i = 0 ; i < line_count.x; i++) {
				DrawLineEx(start_pos.rl, end_pos.rl, 3, ctx->fg);
				snprintf((char *)txt.data, txt.len, "%+0.01f mm", x_mm);
				DrawTextPro(ctx->font, (char *)txt.data, txt_pos.rl, (Vector2){0},
				            90, ctx->font_size, ctx->font_spacing, ctx->fg);
				start_pos.x += x_inc;
				end_pos.x   += x_inc;
				txt_pos.x   += x_inc;
				x_mm        += x_mm_inc;
			}
		}

		/* NOTE: Vertical Scale Bar */
		{
			f32 y_inc    = view_size.y / (line_count.y - 1);
			v2 start_pos = {
				.x = view_pos.x + view_size.x,
				.y = view_pos.y,
			};

			f32 y_mm     = ctx->params->output_min_xz.y * 1e3;
			f32 y_mm_inc = y_inc * output_dim.y * 1e3 / view_size.y;

			v2 end_pos  = start_pos;
			end_pos.x  += 20;

			v2 txt_pos  = end_pos;
			txt_pos.x  += 10;
			txt_pos.y   -= txt_s.y/2;

			for (u32 i = 0 ; i < line_count.y; i++) {
				DrawLineEx(start_pos.rl, end_pos.rl, 3, ctx->fg);
				snprintf((char *)txt.data, txt.len, "%0.01f mm", y_mm);
				DrawTextEx(ctx->font, (char *)txt.data, txt_pos.rl,
				           ctx->font_size, ctx->font_spacing, ctx->fg);
				start_pos.y += y_inc;
				end_pos.y   += y_inc;
				txt_pos.y   += y_inc;
				y_mm        += y_mm_inc;
			}
		}

		Rectangle view_r  = { view_pos.x, view_pos.y, view_size.x, view_size.y };
		Rectangle tex_r   = { 0.0f, 0.0f, (f32)output->width, -(f32)output->height };
		NPatchInfo tex_np = { tex_r, 0, 0, 0, 0, NPATCH_NINE_PATCH };
		DrawTextureNPatch(*output, tex_np, view_r, (Vector2){0}, 0, WHITE);

		/* NOTE: check mouse wheel for adjusting dynamic range of image */
		v2 mouse = { .rl = GetMousePosition() };
		if (CheckCollisionPointRec(mouse.rl, view_r)) {
			ctx->fsctx.db += GetMouseWheelMove();
			CLAMP(ctx->fsctx.db, -120, 0);
		};

		v2 ui_upper_left   = {.x = 10, .y = 10};
		v2 ui_bottom_right = {
			.x = ui_upper_left.x + view_pos.x - 30,
			.y = (f32)ctx->window_size.h - 10,
		};
		draw_settings_ui(ctx, arena, dt, ui_upper_left, ui_bottom_right);
		draw_debug_overlay(ctx, arena, dt);
	EndDrawing();

	if (IsKeyPressed(KEY_R))
		ctx->flags |= RELOAD_SHADERS;
}
