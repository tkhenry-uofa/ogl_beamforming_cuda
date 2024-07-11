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

static Color
colour_from_normalized(v4 rgba)
{
	return (Color){.r = rgba.r * 255.0f, .g = rgba.g * 255.0f,
	               .b = rgba.b * 255.0f, .a = rgba.a * 255.0f};
}

static f32
move_towards_f32(f32 current, f32 target, f32 delta)
{
	if (target < current) {
		current -= delta;
		if (current < target)
			current = target;
	} else {
		current += delta;
		if (current > target)
			current = target;
	}
	return current;
}

static v4
move_towards_v4(v4 current, v4 target, v4 delta)
{
	current.x = move_towards_f32(current.x, target.x, delta.x);
	current.y = move_towards_f32(current.y, target.y, delta.y);
	current.z = move_towards_f32(current.z, target.z, delta.z);
	current.w = move_towards_f32(current.w, target.w, delta.w);
	return current;
}

static v4
fmsub_v4(v4 a, v4 b, v4 scale)
{
	return (v4){
		.x = scale.x * (a.x - b.x),
		.y = scale.y * (a.y - b.y),
		.z = scale.z * (a.z - b.z),
		.w = scale.w * (a.w - b.w),
	};
}

static void
draw_settings_ui(BeamformerCtx *ctx, Arena arena, f32 dt, Rect r, v2 mouse)
{
	struct listing {
		char *prefix;
		char *suffix;
		f32  *data;
		f32  data_scale;
		b32  editable;
	} listings[] = {
		{ "Sampling Rate:",  " [MHz]", &ctx->params->sampling_frequency, 1e-6, 0 },
		{ "Speed of Sound:", " [m/s]", &ctx->params->speed_of_sound,     1,    1 },
		{ "Min X Point:",    " [mm]",  &ctx->params->output_min_xz.x,    1e3,  1 },
		{ "Max X Point:",    " [mm]",  &ctx->params->output_max_xz.x,    1e3,  1 },
		{ "Min Z Point:",    " [mm]",  &ctx->params->output_min_xz.y,    1e3,  1 },
		{ "Max Z Point:",    " [mm]",  &ctx->params->output_max_xz.y,    1e3,  1 },
		{ "Dynamic Range:",  " [dB]",  &ctx->fsctx.db,                   1,    1 },
	};

	struct { f32 min, max; } limits[] = {
		{0},
		{0,                                   1e6},
		{-1e3,                                ctx->params->output_max_xz.x - 1e-6},
		{ctx->params->output_min_xz.x + 1e-6, 1e3},
		{0,                                   ctx->params->output_max_xz.y - 1e-6},
		{ctx->params->output_min_xz.y + 1e-6, 1e3},
		{-120, 0},
	};

	static b32 init = 1;
	static v4 colours[ARRAY_COUNT(listings)];
	f32 scale = 6;
	v4 scaled_dt = (v4){.x = scale * dt, .y = scale * dt, .z = scale * dt, .w = scale * dt};
	v4 delta = fmsub_v4(ctx->fg, ctx->hovered_colour, scaled_dt);
	if (init) {
		for (i32 i = 0; i < ARRAY_COUNT(colours); i++)
			colours[i] = ctx->fg;
		init = 0;
	}

	static char focus_buf[64];
	static i32 focus_buf_curs = 0;
	static i32 focused_idx    = -1;
	i32 overlap_idx           = -1;

	f32 line_pad  = 10;

	v2 pos  = r.pos;
	pos.y  += 50;
	pos.x  += 10;

	s8 txt  = s8alloc(&arena, 64);

	for (i32 i = 0; i < ARRAY_COUNT(listings); i++) {
		struct listing *l = listings + i;
		DrawTextEx(ctx->font, l->prefix, pos.rl, ctx->font_size, ctx->font_spacing,
		           colour_from_normalized(ctx->fg));

		if (i == focused_idx) snprintf((char *)txt.data, txt.len, "%s", focus_buf);
		else                  snprintf((char *)txt.data, txt.len, "%0.02f", *l->data * l->data_scale);

		v2 suffix_s = {.rl = MeasureTextEx(ctx->font, l->suffix, ctx->font_size,
		                                   ctx->font_spacing)};
		v2 txt_s    = {.rl = MeasureTextEx(ctx->font, (char *)txt.data, ctx->font_size,
		                                   ctx->font_spacing)};

		v2 rpos  = {.x = r.pos.x + r.size.w - txt_s.w - suffix_s.w, .y = pos.y};

		Rectangle edit_rect = {rpos.x, rpos.y, txt_s.x, txt_s.y};
		if (CheckCollisionPointRec(mouse.rl, edit_rect) && l->editable) {
			overlap_idx = i;
			f32 mouse_scroll = GetMouseWheelMove();
			if (mouse_scroll) {
				*l->data += mouse_scroll / l->data_scale;
				CLAMP(*l->data, limits[i].min, limits[i].max);
				ctx->flags |= UPLOAD_UBO|DO_COMPUTE;
			}
		}

		Color tcol;
		if (i == overlap_idx)
			colours[i] = move_towards_v4(colours[i], ctx->hovered_colour, delta);
		else
			colours[i] = move_towards_v4(colours[i], ctx->fg, delta);

		if (i == focused_idx)
			tcol = colour_from_normalized(ctx->focused_colour);
		else
			tcol = colour_from_normalized(colours[i]);

		DrawTextEx(ctx->font, (char *)txt.data, rpos.rl, ctx->font_size,
		           ctx->font_spacing, tcol);

		rpos.x += txt_s.x;
		DrawTextEx(ctx->font, l->suffix, rpos.rl, ctx->font_size, ctx->font_spacing,
		           colour_from_normalized(ctx->fg));
		pos.y += txt_s.y + line_pad;
	}

	b32 save_focus = IsKeyPressed(KEY_ENTER) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
	if (save_focus && focused_idx != -1) {
		f32 new_val = strtof(focus_buf, NULL);
		/* TODO: allow zero for certain listings only */
		if (new_val != 0) {
			*listings[focused_idx].data = new_val / listings[focused_idx].data_scale;
			CLAMP(*listings[focused_idx].data, limits[focused_idx].min,
			      limits[focused_idx].max);
			ctx->flags |= UPLOAD_UBO|DO_COMPUTE;
		}
		focused_idx  = -1;
		focus_buf[0] = 0;
	}

	if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
		focused_idx = overlap_idx;
		if (focused_idx != -1) {
			f32 val = *listings[overlap_idx].data * listings[overlap_idx].data_scale;
			focus_buf_curs = snprintf(focus_buf, sizeof(focus_buf), "%0.02f", val);
		}
	}

	if (overlap_idx != -1) SetMouseCursor(MOUSE_CURSOR_IBEAM);
	else                   SetMouseCursor(MOUSE_CURSOR_DEFAULT);

	if (focused_idx == -1)
		return;

	i32 key = GetCharPressed();
	while (key > 0) {
		if (focus_buf_curs == (sizeof(focus_buf) - 1)) {
			focus_buf[focus_buf_curs] = 0;
			break;
		}

		if ((key >= '0' && key <= '9') || key == '.')
			focus_buf[focus_buf_curs++] = key;

		key = GetCharPressed();
	}

	if (IsKeyPressed(KEY_BACKSPACE) && focus_buf_curs > 0)
		focus_buf[--focus_buf_curs] = 0;
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
		DrawTextEx(ctx->font, (char *)partial_txt.data, pos.rl,  fontsize, fontspace,
		           colour_from_normalized(ctx->fg));
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
		ctx->window_size.h = GetScreenHeight();
		ctx->window_size.w = GetScreenWidth();
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
		ClearBackground(PINK);
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
		ClearBackground(colour_from_normalized(ctx->bg));

		Texture *output   = &ctx->fsctx.output.texture;

		v2 output_dim = {
			.x = ctx->params->output_max_xz.x - ctx->params->output_min_xz.x,
			.y = ctx->params->output_max_xz.y - ctx->params->output_min_xz.y,
		};

		v2 line_step_mm = {.x = 3, .y = 5};
		uv2 line_count  = {
			.x = output_dim.x * 1e3/line_step_mm.x + 1,
			.y = output_dim.y * 1e3/line_step_mm.y + 1,
		};

		s8 txt = s8alloc(&arena, 64);
		snprintf((char *)txt.data, txt.len, "%+0.01f mm", -88.8f);
		v2 txt_s = {.rl = MeasureTextEx(ctx->font, (char *)txt.data,
		                                ctx->font_size, ctx->font_spacing)};

		Rect wr = {.size = {.w = (f32)ctx->window_size.w, .h = (f32)ctx->window_size.h}};
		Rect lr = wr, rr = wr;
		lr.size.w = 420;
		rr.size.w = wr.size.w - lr.size.w - 10;
		rr.pos.x  = lr.pos.x  + lr.size.w + 10;

		f32 tick_len = 20;
		f32 x_pad    = txt_s.x + tick_len;
		f32 y_pad    = 1.5 * txt_s.x + tick_len;

		Rect vr    = rr;
		vr.size.h -= y_pad;
		vr.size.w  = vr.size.h * output_dim.w / output_dim.h;
		if (vr.size.w + x_pad > rr.size.w) {
			vr.size.h = (rr.size.w - x_pad) * output_dim.h / output_dim.w;
			vr.size.w = vr.size.h * output_dim.w / output_dim.h;
		}
		vr.pos.x += (rr.size.w - (vr.size.w + x_pad)) / 2;
		vr.pos.y += (rr.size.h - (vr.size.h + y_pad) + txt_s.h) / 2;

		Rectangle tex_r   = { 0.0f, 0.0f, (f32)output->width, -(f32)output->height };
		NPatchInfo tex_np = { tex_r, 0, 0, 0, 0, NPATCH_NINE_PATCH };
		DrawTextureNPatch(*output, tex_np, vr.rl, (Vector2){0}, 0, WHITE);

		/* NOTE: Horizontal Scale Bar */
		{
			f32 x_inc     = vr.size.w / (line_count.x - 1);
			v2 start_pos  = vr.pos;
			start_pos.y  += vr.size.h;

			f32 x_mm     = ctx->params->output_min_xz.x * 1e3;
			f32 x_mm_inc = x_inc * output_dim.x * 1e3 / vr.size.w;

			v2 end_pos  = start_pos;
			end_pos.y  += tick_len;

			v2 txt_pos  = end_pos;
			txt_pos.y  += 10;
			txt_pos.x  += txt_s.y/2;

			for (u32 i = 0 ; i < line_count.x; i++) {
				DrawLineEx(start_pos.rl, end_pos.rl, 3, colour_from_normalized(ctx->fg));
				snprintf((char *)txt.data, txt.len, "%+0.01f mm", x_mm);
				DrawTextPro(ctx->font, (char *)txt.data, txt_pos.rl, (Vector2){0},
				            90, ctx->font_size, ctx->font_spacing,
				            colour_from_normalized(ctx->fg));
				start_pos.x += x_inc;
				end_pos.x   += x_inc;
				txt_pos.x   += x_inc;
				x_mm        += x_mm_inc;
			}
		}

		/* NOTE: Vertical Scale Bar */
		{
			f32 y_inc     = vr.size.h / (line_count.y - 1);
			v2 start_pos  = vr.pos;
			start_pos.x  += vr.size.w;

			f32 y_mm     = ctx->params->output_min_xz.y * 1e3;
			f32 y_mm_inc = y_inc * output_dim.y * 1e3 / vr.size.h;

			v2 end_pos  = start_pos;
			end_pos.x  += tick_len;

			v2 txt_pos  = end_pos;
			txt_pos.x  += 10;
			txt_pos.y  -= txt_s.y/2;

			for (u32 i = 0 ; i < line_count.y; i++) {
				DrawLineEx(start_pos.rl, end_pos.rl, 3, colour_from_normalized(ctx->fg));
				snprintf((char *)txt.data, txt.len, "%0.01f mm", y_mm);
				DrawTextEx(ctx->font, (char *)txt.data, txt_pos.rl,
				           ctx->font_size, ctx->font_spacing,
				           colour_from_normalized(ctx->fg));
				start_pos.y += y_inc;
				end_pos.y   += y_inc;
				txt_pos.y   += y_inc;
				y_mm        += y_mm_inc;
			}
		}

		f32 desired_width = lr.size.w + vr.size.w;
		if (desired_width > ctx->window_size.w) {
			ctx->window_size.w = desired_width;
			SetWindowSize(ctx->window_size.w, ctx->window_size.h);
			SetWindowMinSize(desired_width, 960);
		}

		/* NOTE: check mouse wheel for adjusting dynamic range of image */
		v2 mouse = { .rl = GetMousePosition() };
		if (CheckCollisionPointRec(mouse.rl, vr.rl)) {
			ctx->fsctx.db += GetMouseWheelMove();
			CLAMP(ctx->fsctx.db, -120, 0);
		}

		draw_settings_ui(ctx, arena, dt, lr, mouse);
		draw_debug_overlay(ctx, arena, dt);
	EndDrawing();

	if (IsKeyPressed(KEY_R))
		ctx->flags |= RELOAD_SHADERS;
}
